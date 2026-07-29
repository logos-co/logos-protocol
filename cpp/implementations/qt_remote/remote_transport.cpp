#include "remote_transport.h"
#include "../../logos_async_dispatch.h"
#include "../../logos_socket_paths.h"
#include "qt_socket_path.h"
#include <QRemoteObjectRegistryHost>
#include <QRemoteObjectNode>
#include <QRemoteObjectReplica>
#include <QRemoteObjectPendingCall>
#include <QRemoteObjectPendingCallWatcher>
#include <QTimer>
#include <QEventLoop>
#include <QDebug>
#include <QUrl>
#include <QMetaObject>
#include <QTime>
#include <QJsonArray>
#include <QVariantMap>
#include <atomic>

// Process-wide count of replicas acquired by requestObject() — a test hook to
// prove the consumer reuses one cached handle instead of re-acquiring per call.
static std::atomic<long> g_acquireCount{0};

using logos::qtremote::localSocketFilePath;

// ── RemoteLogosObject ────────────────────────────────────────────────────────

namespace {

class RemoteEventHelper : public QObject {
    Q_OBJECT
public:
    explicit RemoteEventHelper(QObject* parent = nullptr) : QObject(parent) {}

    void addCallback(const QString& eventName, LogosObject::EventCallback cb) {
        m_callbacks[eventName].append(std::move(cb));
    }

public slots:
    void onEventResponse(const QString& eventName, const QVariantList& data) {
        // Dispatch to callbacks registered for this specific event name,
        // plus any wildcard subscribers (callbacks registered with an
        // empty event name, meaning "receive every event").
        auto cbs = m_callbacks.value(eventName);
        cbs.append(m_callbacks.value(QString()));
        if (!cbs.isEmpty()) {
            qDebug() << "[LogosObject] Remote EventHelper: dispatching event" << eventName << "to" << cbs.size() << "callback(s) (via IPC)";
        }
        for (const auto& cb : cbs) {
            try { cb(eventName, data); } catch (...) {}
        }
    }

private:
    QHash<QString, QList<LogosObject::EventCallback>> m_callbacks;
};

} // anonymous namespace

class RemoteLogosObject : public LogosObject {
public:
    explicit RemoteLogosObject(QObject* replica)
        : m_replica(replica), m_helper(nullptr)
    {
        qDebug() << "[LogosObject] Created RemoteLogosObject wrapping QRemoteObjectReplica" << reinterpret_cast<quintptr>(replica);
        if (m_replica) {
            // Eager event wiring — a deferred ("multi") call's result arrives as a
            // completion event, so the channel must be live even when the caller
            // never subscribes to a user event. onEvent() reuses this same helper.
            m_helper = new RemoteEventHelper();
            QObject::connect(m_replica, SIGNAL(eventResponse(QString,QVariantList)),
                             m_helper, SLOT(onEventResponse(QString,QVariantList)));
            m_helper->addCallback(logos::callCompleteEvent(),
                [this](const QString&, const QVariantList& data) {
                    if (data.size() != 2) return;
                    const QString id = data.at(0).toString();
                    const QVariant result = data.at(1);
                    m_completions.insert(id, result);
                    if (QEventLoop* loop = m_completionWaiters.value(id, nullptr))
                        loop->quit();                       // wake a sync waiter
                    if (m_asyncCompletionCbs.contains(id)) {  // fire an async waiter
                        auto cb = m_asyncCompletionCbs.take(id);
                        m_completions.remove(id);
                        // Deliver on the NEXT event-loop turn, never inline. This
                        // runs from RemoteEventHelper::onEventResponse, which —
                        // cross-process — is on QtRO's onClientRead read stack. The
                        // user callback (LogosAPIConsumer's async lambda → the
                        // module's completion handler) routinely emits a module
                        // event (host ModuleProxy → QtRO source serialization) and
                        // then release()s this object. Doing either while
                        // onClientRead is still unwinding re-enters QtRO and
                        // corrupts the node — the refresh_balances SIGSEGV
                        // (KERN_INVALID_ADDRESS in onClientRead). Deferring runs the
                        // user code after the read fully unwinds. m_helper is the
                        // context so the callback is dropped if we're torn down
                        // first (mirrors the QPointer guard in invokeRemoteMethodAsync).
                        if (cb)
                            QTimer::singleShot(0, m_helper,
                                [cb = std::move(cb), result]() { cb(result); });
                    }
                });
        }
    }

