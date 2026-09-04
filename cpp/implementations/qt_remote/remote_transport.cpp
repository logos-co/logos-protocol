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
#include <QLocalSocket>
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

class RemoteLogosObject : public LogosObject, public LogosObjectErrorChannel {
public:
    // objectName is carried purely so a failure can name the module it belongs
    // to — logos::CallError::origin, the same field the acquire-time error and
    // lp_invoke's out_error_json already fill in.
    RemoteLogosObject(QObject* replica, QString objectName)
        : m_replica(replica), m_helper(nullptr), m_objectName(std::move(objectName))
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
                                [cb = std::move(cb), result]() {
                                    cb(result, logos::CallError{});
                                });
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

    // Adapter over the error-carrying implementation: discards the diagnosis,
    // which is exactly what this entry point has always done.
    QVariant callMethod(const QString& authToken,
                        const QString& methodName,
                        const QVariantList& args,
                        int timeoutMs) override
    {
        return callMethodWithError(authToken, methodName, args, timeoutMs, nullptr);
    }

    QVariant callMethodWithError(const QString& authToken,
                                 const QString& methodName,
                                 const QVariantList& args,
                                 int timeoutMs,
                                 logos::CallError* err) override
    {
        if (err) err->clear();
        const std::string origin = m_objectName.toStdString();
        const std::string method = methodName.toStdString();

        if (!m_replica) {
            qWarning() << "RemoteLogosObject: Cannot call method on null replica";
            if (err)
                *err = logos::callErrorObjectUnavailable(
                    origin, "replica for '" + origin + "' is gone (module unloaded"
                            " or transport dropped)");
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
            if (err)
                *err = logos::callErrorCallFailed(
                    origin, "replica did not accept callRemoteMethod for '"
                            + method + "'");
            return QVariant();
        }

        pendingCall.waitForFinished(timeoutMs);

        // Two distinct outcomes that used to collapse into one empty QVariant:
        // the deadline elapsed with the call still in flight (timeout), and QtRO
        // itself failed the call (transport).
        if (!pendingCall.isFinished()) {
            qWarning() << "RemoteLogosObject: callRemoteMethod timed out";
            if (err) *err = logos::callErrorTimeout(origin, method, timeoutMs);
            return QVariant();
        }
        if (pendingCall.error() != QRemoteObjectPendingCall::NoError) {
            qWarning() << "RemoteLogosObject: callRemoteMethod failed:" << pendingCall.error();
            if (err)
                *err = logos::callErrorTransport(
                    origin, "QtRO call to '" + origin + "." + method
                            + "' failed with error "
                            + std::to_string(static_cast<int>(pendingCall.error())));
            return QVariant();
        }

        // A "multi" provider may have deferred the result (returned a pending
        // sentinel); resolveDeferred waits for the completion event, or returns
        // the value unchanged for an ordinary (synchronous) result.
        return resolveDeferred(pendingCall.returnValue(), timeoutMs, methodName, err);
    }

    // Adapter over the error-carrying implementation: discards the diagnosis,
    // which is exactly what this entry point has always done.
    void callMethodAsync(const QString& authToken,
                         const QString& methodName,
                         const QVariantList& args,
                         int timeoutMs,
                         AsyncResultCallback callback) override
    {
        if (!callback) return;
        callMethodAsyncWithError(authToken, methodName, args, timeoutMs,
            [cb = std::move(callback)](QVariant v, const logos::CallError&) mutable {
                cb(std::move(v));
            });
    }

    void callMethodAsyncWithError(const QString& authToken,
                                  const QString& methodName,
                                  const QVariantList& args,
                                  int timeoutMs,
                                  AsyncResultErrorCallback callback) override
    {
        if (!callback) return;
        const std::string origin = m_objectName.toStdString();
        const std::string method = methodName.toStdString();

        if (!m_replica) {
            const logos::CallError e = logos::callErrorObjectUnavailable(
                origin, "replica for '" + origin + "' is gone (module unloaded"
                        " or transport dropped)");
            QTimer::singleShot(0, [callback, e]() { callback(QVariant(), e); });
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
            const logos::CallError e = logos::callErrorCallFailed(
                origin, "replica did not accept callRemoteMethod for '" + method + "'");
            QTimer::singleShot(0, [callback, e]() { callback(QVariant(), e); });
            return;
        }

        auto* watcher = new QRemoteObjectPendingCallWatcher(pendingCall);

        // Timeout timer -- parented to the watcher so it is auto-deleted
        // when the watcher is destroyed, preventing late timeout callbacks.
        auto* timer = new QTimer(watcher);
        timer->setSingleShot(true);

        // Exactly-once gate. The finished handler and the timeout timer can
        // both be queued around the same moment; without a guard that races
        // into a double callback, which violates callMethodAsyncWithError's
        // contract and is a latent double-free for every consumer. The same
        // gate wraps the deferred-completion path so a late completion cannot
        // deliver after the initial timeout already has (or vice versa).
        auto delivered = std::make_shared<std::atomic_bool>(false);
        AsyncResultErrorCallback deliverOnce =
            [delivered, callback = std::move(callback)](QVariant result,
                                                       const logos::CallError& err) mutable {
                if (delivered->exchange(true))
                    return;
                if (callback)
                    callback(std::move(result), err);
            };

        // Success handler -- delivers result on the consumer's thread
        QObject::connect(watcher, &QRemoteObjectPendingCallWatcher::finished,
                         watcher, [this, deliverOnce, timer, timeoutMs, origin, method, delivered](QRemoteObjectPendingCallWatcher* w) {
            timer->stop(); // cancel timeout
            // Timeout may already have won the race and deleteLater'd us; if
            // the slot still runs, do not enter the deferred-completion path
            // or we would arm a second delivery after the caller already saw
            // a timeout.
            if (delivered->load()) {
                w->deleteLater();
                return;
            }
            QVariant result;
            logos::CallError err;
            if (w->error() == QRemoteObjectPendingCall::NoError) {
                result = w->returnValue();
            } else {
                qWarning() << "RemoteLogosObject: async callMethod error:" << w->error();
                err = logos::callErrorTransport(
                    origin, "QtRO call to '" + origin + "." + method
                            + "' failed with error "
                            + std::to_string(static_cast<int>(w->error())));
            }
            w->deleteLater();
            // A "multi" provider may have deferred the result: wait for the
            // completion event instead of delivering the pending sentinel.
            if (err.ok()) {
                QString callId;
                if (logos::isPendingCallSentinel(result, &callId)) {
                    if (m_completions.contains(callId)) {
                        deliverOnce(m_completions.take(callId), logos::CallError{});
                        return;
                    }
                    m_asyncCompletionCbs.insert(callId, deliverOnce);
                    // Bound the wait: a completion that never lands is a timeout,
                    // reported as one instead of as an empty result.
                    QTimer::singleShot(timeoutMs, m_helper, [this, callId, origin, method, timeoutMs]() {
                        if (m_asyncCompletionCbs.contains(callId)) {
                            auto cb = m_asyncCompletionCbs.take(callId);
                            if (cb)
                                cb(QVariant(),
                                   logos::callErrorTimeout(origin, method, timeoutMs));
                        }
                    });
                    return;
                }
            }
            deliverOnce(result, err);
        }, Qt::QueuedConnection);

        // Timeout handler -- stops the watcher and reports the elapsed deadline
        QObject::connect(timer, &QTimer::timeout, watcher, [watcher, deliverOnce, origin, method, timeoutMs]() {
            qWarning() << "RemoteLogosObject: async callMethod timed out";
            deliverOnce(QVariant(), logos::callErrorTimeout(origin, method, timeoutMs));
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
    QVariant resolveDeferred(const QVariant& rv, int timeoutMs,
                             const QString& methodName = QString(),
                             logos::CallError* err = nullptr)
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
        if (err)
            *err = logos::callErrorTimeout(m_objectName.toStdString(),
                                           methodName.toStdString(),
                                           timeoutMs > 0 ? timeoutMs : 30000);
        return QVariant();
    }

    QObject* m_replica;
    RemoteEventHelper* m_helper;
    // Deferred ("multi") completions delivered over the event channel, keyed by
    // callId: buffered results + sync (QEventLoop) and async (callback) waiters.
    // Touched only on the consumer event-loop thread.
    QHash<QString, QVariant> m_completions;
    QHash<QString, QEventLoop*> m_completionWaiters;
    QHash<QString, AsyncResultErrorCallback> m_asyncCompletionCbs;
    QString m_objectName;
};

// ── PendingAcquire ───────────────────────────────────────────────────────────

namespace {

// One outstanding non-blocking acquire.
//
// Acquiring a dynamic replica before the peer exists is legal and free:
// QRemoteObjectNodePrivate::handleNewAcquire registers the replica in the
// node's `replicas` map with no connection, and when the peer finally shows up
// the ObjectList packet handler calls handleReplicaConnection for every waiting
// replica, which drives the replica to Valid. There is no polling here — the
// node's own 250 ms reconnect timer (onShouldReconnect) is running for this
// endpoint regardless of whether anyone is waiting on it.
//
// Deletes itself after delivering exactly once. If it is destroyed before
// delivering (transport torn down), it owns and deletes the replica.
class PendingAcquire : public QObject {
    Q_OBJECT
public:
    PendingAcquire(QRemoteObjectReplica* replica,
                   const QString& objectName,
                   LogosTransportAsyncAcquire::AcquireCallback cb,
                   QObject* parent)
        : QObject(parent)
        , m_replica(replica)
        , m_objectName(objectName)
        , m_cb(std::move(cb))
    {
        connect(m_replica, &QRemoteObjectReplica::stateChanged,
                this, &PendingAcquire::onStateChanged);
        // Already live (module was up before we asked) — still deliver via the
        // event loop, so the callback contract is "never synchronously".
        if (m_replica->state() == QRemoteObjectReplica::Valid)
            schedule(true);
    }

    ~PendingAcquire() override
    {
        // Only reached when we never handed the replica over.
        delete m_replica;
    }

private slots:
    void onStateChanged(QRemoteObjectReplica::State state,
                        QRemoteObjectReplica::State /*old*/)
    {
        if (m_done) return;
        if (state == QRemoteObjectReplica::Valid) {
            schedule(true);
        } else if (state == QRemoteObjectReplica::SignatureMismatch) {
            // Terminal: handleReplicaConnection warns and never connects, and
            // no further state change will arrive. Report it rather than
            // sitting Pending forever in silence — a subscription that can
            // never arm has to SAY so.
            qWarning() << "RemoteTransportConnection: source signature mismatch for"
                       << m_objectName << "-- this acquire can never succeed";
            schedule(false);
        }
        // Suspect / Default are NOT terminal: the source can come back and the
        // node will re-drive the replica to Valid.
    }

private:
    void schedule(bool ok)
    {
        if (m_done) return;
        m_done = true;
        // NEVER deliver inline. stateChanged is emitted from inside
        // QRemoteObjectNodePrivate::onClientRead; constructing a
        // RemoteLogosObject (which connects to the same replica) and then
        // running caller code there re-enters QtRO while the replica is still
        // emitting. That is precisely the refresh_balances SIGSEGV documented
        // on RemoteLogosObject::disconnectEvents(). Hop to the next event-loop
        // turn; `this` as context cancels the call if we are destroyed first.
        QTimer::singleShot(0, this, [this, ok]() { deliver(ok); });
    }

    void deliver(bool ok)
    {
        QRemoteObjectReplica* replica = m_replica;
        m_replica = nullptr;                       // ownership leaves us either way
        disconnect(replica, nullptr, this, nullptr);

        LogosObject* obj = nullptr;
        if (ok) {
            g_acquireCount.fetch_add(1, std::memory_order_relaxed);
            obj = new RemoteLogosObject(replica, m_objectName);
        } else {
            delete replica;
        }
        auto cb = std::move(m_cb);
        deleteLater();
        cb(obj);                                   // may re-enter the transport safely
    }

    QRemoteObjectReplica* m_replica;
    QString m_objectName;
    LogosTransportAsyncAcquire::AcquireCallback m_cb;
    bool m_done = false;
};

} // anonymous namespace

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
    , m_pendingAcquires(new QObject())
    , m_registryUrl(registryUrl)
    , m_connected(false)
{
}

RemoteTransportConnection::~RemoteTransportConnection()
{
    // Pending replicas and parked probes belong to the node; kill them FIRST so
    // none outlives it.
    m_probes.clear();
    delete m_pendingAcquires;
    m_pendingAcquires = nullptr;
    delete m_node;
}

bool RemoteTransportConnection::connectToHost()
{
    return connectToRegistry();
}

bool RemoteTransportConnection::endpointHasListener() const
{
    const QUrl url(m_registryUrl);
    // Only `local:` can be probed cheaply and without side effects worth
    // worrying about. Anything else keeps the previous semantics.
    if (url.scheme() != QLatin1String("local"))
        return true;

    const QString serverName = url.path().isEmpty() ? url.host() : url.path();
    if (serverName.isEmpty())
        return true;

    // A client connect is exactly what QtRO itself does, so a transient one is
    // benign against a live registry, and against a dead endpoint it fails
    // immediately -- ERROR_FILE_NOT_FOUND on Windows, ENOENT/ECONNREFUSED on
    // Unix. That is the whole point: cost is microseconds when the answer is
    // "no", which is the case that currently costs 20 seconds.
    QLocalSocket probe;
    probe.connectToServer(serverName, QIODevice::ReadOnly);
    const bool alive = probe.waitForConnected(250);
    probe.abort();
    return alive;
}

bool RemoteTransportConnection::isConnected() const
{
    // m_connected alone is NOT an answer. QRemoteObjectNode::connectToNode()
    // returns false only when the URL scheme is unregistered -- it never
    // contacts the peer -- and our registry URLs are COMPUTED rather than
    // discovered (logos_instance.h: local:logos_<module>_<instanceId>), so they
    // are identical whether or not the module exists. Returning the raw latch
    // made every `if (!client->isConnected()) return;` guard in the codebase
    // dead code, and callers then paid a 20 s waitForSource per call against
    // modules that were never loaded -- measured at ~417 s of blocked GUI
    // thread in Basecamp on macOS, 361 s on Linux, before its window appeared.
    if (!m_connected)
        return false;
    return endpointHasListener();
}

bool RemoteTransportConnection::reconnect()
{
    qDebug() << "RemoteTransportConnection: Attempting to reconnect to registry:" << m_registryUrl;

    if (m_connected) {
        // In-flight acquires hold replicas belonging to the node about to die.
        // Drop them first; their callbacks are cancelled with them, so the
        // consumer's subscription registry re-arms against the new node
        // (LogosAPIConsumer::reconnect -> reconnected()).
        m_probes.clear();
        delete m_pendingAcquires;
        m_pendingAcquires = new QObject();
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
        // Deliberately NOT "Successfully connected". connectToNode() only
        // accepted the URL scheme; it never contacted a peer, and this line
        // previously asserted a connection that frequently did not exist. That
        // false claim sent three separate investigations to the wrong place --
        // it cost considerably more than the bug it hid.
        qDebug() << "RemoteTransportConnection: Registry connect attempt started (no peer contact yet):"
                 << m_registryUrl;
    } else {
        m_connected = false;
        qWarning() << "RemoteTransportConnection: Registry URL scheme rejected:" << m_registryUrl;
    }

    return m_connected;
}

LogosObject* RemoteTransportConnection::requestObject(const QString& objectName, int timeoutMs)
{
    if (!m_connected) {
        qWarning() << "RemoteTransportConnection: Not connected. Cannot request object:" << objectName;
        return nullptr;
    }

    // Warn BEFORE a doomed wait rather than after it. Without this the log went
    // silent for the full timeout and then reported failure, which reads as a
    // hang with no cause attached.
    if (!endpointHasListener()) {
        qWarning() << "RemoteTransportConnection: no listener at" << m_registryUrl
                   << "-- request for" << objectName << "will block up to" << timeoutMs
                   << "ms and then fail. Is the module loaded?";
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
    return new RemoteLogosObject(replica, objectName);
}

bool RemoteTransportConnection::requestObjectWhenAvailable(const QString& objectName,
                                                           AcquireCallback onReady)
{
    if (objectName.isEmpty() || !onReady) return false;
    if (!m_node || !m_pendingAcquires) return false;
    // m_connected is the raw "connectToNode() accepted the URL" latch, and here
    // that is exactly the right question: we are NOT asking whether the peer is
    // up (it deliberately need not be), only whether this node is wired to the
    // endpoint so QtRO's reconnect loop is running for it. Calling the truthful
    // isConnected() here would reintroduce the very bug this exists to fix.
    if (!m_connected) return false;

    QRemoteObjectReplica* replica = m_node->acquireDynamic(objectName);
    if (!replica) {
        qWarning() << "RemoteTransportConnection: Failed to acquire replica for:" << objectName;
        return false;
    }

    new PendingAcquire(replica, objectName, std::move(onReady), m_pendingAcquires);
    return true;
}

LogosObject* RemoteTransportConnection::tryAcquireNow(const QString& objectName)
{
    if (objectName.isEmpty() || !m_node) return nullptr;
    // Raw latch, same reasoning as requestObjectWhenAvailable: the question is
    // whether this node is wired to the endpoint, not whether the peer is up.
    if (!m_connected) return nullptr;

    // PARK the probe; never free it while it is unconfigured. QtRO shares one
    // replica implementation per object name on a node, and while that
    // implementation is still waiting for the source's metaobject it records
    // every facade built on it as a RAW pointer in m_parentsNeedingConnect.
    // ~QRemoteObjectReplica is an empty body, so destroying a facade does not
    // deregister it — and the implementation dereferences the whole list when
    // the class definition finally arrives.
    //
    // The earlier acquire-then-delete form was therefore a use-after-free that
    // left one dangling pointer per probe. It survived a single subscription
    // (the probe owned the only implementation and took it down with itself)
    // and crashed once a second subscription shared an implementation pinned by
    // an in-flight PendingAcquire: SIGBUS with BUS_ADRALN, in the consumer's
    // event loop rather than at the call site.
    //
    // Parking costs one idle replica per name until it goes Valid or the
    // connection dies. Parented to m_pendingAcquires, which both ~RemoteTransportConnection
    // and reconnect() destroy BEFORE the node — the ordering is what makes
    // freeing them safe, since the implementations die in the same breath.
    QPointer<QRemoteObjectReplica>& probe = m_probes[objectName];
    if (!probe) {
        QRemoteObjectReplica* fresh = m_node->acquireDynamic(objectName);
        if (!fresh) {
            m_probes.remove(objectName);
            return nullptr;
        }
        if (m_pendingAcquires) fresh->setParent(m_pendingAcquires);
        probe = fresh;
    }

    // Not Valid means "would have to be waited on", and waiting is exactly what
    // this must not do — requestObjectWhenAvailable owns that. Leave the probe
    // parked and answer no.
    if (probe->state() != QRemoteObjectReplica::Valid)
        return nullptr;

    // Valid: the implementation is configured, so it is no longer holding this
    // facade in m_parentsNeedingConnect and handing ownership over is safe.
    QRemoteObjectReplica* replica = probe;
    m_probes.remove(objectName);
    replica->setParent(nullptr);

    g_acquireCount.fetch_add(1, std::memory_order_relaxed);
    return new RemoteLogosObject(replica, objectName);
}

TargetPresence RemoteTransportConnection::targetPresence(const QString& objectName)
{
    if (objectName.isEmpty() || !m_node || !m_connected)
        return TargetPresence::Unknown;

    // Park a probe exactly as tryAcquireNow does, and for the same
    // use-after-free reason documented there — but never hand it over. Parking
    // is also what makes this cheap on repeat and self-correcting: the node
    // retries the endpoint every 250 ms for the life of the process, so a probe
    // for a module that arrives later goes Valid on its own.
    QPointer<QRemoteObjectReplica>& probe = m_probes[objectName];
    if (!probe) {
        QRemoteObjectReplica* fresh = m_node->acquireDynamic(objectName);
        if (!fresh) {
            m_probes.remove(objectName);
            return TargetPresence::Unknown;
        }
        if (m_pendingAcquires) fresh->setParent(m_pendingAcquires);
        probe = fresh;
    }

    // Valid means the source published and the metaobject arrived. Anything
    // else is "would have to be waited on", which is NOT the same as absent —
    // a module whose host is still starting looks identical to one that was
    // never installed, so answering Absent here would be a guess.
    return probe->state() == QRemoteObjectReplica::Valid
               ? TargetPresence::Present
               : TargetPresence::Unknown;
}

long RemoteTransportConnection::acquireCount() { return g_acquireCount.load(std::memory_order_relaxed); }
void RemoteTransportConnection::resetAcquireCount() { g_acquireCount.store(0, std::memory_order_relaxed); }

#include "remote_transport.moc"
