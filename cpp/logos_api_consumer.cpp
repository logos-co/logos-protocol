#include "logos_api_consumer.h"
#include "logos_object.h"
#include "module_proxy.h"
#include "token_manager.h"
#include "logos_mode.h"
#include "logos_instance.h"
#include "logos_transport.h"
#include "logos_transport_factory.h"
#include <chrono>
#include <thread>
#include <QDebug>
#include <QUrl>
#include <QMetaObject>
#include <QTimer>
#include <QTime>
#include <QPointer>
#include <QElapsedTimer>
#include <QSet>
#include <QVector>

// ── LogosPendingSubscriptions ────────────────────────────────────────────────
//
// The registry behind LogosAPIConsumer::onEventWhenAvailable(). It holds every
// subscription whose target object is not reachable yet and arms it the moment
// it becomes reachable — see the contract on the header declaration.
//
// Defined here rather than in the header on purpose: all of its state is
// instance state reached through one owned pointer, so nothing about it can
// become a per-image duplicate on Windows.
class LogosPendingSubscriptions
{
public:
    LogosPendingSubscriptions(LogosAPIConsumer* owner,
                              LogosTransportConnection* transport)
        : m_owner(owner), m_transport(transport) {}

    ~LogosPendingSubscriptions()
    {
        if (m_timer) { m_timer->stop(); delete m_timer; }
        for (LogosObject* obj : m_handles)
            if (obj) obj->release();
        m_handles.clear();
    }

    void add(const QString& objectName, const QString& eventName,
             LogosObject::EventCallback cb, std::function<void(bool)> onArmed)
    {
        Entry e;
        e.objectName = objectName;
        e.eventName  = eventName;
        e.callback   = std::move(cb);
        e.onArmed    = std::move(onArmed);
        e.since.start();
        m_entries.push_back(std::move(e));

        // If a handle for this object is already live, arm right now.
        if (LogosObject* obj = liveHandle(objectName)) {
            armAgainst(objectName, obj);
            return;
        }
        qDebug().nospace() << "LogosAPIConsumer: '" << objectName << "::" << eventName
                           << "' deferred pending the module becoming reachable";
        startAcquire(objectName);
        ensureTimer();
    }

    QStringList pending() const
    {
        QStringList out;
        for (const Entry& e : m_entries)
            out << (e.objectName + QStringLiteral("::") + e.eventName);
        return out;
    }

    // The connection was torn down and rebuilt (LogosAPIConsumer::reconnect):
    // every handle we hold points at a replica whose node is gone. Put the
    // armed subscriptions back into the pending set so they re-arm against the
    // new connection instead of going quietly dead.
    //
    // NOTE this is NOT the module-unload path. A module that unloads and comes
    // back drives its replica Suspect → Valid on the SAME node, and the event
    // helper is attached to that replica, so those subscriptions survive on
    // their own with nothing to do here.
    void reconnected()
    {
        for (LogosObject* obj : m_handles)
            if (obj) obj->release();
        m_handles.clear();
        m_acquiring.clear();

        QVector<Entry> revive = std::move(m_armed);
        m_armed.clear();
        QSet<QString> objects;
        for (Entry& e : revive) {
            e.since.start();
            e.warnLevel = 0;
            objects.insert(e.objectName);
            m_entries.push_back(std::move(e));
        }
        for (const QString& name : objects)
            startAcquire(name);
    }

private:
    struct Entry {
        QString objectName;
        QString eventName;
        LogosObject::EventCallback callback;
        std::function<void(bool)> onArmed;
        QElapsedTimer since;
        int warnLevel = 0;   // 0 = quiet, 1 = warned at 3s, 2 = warned at 60s
    };

    LogosObject* liveHandle(const QString& objectName) const
    {
        LogosObject* obj = m_handles.value(objectName, nullptr);
        return (obj && obj->isValid()) ? obj : nullptr;
    }