    ~RemoteLogosObject() override {
        qDebug() << "[LogosObject] Destroying RemoteLogosObject" << reinterpret_cast<quintptr>(m_replica);
        // release() normally clears m_helper first (deferred). If we get here on a
        // direct delete, defer the helper too: a direct delete can still be reached
        // from within the helper's own slot dispatch. See disconnectEvents().
        if (m_helper) {
            if (m_replica)
                QObject::disconnect(m_replica, nullptr, m_helper, nullptr);
            m_helper->deleteLater();
            m_helper = nullptr;
        }
    }

    QVariant callMethod(const QString& authToken,
                        const QString& methodName,
                        const QVariantList& args,
                        int timeoutMs) override
    {
        if (!m_replica) {
            qWarning() << "RemoteLogosObject: Cannot call method on null replica";
            return QVariant();
        }
        qDebug() << "[LogosObject] RemoteLogosObject::callMethod" << methodName << "args:" << args.size();

        QRemoteObjectPendingCall pendingCall;
        bool success = QMetaObject::invokeMethod(
            m_replica,
            "callRemoteMethod",
            Qt::DirectConnection,
            Q_RETURN_ARG(QRemoteObjectPendingCall, pendingCall),
            Q_ARG(QString, authToken),
            Q_ARG(QString, methodName),
            Q_ARG(QVariantList, args)
        );

        if (!success) {
            qWarning() << "RemoteLogosObject: Failed to invoke callRemoteMethod on replica";
            return QVariant();
        }

        pendingCall.waitForFinished(timeoutMs);

        if (!pendingCall.isFinished() || pendingCall.error() != QRemoteObjectPendingCall::NoError) {
            qWarning() << "RemoteLogosObject: callRemoteMethod failed or timed out:" << pendingCall.error();
            return QVariant();
        }

        // A "multi" provider may have deferred the result (returned a pending
        // sentinel); resolveDeferred waits for the completion event, or returns
        // the value unchanged for an ordinary (synchronous) result.
        return resolveDeferred(pendingCall.returnValue(), timeoutMs);
    }

    void callMethodAsync(const QString& authToken,
                         const QString& methodName,
                         const QVariantList& args,
                         int timeoutMs,
                         AsyncResultCallback callback) override
    {
        if (!callback) return;
        if (!m_replica) {
            QTimer::singleShot(0, [callback]() { callback(QVariant()); });
            return;
        }

        qDebug() << "[LogosObject] RemoteLogosObject::callMethodAsync" << methodName << "args:" << args.size();

        QRemoteObjectPendingCall pendingCall;
        bool success = QMetaObject::invokeMethod(
            m_replica,
            "callRemoteMethod",
            Qt::DirectConnection,
            Q_RETURN_ARG(QRemoteObjectPendingCall, pendingCall),
            Q_ARG(QString, authToken),
            Q_ARG(QString, methodName),
            Q_ARG(QVariantList, args)
        );

        if (!success) {
            qWarning() << "RemoteLogosObject: Failed to invoke callRemoteMethod on replica (async)";
            QTimer::singleShot(0, [callback]() { callback(QVariant()); });
            return;
        }

        auto* watcher = new QRemoteObjectPendingCallWatcher(pendingCall);

        // Timeout timer -- parented to the watcher so it is auto-deleted
        // when the watcher is destroyed, preventing late timeout callbacks.
        auto* timer = new QTimer(watcher);
        timer->setSingleShot(true);

        // Success handler -- delivers result on the consumer's thread
        QObject::connect(watcher, &QRemoteObjectPendingCallWatcher::finished,
                         watcher, [this, callback, timer, timeoutMs](QRemoteObjectPendingCallWatcher* w) {
            timer->stop(); // cancel timeout
            QVariant result;
            if (w->error() == QRemoteObjectPendingCall::NoError) {
                result = w->returnValue();
            } else {
                qWarning() << "RemoteLogosObject: async callMethod error:" << w->error();
            }
            w->deleteLater();
            // A "multi" provider may have deferred the result: wait for the
            // completion event instead of delivering the pending sentinel.
            {
                QString callId;
                if (logos::isPendingCallSentinel(result, &callId)) {
                    if (m_completions.contains(callId)) { callback(m_completions.take(callId)); return; }
                    m_asyncCompletionCbs.insert(callId, callback);
                    // Bound the wait: deliver an empty result once if it never lands.
                    QTimer::singleShot(timeoutMs, m_helper, [this, callId]() {
                        if (m_asyncCompletionCbs.contains(callId)) {
                            auto cb = m_asyncCompletionCbs.take(callId);
                            if (cb) cb(QVariant());
                        }
                    });
                    return;
                }
            }
            callback(result);
        }, Qt::QueuedConnection);

        // Timeout handler -- stops the watcher and delivers empty result
        QObject::connect(timer, &QTimer::timeout, watcher, [watcher, callback]() {
            qWarning() << "RemoteLogosObject: async callMethod timed out";
            callback(QVariant());
            watcher->deleteLater(); // also destroys the timer (child)
        });

        timer->start(timeoutMs);
    }

