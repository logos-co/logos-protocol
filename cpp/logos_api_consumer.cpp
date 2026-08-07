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
    // body runs before member destruction).
    clearObjectCache();
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
    return m_transport->reconnect();
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
    const QString handshake = logos::handshakeObjectName(originModule);
    if (LogosObject* early = acquireCachedObject(handshake, kHandshakeProbeTimeoutMs)) {
        qDebug() << "[LogosObject] LogosAPIConsumer: delivering token for" << moduleName
                 << "via the handshake surface of" << originModule;
        const bool result = early->informModuleToken(authToken, moduleName, token, timeoutMs);
        qDebug() << "LogosAPIConsumer: informModuleToken completed with result:" << result;
        return result;
    }

    // Fall back to the business object: modules built before the handshake
    // surface existed are reached exactly as they always were.
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
    // The cache owns both handles now, so neither is released here.
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