    // Ask the transport for a handle. Prefers the deferred path; falls back to
    // a bounded-backoff poll for transports that cannot defer.
    void startAcquire(const QString& objectName)
    {
        if (m_acquiring.contains(objectName)) return;   // one acquire per object

        if (auto* async = dynamic_cast<LogosTransportAsyncAcquire*>(m_transport)) {
            QPointer<LogosAPIConsumer> guard(m_owner);
            const QString name = objectName;
            if (async->requestObjectWhenAvailable(name, [this, guard, name](LogosObject* obj) {
                    if (!guard) return;               // consumer died first
                    m_acquiring.remove(name);
                    if (obj) armAgainst(name, obj);
                    else     abandon(name);
                })) {
                m_acquiring.insert(objectName);
                return;
            }
        }
        // No deferred acquire on this transport (qt_local / mock / plain). Their
        // requestObject() is a registry hash lookup or an in-memory socket
        // check — non-blocking and cheap enough to poll; tick() will do it.
        // Deliberately NOT used for qt_remote, whose requestObject() enters
        // QRemoteObjectReplica::waitForSource()'s nested event loop even with a
        // 0 timeout (QTBUG-94570 comment aside, timeout>=0 still calls
        // loop.exec()) — a GUI-thread hazard smuggled in through the retry.
    }

    // One timer per consumer, running only while something is pending.
    //
    // On qt_remote it does NO polling at all — every pending object is in
    // m_acquiring, so tick() finds nothing to ask for and the timer exists
    // purely as the log watchdog below. On the other transports it also retries
    // requestObject(), at 250 ms → 5 s, costing a hash lookup or an in-memory
    // socket-state read per pending object per tick.
    //
    // Known cost, stated rather than hidden: on qt_local/mock a retry against a
    // module that is not registered makes the transport log its own "plugin not
    // found" warning, so a long-pending subscription there produces roughly one
    // such line per 5 s per object. That noise is deliberate — it is the
    // transport truthfully reporting a module that is not there — and silencing
    // it would be the silent-failure shape this whole change exists to remove.
    void ensureTimer()
    {
        if (m_entries.isEmpty()) return;
        if (!m_timer) {
            m_timer = new QTimer(m_owner);
            QObject::connect(m_timer, &QTimer::timeout, m_owner, [this]() { tick(); });
        }
        m_intervalMs = 250;                            // a new pending resets the backoff
        m_timer->start(m_intervalMs);
    }

    void tick()
    {
        QSet<QString> wanted;
        for (const Entry& e : m_entries)
            if (!m_acquiring.contains(e.objectName)) wanted.insert(e.objectName);

        for (const QString& name : wanted) {
            LogosObject* obj = m_transport->requestObject(name, 0);
            if (obj) armAgainst(name, obj);
        }

        reportStillPending();

        if (m_entries.isEmpty()) { m_timer->stop(); return; }
        // 250 ms → 5 s cap. Unbounded in TIME on purpose: a module can be
        // installed and loaded mid-session, so any give-up would silently break
        // the package manager's core flow. What is bounded is the noise — two
        // log lines per (object, event), ever.
        if (m_intervalMs < 5000) {
            m_intervalMs = qMin(5000, m_intervalMs * 2);
            m_timer->start(m_intervalMs);
        }
    }

    // Bounded diagnostics. A subscription that is deferred for a few
    // milliseconds during normal startup is not news and must not spam the log;
    // one that is still waiting seconds later is the shape of the original bug
    // and has to leave a durable record. So: one warning at 3 s, one more at
    // 60 s, then silence — never per retry.
    void reportStillPending()
    {
        for (Entry& e : m_entries) {
            const qint64 ms = e.since.elapsed();
            if (e.warnLevel == 0 && ms >= 3000) {
                e.warnLevel = 1;
                qWarning().nospace()
                    << "LogosAPIConsumer: '" << e.objectName << "::" << e.eventName
                    << "' still not reachable after " << ms
                    << " ms -- subscription is DEFERRED, not lost; it will arm when the "
                       "module appears. Is the module loaded?";
            } else if (e.warnLevel == 1 && ms >= 60000) {
                e.warnLevel = 2;
                qWarning().nospace()
                    << "LogosAPIConsumer: '" << e.objectName << "::" << e.eventName
                    << "' still pending after " << ms
                    << " ms. Still retrying; this is the last message about it.";
            }
        }
    }