    bool informModuleToken(const QString& authToken,
                           const QString& moduleName,
                           const QString& token,
                           int timeoutMs) override
    {
        if (!m_replica) {
            qWarning() << "RemoteLogosObject: Cannot call informModuleToken on null replica";
            return false;
        }

        QRemoteObjectPendingCall pendingCall;
        bool success = QMetaObject::invokeMethod(
            m_replica,
            "informModuleToken",
            Qt::DirectConnection,
            Q_RETURN_ARG(QRemoteObjectPendingCall, pendingCall),
            Q_ARG(QString, authToken),
            Q_ARG(QString, moduleName),
            Q_ARG(QString, token)
        );

        if (!success) {
            qWarning() << "RemoteLogosObject: Failed to invoke informModuleToken on replica";
            return false;
        }

        pendingCall.waitForFinished(timeoutMs);

        if (!pendingCall.isFinished() || pendingCall.error() != QRemoteObjectPendingCall::NoError) {
            qWarning() << "RemoteLogosObject: informModuleToken failed or timed out:" << pendingCall.error();
            return false;
        }

        return pendingCall.returnValue().toBool();
    }

    void onEvent(const QString& eventName, EventCallback callback) override
    {
        if (!m_replica) return;

        qDebug() << "[LogosObject] RemoteLogosObject::onEvent subscribing to event:" << eventName;
        if (!m_helper) {
            m_helper = new RemoteEventHelper();
            QObject::connect(m_replica, SIGNAL(eventResponse(QString,QVariantList)),
                             m_helper, SLOT(onEventResponse(QString,QVariantList)));
            qDebug() << "[LogosObject] RemoteLogosObject: connected EventHelper to QRemoteObjectReplica signals (IPC)";
        }
        m_helper->addCallback(eventName, std::move(callback));
    }

    void disconnectEvents() override
    {
        if (!m_helper) return;
        // Defer the helper's destruction instead of deleting it inline.
        //
        // release()/disconnectEvents() can be reached *synchronously from inside
        // the helper's own onEventResponse() slot*: a deferred ("multi") call's
        // result is delivered as a completion event that fires onEventResponse,
        // whose callback (LogosAPIConsumer::invokeRemoteMethodAsync) runs the
        // user callback and then calls plugin->release(). With the qt_remote
        // transport the event arrives cross-process, so onEventResponse runs on
        // the QtRO read stack (QRemoteObjectNodePrivate::onClientRead) while the
        // replica is still emitting. Deleting the helper (the signal receiver)
        // here — and the replica (the sender) in release() — corrupts the
        // connection list Qt is still iterating, a use-after-free that crashes in
        // QMetaObjectPrivate::signal / onClientRead (the refresh_balances SIGSEGV).
        //
        // Stop further dispatch now by disconnecting, then let the event loop
        // delete the helper once the current emission has fully unwound.
        if (m_replica)
            QObject::disconnect(m_replica, nullptr, m_helper, nullptr);
        m_helper->deleteLater();
        m_helper = nullptr;
    }

    void emitEvent(const QString& eventName, const QVariantList& data) override
    {
        if (!m_replica) return;
        qDebug() << "[LogosObject] RemoteLogosObject::emitEvent" << eventName << "data:" << data.size() << "items (via IPC)";
        QMetaObject::invokeMethod(m_replica, "eventResponse",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, eventName),
                                  Q_ARG(QVariantList, data));
    }

    QJsonArray getMethods() override
    {
        // Remote introspection not implemented — callers should use
        // the local module inspection tools (lm) instead.
        return QJsonArray();
    }

    void release() override
    {
        disconnectEvents();              // defers the helper (signal receiver)
        // The replica is the QtRO signal *sender* whose eventResponse() may be the
        // very emission that re-entered release() (a deferred completion event).
        // Deleting the sender mid-emit corrupts QtRO's read path
        // (QRemoteObjectNodePrivate::onClientRead) — defer it too. deleteLater also
        // keeps any QVariant return storage backed by the replica alive until the
        // consumer callback (which runs before this release) has consumed it.
        if (m_replica) {
            m_replica->deleteLater();
            m_replica = nullptr;
        }
        delete this;
    }

    quintptr id() const override { return reinterpret_cast<quintptr>(m_replica); }

    // Valid only while the underlying replica is synced to its source. When the
    // target module unloads, the source drops and state() leaves Valid, so a
    // cached handle knows to re-acquire instead of calling into a dead replica.
    bool isValid() const override
    {
        auto* r = qobject_cast<QRemoteObjectReplica*>(m_replica);
        return r && r->state() == QRemoteObjectReplica::Valid;
    }

private:
    // Resolve a possibly-deferred result. If `rv` is a pending sentinel from a
    // "multi" provider, wait (up to timeoutMs) for the completion event keyed by
    // callId, pumping the consumer event loop; otherwise return `rv` unchanged.
    QVariant resolveDeferred(const QVariant& rv, int timeoutMs)
    {
        QString callId;
        if (!logos::isPendingCallSentinel(rv, &callId)) return rv;
        // The completion can arrive during the call's own waitForFinished above.
        if (m_completions.contains(callId)) return m_completions.take(callId);
        QEventLoop loop;
        m_completionWaiters.insert(callId, &loop);
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(timeoutMs > 0 ? timeoutMs : 30000);
        loop.exec();
        m_completionWaiters.remove(callId);
        if (m_completions.contains(callId)) return m_completions.take(callId);
        qWarning() << "RemoteLogosObject: deferred call" << callId << "timed out";
        return QVariant();
    }

    QObject* m_replica;
    RemoteEventHelper* m_helper;
    // Deferred ("multi") completions delivered over the event channel, keyed by
    // callId: buffered results + sync (QEventLoop) and async (callback) waiters.
    // Touched only on the consumer event-loop thread.
    QHash<QString, QVariant> m_completions;
    QHash<QString, QEventLoop*> m_completionWaiters;
    QHash<QString, AsyncResultCallback> m_asyncCompletionCbs;
};

// ── RemoteTransportHost ──────────────────────────────────────────────────────

RemoteTransportHost::RemoteTransportHost(const QString& registryUrl)
    : m_registryHost(nullptr)
    , m_registryUrl(registryUrl)
{
}

RemoteTransportHost::~RemoteTransportHost()
{
    delete m_registryHost;
}

bool RemoteTransportHost::publishObject(const QString& name, QObject* object)
{
    if (!m_registryHost) {
        // Construct WITHOUT a URL and listen via setRegistryUrl() so we can
        // observe the result. The QUrl-taking ctor swallows the listen bool: on
        // a failed bind (stale socket, permission denied, sun_path overflow) it
        // leaves a half-built host that silently rejects every enableRemoting()
        // with OperationNotValidOnClientNode — the failure only surfaced as
        // clients hanging on a dead endpoint.
        auto* host = new QRemoteObjectRegistryHost();
        if (!host->setRegistryUrl(QUrl(m_registryUrl))) {
            qCritical() << "RemoteTransportHost: failed to listen on" << m_registryUrl
                        << "- error" << host->lastError()
                        << "- socket path" << localSocketFilePath(m_registryUrl);
            delete host;
            return false;
        }
        m_registryHost = host;

        // Make the freshly-bound socket reachable by a co-resident client per
        // the LOGOS_SOCKET_GROUP / LOGOS_SOCKET_MODE policy. No-op when those
        // env vars are unset (socket keeps its owner-only default). Only `local:`
        // registries have a socket file — a tcp:// one does not.
        if (QUrl(m_registryUrl).scheme() == QLatin1String("local")) {
            std::string permErr;
            if (!logos::applySocketPerms(
                    localSocketFilePath(m_registryUrl).toStdString(), &permErr)) {
                qWarning() << "RemoteTransportHost: could not apply socket perms:"
                           << QString::fromStdString(permErr);
            }
        }
        qDebug() << "RemoteTransportHost: Created registry host with URL:" << m_registryUrl;
    }

    bool success = m_registryHost->enableRemoting(object, name);
    if (success) {
        qDebug() << "RemoteTransportHost: Published object:" << name;
    } else {
        qCritical() << "RemoteTransportHost: Failed to publish object:" << name;
    }
    return success;
}