    // A handle arrived: attach every pending subscription for that object.
    void armAgainst(const QString& objectName, LogosObject* obj)
    {
        LogosObject* handle = m_handles.value(objectName, nullptr);
        if (handle && handle != obj && !handle->isValid()) {
            handle->release();
            handle = nullptr;
        }
        if (!handle) {
            m_handles.insert(objectName, obj);
            handle = obj;
        } else if (handle != obj) {
            obj->release();                            // already had a live one
        }

        // Split FIRST, run callbacks after. onArmed / onEvent can re-enter
        // add() (a consumer re-subscribing on arm), and mutating m_entries
        // while iterating it would be a use-after-free.
        QVector<Entry> matched = takeMatching(objectName);
        for (Entry& e : matched) {
            handle->onEvent(e.eventName, e.callback);
            // Log at the level that matches what was already said: if we
            // warned that this one was pending, close the loop out loud;
            // otherwise it armed promptly and is not news.
            if (e.warnLevel > 0)
                qInfo().nospace()
                    << "LogosAPIConsumer: '" << e.objectName << "::" << e.eventName
                    << "' subscription ARMED after " << e.since.elapsed() << " ms";
            else
                qDebug().nospace()
                    << "LogosAPIConsumer: '" << e.objectName << "::" << e.eventName
                    << "' subscription armed after " << e.since.elapsed() << " ms";
            if (e.onArmed) e.onArmed(true);
            m_armed.push_back(std::move(e));           // keep, so reconnect can re-arm
        }
    }

    QVector<Entry> takeMatching(const QString& objectName)
    {
        QVector<Entry> matched, remaining;
        for (Entry& e : m_entries) {
            if (e.objectName == objectName) matched.push_back(std::move(e));
            else                            remaining.push_back(std::move(e));
        }
        m_entries = std::move(remaining);
        if (m_entries.isEmpty() && m_timer) m_timer->stop();
        return matched;
    }

    // The transport proved this object can never be acquired on this
    // connection. Drop the subscriptions LOUDLY — a permanently dead
    // subscription that still looks pending is the original bug wearing a
    // different hat.
    void abandon(const QString& objectName)
    {
        const QVector<Entry> matched = takeMatching(objectName);
        for (const Entry& e : matched) {
            qWarning().nospace()
                << "LogosAPIConsumer: '" << e.objectName << "::" << e.eventName
                << "' ABANDONED -- the transport reported this object permanently "
                   "unavailable. This subscription will never fire.";
            if (e.onArmed) e.onArmed(false);
        }
    }

    LogosAPIConsumer* m_owner;
    LogosTransportConnection* m_transport;
    QVector<Entry> m_entries;   // waiting to arm
    QVector<Entry> m_armed;     // live; retained only so reconnected() can re-arm
    QSet<QString> m_acquiring;
    // Subscription handles, one per object, deliberately SEPARATE from
    // LogosAPIConsumer::m_objectCache. Sharing that cache would let the call
    // path release() a handle a live subscription is attached to (it drops a
    // stale entry on the next call), killing the subscription with no trace.
    QHash<QString, LogosObject*> m_handles;
    QTimer* m_timer = nullptr;
    int m_intervalMs = 250;
};