void RemoteTransportHost::unpublishObject(const QString& /*name*/)
{
}

// ── RemoteTransportConnection ────────────────────────────────────────────────

RemoteTransportConnection::RemoteTransportConnection(const QString& registryUrl)
    : m_node(new QRemoteObjectNode())
    , m_registryUrl(registryUrl)
    , m_connected(false)
{
}

RemoteTransportConnection::~RemoteTransportConnection()
{
    delete m_node;
}

bool RemoteTransportConnection::connectToHost()
{
    return connectToRegistry();
}

bool RemoteTransportConnection::isConnected() const
{
    return m_connected;
}

bool RemoteTransportConnection::reconnect()
{
    qDebug() << "RemoteTransportConnection: Attempting to reconnect to registry:" << m_registryUrl;

    if (m_connected) {
        delete m_node;
        m_node = new QRemoteObjectNode();
        m_connected = false;
    }

    return connectToRegistry();
}

bool RemoteTransportConnection::connectToRegistry()
{
    if (!m_node) {
        qWarning() << "RemoteTransportConnection: Remote object node is null";
        return false;
    }

    if (m_registryUrl.isEmpty()) {
        qWarning() << "RemoteTransportConnection: Registry URL is empty";
        return false;
    }

    qDebug() << "RemoteTransportConnection: Connecting to registry:" << m_registryUrl
             << "at" << QTime::currentTime().toString("hh:mm:ss.zzz");

    QUrl url(m_registryUrl);
    bool success = m_node->connectToNode(url);

    if (success) {
        m_connected = true;
        qDebug() << "RemoteTransportConnection: Successfully connected to registry:" << m_registryUrl;
    } else {
        m_connected = false;
        qWarning() << "RemoteTransportConnection: Failed to connect to registry:" << m_registryUrl;
    }
    qDebug() << "RemoteTransportConnection: Connected to registry at"
             << QTime::currentTime().toString("hh:mm:ss.zzz");

    return m_connected;
}

LogosObject* RemoteTransportConnection::requestObject(const QString& objectName, int timeoutMs)
{
    if (!m_connected) {
        qWarning() << "RemoteTransportConnection: Not connected. Cannot request object:" << objectName;
        return nullptr;
    }

    qDebug() << "RemoteTransportConnection: Requesting object:" << objectName
             << "at" << QTime::currentTime().toString("hh:mm:ss.zzz");

    QRemoteObjectReplica* replica = m_node->acquireDynamic(objectName);
    if (!replica) {
        qWarning() << "RemoteTransportConnection: Failed to acquire replica for:" << objectName;
        return nullptr;
    }

    if (!replica->waitForSource(timeoutMs)) {
        qWarning() << "RemoteTransportConnection: Timeout waiting for replica:" << objectName;
        delete replica;
        return nullptr;
    }

    qDebug() << "[LogosObject] RemoteTransportConnection: returning RemoteLogosObject for:" << objectName;
    g_acquireCount.fetch_add(1, std::memory_order_relaxed);
    return new RemoteLogosObject(replica);
}

long RemoteTransportConnection::acquireCount() { return g_acquireCount.load(std::memory_order_relaxed); }
void RemoteTransportConnection::resetAcquireCount() { g_acquireCount.store(0, std::memory_order_relaxed); }

#include "remote_transport.moc"