LogosAPIConsumer::LogosAPIConsumer(const QString& module_to_talk_to,
                                   const QString& origin_module,
                                   TokenManager* token_manager,
                                   const LogosTransportConfig& transport,
                                   QObject *parent)
    : QObject(parent)
    , m_registryUrl(LogosInstance::id(module_to_talk_to))
    , m_token_manager(token_manager)
{
    // Single transport-resolution path: the factory combines LogosMode
    // + LogosTransportConfig (mode wins for Mock/Local; transport
    // chooses the wire protocol in Remote mode). The choice scopes to
    // this consumer only — any LogosAPIProvider in the same LogosAPI
    // still constructs its host from the global default.
    m_transport = LogosTransportFactory::createConnection(transport, m_registryUrl);

    // Initial connect with deadline-driven retry. The target module's
    // listener may not be ready yet — particularly for TCP/TLS, where
    // the child subprocess's QTcpServer::listen() lags the runtime
    // returning from its load callback. QLocalSocket internally
    // tolerates this (it retries connect until a deadline), but
    // boost::asio::connect on TCP fails fast with "connection refused"
    // and we'd surface a warning + return nullptr for any subsequent
    // requestObject before the listener even came up.
    //
    // 50ms × up-to-100 attempts ≈ 5s budget — same shape as
    // logos-liblogos's sendTokenToProcess loop and generous enough to
    // cover cold-start child Qt initialisation under load.
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(5000);
    while (true) {
        if (m_transport->connectToHost()) break;
        if (clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

LogosAPIConsumer::LogosAPIConsumer(const QString& module_to_talk_to,
                                   const QString& origin_module,
                                   TokenManager* token_manager,
                                   QObject *parent)
    : LogosAPIConsumer(module_to_talk_to, origin_module, token_manager,
                       LogosTransportConfigGlobal::getDefault(), parent)
{
}

LogosAPIConsumer::~LogosAPIConsumer()
{
    // Release cached handles while m_transport is still alive (the destructor
    // body runs before member destruction). Same for the subscription registry,
    // which owns handles of its own.
    delete m_pendingSubs;
    m_pendingSubs = nullptr;
    clearObjectCache();
}

void LogosAPIConsumer::onEventWhenAvailable(const QString& objectName,
                                            const QString& eventName,
                                            std::function<void(const QString&, const QVariantList&)> callback,
                                            std::function<void(bool)> onArmed)
{
    if (objectName.isEmpty() || eventName.isEmpty() || !callback) {
        qWarning() << "LogosAPIConsumer::onEventWhenAvailable: empty object/event name "
                      "or null callback -- refusing" << objectName << eventName;
        if (onArmed) onArmed(false);
        return;
    }
    if (!m_pendingSubs)
        m_pendingSubs = new LogosPendingSubscriptions(this, m_transport.get());
    m_pendingSubs->add(objectName, eventName, std::move(callback), std::move(onArmed));
}

QStringList LogosAPIConsumer::pendingSubscriptions() const
{
    return m_pendingSubs ? m_pendingSubs->pending() : QStringList();
}

LogosObject* LogosAPIConsumer::requestObject(const QString& objectName, Timeout timeout)
{
    qDebug() << "LogosAPIConsumer: Requesting object:" << objectName << "at" << QTime::currentTime().toString("hh:mm:ss.zzz");

    if (objectName.isEmpty()) {
        qWarning() << "LogosAPIConsumer: Object name cannot be empty";
        return nullptr;
    }

    if (!m_transport->isConnected()) {
        qWarning() << "LogosAPIConsumer: Not connected to registry. Cannot request object:" << objectName;
        return nullptr;
    }

    LogosObject* object = m_transport->requestObject(objectName, timeout.ms);
    if (object) {
        qDebug() << "[LogosObject] LogosAPIConsumer: acquired LogosObject for:" << objectName << "(id:" << object->id() << ")";
    }
    return object;
}

bool LogosAPIConsumer::isConnected() const
{
    return m_transport->isConnected();
}

QString LogosAPIConsumer::registryUrl() const
{
    return m_registryUrl;
}

bool LogosAPIConsumer::reconnect()
{
    qDebug() << "LogosAPIConsumer: Attempting to reconnect to registry:" << m_registryUrl;
    // Handles from the old connection point at replicas that are now dead; drop
    // them so the next call re-acquires against the fresh connection.
    clearObjectCache();
    const bool ok = m_transport->reconnect();
    // Same problem, different owner: deferred subscriptions hold their own
    // handles. Re-arm them rather than leaving them attached to dead replicas.
    if (m_pendingSubs) m_pendingSubs->reconnected();
    return ok;
}

QVariant LogosAPIConsumer::invokeRemoteMethod(const QString& authToken, const QString& objectName, const QString& methodName,
                                   const QVariantList& args, Timeout timeout)
{
    return invokeRemoteMethod(authToken, objectName, methodName, args, timeout, nullptr);
}

QVariant LogosAPIConsumer::invokeRemoteMethod(const QString& authToken, const QString& objectName, const QString& methodName,
                                   const QVariantList& args, Timeout timeout, logos::CallError* err)
{
    if (err) err->clear();
    qDebug() << "LogosAPIConsumer: Calling invokeRemoteMethod:" << objectName << methodName << "args_count:" << args.size() << "timeout:" << timeout.ms;

    // Reuse a cached handle across calls. Acquiring a QtRO replica per call
    // (acquireDynamic + waitForSource) is expensive — under a tight loop (e.g. a
    // proxy forwarding every method to its target) it dominates and can starve
    // the nested synchronous calls. The handle is kept alive in m_objectCache
    // and re-acquired only when it goes stale.
    LogosObject* plugin = acquireCachedObject(objectName, timeout.ms);
    if (!plugin) {
        qWarning() << "LogosAPIConsumer: Failed to acquire plugin/replica for object:" << objectName;
        if (err) {
            err->code = "object_unavailable";
            err->message = "failed to acquire remote object '"
                           + objectName.toStdString()
                           + "' (module not loaded, not published, or transport failure)";
            err->origin = objectName.toStdString();
        }
        return QVariant();
    }

    qDebug() << "[LogosObject] LogosAPIConsumer: calling via LogosObject::callMethod" << methodName;
    // No release() here: the handle stays cached for the next call. Released in
    // clearObjectCache() (destructor / reconnect) or evicted when stale.
    //
    // Prefer the error channel when the transport implements it (see
    // LogosObjectErrorChannel in logos_object.h). Without it, `err` could only
    // ever describe an ACQUIRE failure — everything that went wrong after the
    // handle existed (the deadline elapsing, the connection dropping, the peer
    // answering "not published") came back as a bare QVariant() with a clean
    // err, i.e. reported as a method that returned null.
    if (auto* channel = dynamic_cast<LogosObjectErrorChannel*>(plugin))
        return channel->callMethodWithError(authToken, methodName, args,
                                            timeout.ms, err);
    return plugin->callMethod(authToken, methodName, args, timeout.ms);
}

// Get-or-acquire a remote-object handle, transparently refreshing a stale one.
LogosObject* LogosAPIConsumer::acquireCachedObject(const QString& objectName, int timeoutMs)
{
    if (LogosObject* cached = m_objectCache.value(objectName, nullptr)) {
        if (cached->isValid())
            return cached;
        // The source went away (module unloaded / transport dropped) — discard
        // the dead handle and acquire a fresh one below.
        qDebug() << "LogosAPIConsumer: cached handle for" << objectName << "went stale; re-acquiring";
        cached->release();
        m_objectCache.remove(objectName);
    }
    LogosObject* obj = m_transport->requestObject(objectName, timeoutMs);
    if (obj)
        m_objectCache.insert(objectName, obj);
    return obj;
}

void LogosAPIConsumer::clearObjectCache()
{
    for (LogosObject* obj : m_objectCache)
        if (obj) obj->release();
    m_objectCache.clear();
    // Drop the remembered absences too: after a reconnect, or once a module is
    // reloaded from a build that has the surface, it deserves a fresh probe.
    m_noHandshakeSurface.clear();
}

void LogosAPIConsumer::invokeRemoteMethodAsync(const QString& authToken, const QString& objectName, const QString& methodName,
                                                const QVariantList& args,
                                                AsyncResultCallback callback,
                                                Timeout timeout)
{
    // Delegate to the CallError-aware overload so there is one acquire/dispatch
    // path — the legacy callback simply drops the error field.
    invokeRemoteMethodAsync(authToken, objectName, methodName, args,
        [cb = std::move(callback)](QVariant r, const logos::CallError&) mutable {
            if (cb) cb(std::move(r));
        },
        timeout);
}

void LogosAPIConsumer::invokeRemoteMethodAsync(const QString& authToken, const QString& objectName, const QString& methodName,
                                                const QVariantList& args,
                                                AsyncResultErrorCallback callback,
                                                Timeout timeout)
{
    if (!callback) {
        qWarning() << "LogosAPIConsumer: invokeRemoteMethodAsync called with null callback";
        return;
    }

    // Reuse the cached handle, same as the sync path — repeated async calls to
    // one object (e.g. a proxy forwarding asynchronously) no longer re-acquire a
    // replica per call. The handle stays owned by m_objectCache; the callback
    // must NOT release it (it is shared across in-flight calls and freed only on
    // eviction/teardown, via release()'s deferred deleteLater).
    LogosObject* plugin = acquireCachedObject(objectName, timeout.ms);
    if (!plugin) {
        qWarning() << "LogosAPIConsumer: Failed to acquire plugin/replica for object:" << objectName;
        logos::CallError err;
        err.code = "object_unavailable";
        err.message = "failed to acquire remote object '"
                      + objectName.toStdString()
                      + "' (module not loaded, not published, or transport failure)";
        err.origin = objectName.toStdString();
        QTimer::singleShot(0, this, [callback, err]() { callback(QVariant(), err); });
        return;
    }

    qDebug() << "[LogosObject] LogosAPIConsumer: async calling via LogosObject::callMethodAsync" << methodName;
    // QPointer guards against use-after-free: if the consumer is destroyed
    // before the transport callback fires, the callback is silently dropped and
    // the handle is released by the destructor's clearObjectCache(), not here.
    QPointer<LogosAPIConsumer> self(this);

    // Prefer the error channel when the transport implements it. The lambda
    // below used to take only `QVariant result` and hand the caller a
    // hard-coded empty logos::CallError — so once acquire had succeeded, every
    // async outcome was reported as a success, whatever actually happened.
    if (auto* channel = dynamic_cast<LogosObjectErrorChannel*>(plugin)) {
        channel->callMethodAsyncWithError(authToken, methodName, args, timeout.ms,
            [callback, self](QVariant result, const logos::CallError& err) {
                if (!self)
                    return;
                callback(std::move(result), err);
            });
        return;
    }

    // Transport without an error channel (the mock): unchanged behaviour —
    // the value, and no diagnosis to give.
    plugin->callMethodAsync(authToken, methodName, args, timeout.ms,
        [callback, self](QVariant result) {
            if (!self)
                return;
            callback(result, logos::CallError{});
        });
}

void LogosAPIConsumer::onEvent(LogosObject* originObject, const QString& eventName, std::function<void(const QString&, const QVariantList&)> callback)
{
    qDebug() << "[LogosObject] LogosAPIConsumer::onEvent registering for:" << eventName << "on LogosObject id:" << originObject;

    if (!originObject) {
        qWarning() << "LogosAPIConsumer: Cannot register event on null object";
        return;
    }

    originObject->onEvent(eventName, std::move(callback));

    qDebug() << "[LogosObject] LogosAPIConsumer: event callback registered for:" << eventName;
}

bool LogosAPIConsumer::informModuleToken(const QString& authToken, const QString& moduleName, const QString& token)
{
    qDebug() << "LogosAPIConsumer: Informing module token for module:" << moduleName << "with token:" << redactToken(token);

    LogosObject* plugin = m_transport->requestObject("capability_module", 20000);
    if (!plugin) {
        qWarning() << "LogosAPIConsumer: Failed to acquire plugin/replica for object: capability_module";
        return false;
    }

    qDebug() << "[LogosObject] LogosAPIConsumer: calling LogosObject::informModuleToken for" << moduleName;
    bool result = plugin->informModuleToken(authToken, moduleName, token, 20000);
    qDebug() << "LogosAPIConsumer: informModuleToken completed with result:" << result;
    plugin->release();
    return result;
}

namespace {
// How long to wait when probing for a handshake surface before concluding the
// target predates it. Long enough to cover a live local socket round trip,
// short enough that the fallback is not perceptibly delayed.
constexpr int kHandshakeProbeTimeoutMs = 250;
} // namespace

bool LogosAPIConsumer::informModuleToken_module(const QString& authToken, const QString& originModule, const QString& moduleName, const QString& token, int timeoutMs)
{
    // A non-positive budget would make the wait transport-dependent rather than
    // bounded; fall back to the historical default.
    if (timeoutMs <= 0) {
        timeoutMs = 20000;
    }
    qDebug() << "LogosAPIConsumer: Informing module token for module:" << moduleName << "with token:" << redactToken(token);

    // Prefer the handshake surface. It is published before the target's
    // initializer runs, so it is reachable even while the target is still
    // starting up — which is the one case the business object cannot cover,
    // because that one is published only once the initializer returns.
    //
    // Short budget on this attempt: a module built before the handshake surface
    // existed simply has no such object, and we must not spend the full timeout
    // discovering that before falling back.
    // acquireCachedObject caches successes only, so without the negative cache
    // below a module built before this surface existed would pay the full probe
    // budget on EVERY grant — on QtRO that is a blocking waitForSource, i.e.
    // kHandshakeProbeTimeoutMs of dead time per token, forever. Remember the
    // absence instead and go straight to the business object. Cleared with the
    // handle cache on reconnect/destroy, so a module that comes back with a
    // handshake surface is re-probed rather than written off permanently.
    const QString handshake = logos::handshakeObjectName(originModule);
    if (m_noHandshakeSurface.contains(handshake)) {
        return informModuleTokenViaBusinessObject(authToken, originModule, moduleName, token, timeoutMs);
    }
    LogosObject* early = acquireCachedObject(handshake, kHandshakeProbeTimeoutMs);
    if (!early) {
        m_noHandshakeSurface.insert(handshake);
        qDebug() << "LogosAPIConsumer:" << originModule << "publishes no handshake surface"
                 << "- not probing again until the handle cache is cleared";
    }
    if (early) {
        qDebug() << "[LogosObject] LogosAPIConsumer: delivering token for" << moduleName
                 << "via the handshake surface of" << originModule;
        if (early->informModuleToken(authToken, moduleName, token, timeoutMs)) {
            qDebug() << "LogosAPIConsumer: informModuleToken completed with result: true";
            return true;
        }
        // A refusal HERE is not authoritative, so do not report it as the answer.
        // The handshake surface goes live before the target's initializer runs,
        // and a target whose token store is only seeded by that initializer will
        // refuse a push that arrives first. Falling through to the business
        // object — which exists only once the initializer has returned, by which
        // point the store is populated — is what the caller got before this
        // surface existed. Returning false here instead would hand the caller an
        // empty grant that it has no way to distinguish from a real denial.
        //
        // This cannot reintroduce the startup wedge: the wait below is bounded by
        // the caller's own budget (capability_module passes 3000 ms), not by the
        // 20 s default that made the original deadlock fatal.
        qWarning() << "LogosAPIConsumer: handshake surface of" << originModule
                   << "refused the token for" << moduleName
                   << "- it is probably still initializing; retrying on the business object";
    }

    return informModuleTokenViaBusinessObject(authToken, originModule, moduleName, token, timeoutMs);
}

// Fall back to the business object: modules built before the handshake surface
// existed are reached exactly as they always were. Also the landing place for a
// handshake surface that refused the push (target still initializing).
bool LogosAPIConsumer::informModuleTokenViaBusinessObject(const QString& authToken, const QString& originModule, const QString& moduleName, const QString& token, int timeoutMs)
{
    LogosObject* plugin = acquireCachedObject(originModule, timeoutMs);
    if (!plugin) {
        qWarning() << "LogosAPIConsumer: Failed to acquire plugin/replica for object:" << originModule
                   << "- no handshake surface and no published business object"
                   << "(waited" << timeoutMs << "ms; it may still be initializing)";
        return false;
    }

    qDebug() << "[LogosObject] LogosAPIConsumer: calling LogosObject::informModuleToken for" << moduleName << "on" << originModule;
    bool result = plugin->informModuleToken(authToken, moduleName, token, timeoutMs);
    qDebug() << "LogosAPIConsumer: informModuleToken completed with result:" << result;
    // The cache owns the handle now, so it is not released here.
    return result;
}

std::string LogosAPIConsumer::requestModule(const std::string& authToken, const std::string& originModule, const std::string& targetModule)
{
    const QString qOrigin = QString::fromStdString(originModule);
    const QString qTarget = QString::fromStdString(targetModule);
    qDebug() << "LogosAPIConsumer: requestModule for origin:" << qOrigin << "target:" << qTarget;

    LogosObject* plugin = m_transport->requestObject("capability_module", 20000);
    if (!plugin) {
        qWarning() << "LogosAPIConsumer: Failed to acquire plugin/replica for object: capability_module";
        return {};
    }

    QVariant result = plugin->callMethod(QString::fromStdString(authToken), QStringLiteral("requestModule"),
                                         QVariantList() << qOrigin << qTarget, 20000);
    plugin->release();
    return result.toString().toStdString();
}
