#include "logos_api_client.h"
#include "logos_api_consumer.h"
#include "logos_object.h"
#include "logos_types.h"
#include "logos_json_convert.h"
#include "logos_thread_marshal.h"
#include "logos_rpc_status.h"
#include "token_manager.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QMetaObject>
#include <QMetaType>
#include <QPointer>
#include <string>

using logos::qvariantToNlohmann;
using logos::nlohmannArgsToQVariantList;

LogosAPIClient::LogosAPIClient(const QString& module_to_talk_to,
                               const QString& origin_module,
                               TokenManager* token_manager,
                               const LogosTransportConfig& target_transport,
                               const LogosTransportConfig& capability_transport,
                               QObject *parent)
    : QObject(parent)
    , m_consumer(new LogosAPIConsumer(module_to_talk_to, origin_module,
                                      token_manager, target_transport, this))
    , m_token_manager(token_manager)
    , m_origin_module(origin_module)
    // Pre-build the capability_module consumer once. We skip it for
    // the capability_module client itself — the auto-`requestModule`
    // path is gated by `objectName != "capability_module"` so we'd
    // never use it, and constructing one would be a redundant
    // self-connection. Init-list order matches the declaration order
    // in the header — `m_capability_consumer` is appended at the end
    // for ABI stability (see header comment).
    , m_capability_consumer(module_to_talk_to == QStringLiteral("capability_module")
        ? nullptr
        : new LogosAPIConsumer(QStringLiteral("capability_module"),
                                origin_module, token_manager,
                                capability_transport, this))
{
}

LogosAPIClient::LogosAPIClient(const QString& module_to_talk_to,
                               const QString& origin_module,
                               TokenManager* token_manager,
                               QObject *parent)
    : LogosAPIClient(module_to_talk_to, origin_module, token_manager,
                     LogosTransportConfigGlobal::getDefault(),
                     LogosTransportConfigGlobal::getDefault(), parent)
{
}

LogosAPIClient::~LogosAPIClient()
{
}

LogosObject* LogosAPIClient::requestObject(const QString& objectName, Timeout timeout)
{
    // Marshal to the owner thread: the replica is acquired and lives there.
    return logos::runOnOwnerThread(this, [&]() -> LogosObject* {
        return m_consumer->requestObject(objectName, timeout);
    });
}

bool LogosAPIClient::isConnected() const
{
    return m_consumer->isConnected();
}

QString LogosAPIClient::registryUrl() const
{
    return m_consumer->registryUrl();
}

bool LogosAPIClient::reconnect()
{
    return m_consumer->reconnect();
}

QVariant LogosAPIClient::invokeRemoteMethod(const QString& objectName, const QString& methodName,
                                   const QVariantList& args, Timeout timeout)
{
    return invokeRemoteMethod(objectName, methodName, args, timeout, nullptr);
}

QVariant LogosAPIClient::invokeRemoteMethod(const QString& objectName, const QString& methodName,
                                   const QVariantList& args, Timeout timeout, logos::CallError* err)
{
    if (err) err->clear();
    // Marshal the whole operation (capability/token fetch + the call) onto the
    // owner thread so a worker thread (e.g. an HTTP handler) can call other
    // modules. Same-thread callers run directly. See logos_thread_marshal.h.
    return logos::runOnOwnerThread(this, [&]() -> QVariant {
    qDebug() << "LogosAPIClient: invoking remote method" << objectName << methodName << "args_count:" << args.size();

    const bool eligible = objectName != QStringLiteral("capability_module") && m_capability_consumer;

    QString token = getToken(objectName);
    if (token.isEmpty() && eligible)
        token = mintAndCacheToken(objectName);   // first exchange (cached for later calls)

    QVariant result = m_consumer->invokeRemoteMethod(token, objectName, methodName, args, timeout, err);

    // Re-exchange on rejection, once. The provider rejected our (stale) token —
    // drop it, mint a fresh one via capability_module, and retry the call. Gated
    // on the explicit provider sentinel (never a plain empty result), so it can't
    // loop, can't misfire on a legitimately-empty return, and never fires against
    // an old provider (which returns a bare QVariant() we don't match). This also
    // lazily recovers the common provider-reload case. See logos_rpc_status.h.
    if (eligible && logos::isUnauthorizedSentinel(result)) {
        qWarning() << "LogosAPIClient: token for" << objectName
                   << "rejected by provider; re-exchanging and retrying once";
        m_token_manager->removeToken(objectName);
        const QString fresh = mintAndCacheToken(objectName);
        if (!fresh.isEmpty())
            result = m_consumer->invokeRemoteMethod(fresh, objectName, methodName, args, timeout, err);
    }

    // Never surface the sentinel to the typed wrapper. If we still hold it the
    // retry failed (capability down / provider truly gone): collapse to today's
    // empty result, and for NEW callers set a distinguishable CallError.
    if (logos::isUnauthorizedSentinel(result)) {
        if (err) {
            err->code    = "unauthorized";
            err->message = "call to '" + objectName.toStdString()
                         + "' rejected: token not recognized (re-exchange failed)";
            err->origin  = objectName.toStdString();
        }
        return QVariant();
    }
    return result;
    });
}

QString LogosAPIClient::mintAndCacheToken(const QString& objectName)
{
    qDebug() << "LogosAPIClient: calling requestModule for" << objectName;
    const QString capabilityToken = getToken(QStringLiteral("capability_module"));
    const QString token = QString::fromStdString(
        m_capability_consumer->requestModule(capabilityToken.toStdString(),
                                             m_origin_module.toStdString(),
                                             objectName.toStdString()));
    qDebug() << "LogosAPIClient: requestModule result for" << objectName << ":" << token;
    // Cache the minted token so subsequent calls skip the handshake — closes the
    // token-rotation race where overlapping requestModule calls mint fresh tokens
    // that overwrite each other at the target (e.g. QtRO's sync wait reentering
    // via a nested event loop).
    if (!token.isEmpty())
        m_token_manager->saveToken(objectName, token);
    return token;
}

QVariant LogosAPIClient::invokeRemoteMethod(const QString& objectName, const QString& methodName,
                                   const QVariant& arg, Timeout timeout)
{
    return invokeRemoteMethod(objectName, methodName, QVariantList() << arg, timeout);
}

QVariant LogosAPIClient::invokeRemoteMethod(const QString& objectName, const QString& methodName,
                                   const QVariant& arg1, const QVariant& arg2, Timeout timeout)
{
    return invokeRemoteMethod(objectName, methodName, QVariantList() << arg1 << arg2, timeout);
}

QVariant LogosAPIClient::invokeRemoteMethod(const QString& objectName, const QString& methodName,
                                   const QVariant& arg1, const QVariant& arg2, const QVariant& arg3, Timeout timeout)
{
    return invokeRemoteMethod(objectName, methodName, QVariantList() << arg1 << arg2 << arg3, timeout);
}

QVariant LogosAPIClient::invokeRemoteMethod(const QString& objectName, const QString& methodName,
                                   const QVariant& arg1, const QVariant& arg2, const QVariant& arg3,
                                   const QVariant& arg4, Timeout timeout)
{
    return invokeRemoteMethod(objectName, methodName, QVariantList() << arg1 << arg2 << arg3 << arg4, timeout);
}

QVariant LogosAPIClient::invokeRemoteMethod(const QString& objectName, const QString& methodName,
                                   const QVariant& arg1, const QVariant& arg2, const QVariant& arg3,
                                   const QVariant& arg4, const QVariant& arg5, Timeout timeout)
{
    return invokeRemoteMethod(objectName, methodName, QVariantList() << arg1 << arg2 << arg3 << arg4 << arg5, timeout);
}

void LogosAPIClient::invokeRemoteMethodAsync(const QString& objectName, const QString& methodName,
                                              const QVariantList& args, AsyncResultCallback callback,
                                              Timeout timeout)
{
    // Delegate to the CallError-aware overload; legacy callers just drop the
    // error field. Keeps the handshake-coalescing logic single-sourced.
    invokeRemoteMethodAsync(objectName, methodName, args,
        [cb = std::move(callback)](QVariant r, const logos::CallError&) mutable {
            if (cb) cb(std::move(r));
        },
        timeout);
}

void LogosAPIClient::invokeRemoteMethodAsync(const QString& objectName, const QString& methodName,
                                              const QVariantList& args, AsyncResultErrorCallback callback,
                                              Timeout timeout)
{
    // Public entry: grant one retry for the rejection-driven re-exchange.
    invokeRemoteMethodAsyncImpl(objectName, methodName, args, std::move(callback), timeout, /*retriesLeft=*/1);
}

void LogosAPIClient::invokeRemoteMethodAsyncImpl(const QString& objectName, const QString& methodName,
                                                 const QVariantList& args, AsyncResultErrorCallback callback,
                                                 Timeout timeout, int retriesLeft)
{
    if (!callback) return;

    // The async path acquires a replica too, so it must also run on the owner
    // thread. Unlike the sync path we post non-blocking (QueuedConnection): the
    // worker caller returns immediately and the result callback fires on the
    // owner thread when the reply arrives. Preserve retriesLeft across the hop.
    if (QThread::currentThread() != this->thread()) {
        QMetaObject::invokeMethod(this,
            [this, objectName, methodName, args,
             callback = std::move(callback), timeout, retriesLeft]() mutable {
                invokeRemoteMethodAsyncImpl(objectName, methodName, args,
                                            std::move(callback), timeout, retriesLeft);
            },
            Qt::QueuedConnection);
        return;
    }

    const bool eligible = objectName != QStringLiteral("capability_module") && m_capability_consumer;

    // Wrap the user callback so a provider rejection sentinel triggers one
    // re-exchange + retry, and the sentinel is never surfaced to the caller.
    // Mirrors the sync path's retry in logos_api_client.cpp's invokeRemoteMethod.
    QPointer<LogosAPIClient> selfGuard(this);
    AsyncResultErrorCallback onResult =
        [this, selfGuard, objectName, methodName, args, timeout, retriesLeft, cb = std::move(callback)]
        (QVariant result, const logos::CallError& err) mutable {
            if (!selfGuard) return;   // client destroyed mid-flight: drop
            if (retriesLeft > 0 && objectName != QStringLiteral("capability_module")
                && m_capability_consumer && logos::isUnauthorizedSentinel(result)) {
                qWarning() << "LogosAPIClient: token for" << objectName
                           << "rejected by provider (async); re-exchanging and retrying once";
                m_token_manager->removeToken(objectName);
                // Token is empty now → the re-entry coalesces the retry through the
                // same m_pendingHandshakes machinery, so a burst of concurrent
                // rejections doesn't restorm capability_module with N handshakes.
                invokeRemoteMethodAsyncImpl(objectName, methodName, args,
                                            std::move(cb), timeout, retriesLeft - 1);
                return;
            }
            if (logos::isUnauthorizedSentinel(result)) {
                logos::CallError e;
                e.code    = "unauthorized";
                e.message = "call to '" + objectName.toStdString()
                          + "' rejected: token not recognized (re-exchange failed)";
                e.origin  = objectName.toStdString();
                cb(QVariant(), e);
                return;
            }
            cb(std::move(result), err);
        };

    QString token = getToken(objectName);

    if (token.isEmpty() && eligible) {
        // Async-chain: dispatch the requestModule call asynchronously, and only
        // fire the real method's invokeRemoteMethodAsync from its callback. The
        // previous version called `requestModule` synchronously here, which made
        // the "async" entry point block its caller for the full round-trip.
        //
        // COALESCE concurrent first-calls behind ONE handshake. A driver that
        // fans out N async calls to an un-tokened target before any completes
        // would otherwise fire N separate requestModule handshakes; each mints a
        // distinct token and informs the target, and the later inform OVERWRITES
        // the earlier token there (the target stores one token per caller). The
        // already-dispatched calls then carry a superseded token and the target
        // rejects them as unauthorized. So only the first caller starts the
        // handshake; the rest queue and all drain with the single minted token.
        // (The sync path can't hit this — it blocks per call, so handshakes
        // never overlap.) m_pendingHandshakes is touched only on the owner
        // thread, reached above, so no lock is needed.
        m_pendingHandshakes[objectName].push_back(
            [this, objectName, methodName, args, timeout, cb = std::move(onResult)]
            (const QString& tok) mutable {
                m_consumer->invokeRemoteMethodAsync(tok, objectName, methodName, args,
                                                    std::move(cb), timeout);
            });
        if (m_pendingHandshakes[objectName].size() > 1)
            return;  // a handshake for this target is already in flight

        const QString capabilityToken = getToken("capability_module");
        const QString origin = m_origin_module;
        // Lifetime: capture the client through a QPointer guard. If it (and its
        // QObject-parented consumers + the pending queue) is destroyed while the
        // requestModule round-trip is in flight, the guard goes null and we drop
        // the queued continuations instead of dereferencing dangling memory.
        QPointer<LogosAPIClient> self(this);
        m_capability_consumer->invokeRemoteMethodAsync(
            capabilityToken,
            QStringLiteral("capability_module"),
            QStringLiteral("requestModule"),
            QVariantList() << origin << objectName,
            [self, objectName](const QVariant& tokenResult) mutable {
                if (!self) return;  // client destroyed mid-flight
                const QString tok = tokenResult.toString();
                // Cache the minted token before draining so future calls skip the handshake — m_pendingHandshakes only coalesces the first burst, the cache stops a second burst from racing the same rotation.
                if (!tok.isEmpty()) self->m_token_manager->saveToken(objectName, tok);
                // Drain every continuation queued for this target with the one
                // minted token — the target was informed of exactly this token.
                // An empty tok (handshake failed) still flows through: the
                // consumer call is then rejected and each callback fires with an
                // invalid QVariant, so callers never hang.
                auto it = self->m_pendingHandshakes.find(objectName);
                if (it == self->m_pendingHandshakes.end()) return;
                std::vector<std::function<void(const QString&)>> calls = std::move(it.value());
                self->m_pendingHandshakes.erase(it);
                for (auto& c : calls) c(tok);
            },
            timeout);
        return;
    }

    m_consumer->invokeRemoteMethodAsync(token, objectName, methodName, args, std::move(onResult), timeout);
}

void LogosAPIClient::invokeRemoteMethodAsync(const QString& objectName, const QString& methodName,
                                              const QVariant& arg, AsyncResultCallback callback,
                                              Timeout timeout)
{
    invokeRemoteMethodAsync(objectName, methodName, QVariantList() << arg, std::move(callback), timeout);
}

void LogosAPIClient::invokeRemoteMethodAsync(const QString& objectName, const QString& methodName,
                                              const QVariant& arg1, const QVariant& arg2,
                                              AsyncResultCallback callback, Timeout timeout)
{
    invokeRemoteMethodAsync(objectName, methodName, QVariantList() << arg1 << arg2, std::move(callback), timeout);
}

void LogosAPIClient::invokeRemoteMethodAsync(const QString& objectName, const QString& methodName,
                                              const QVariant& arg1, const QVariant& arg2, const QVariant& arg3,
                                              AsyncResultCallback callback, Timeout timeout)
{
    invokeRemoteMethodAsync(objectName, methodName, QVariantList() << arg1 << arg2 << arg3, std::move(callback), timeout);
}

void LogosAPIClient::invokeRemoteMethodAsync(const QString& objectName, const QString& methodName,
                                              const QVariant& arg1, const QVariant& arg2, const QVariant& arg3,
                                              const QVariant& arg4, AsyncResultCallback callback,
                                              Timeout timeout)
{
    invokeRemoteMethodAsync(objectName, methodName, QVariantList() << arg1 << arg2 << arg3 << arg4, std::move(callback), timeout);
}

void LogosAPIClient::invokeRemoteMethodAsync(const QString& objectName, const QString& methodName,
                                              const QVariant& arg1, const QVariant& arg2, const QVariant& arg3,
                                              const QVariant& arg4, const QVariant& arg5,
                                              AsyncResultCallback callback, Timeout timeout)
{
    invokeRemoteMethodAsync(objectName, methodName, QVariantList() << arg1 << arg2 << arg3 << arg4 << arg5, std::move(callback), timeout);
}

void LogosAPIClient::onEvent(LogosObject* originObject, const QString& eventName, std::function<void(const QString&, const QVariantList&)> callback)
{
    // Marshal to the owner thread: event registration touches the replica.
    logos::runOnOwnerThread(this, [&]() {
        m_consumer->onEvent(originObject, eventName, std::move(callback));
    });
}

quint64 LogosAPIClient::onEventWhenAvailable(const QString& objectName, const QString& eventName,
                                             std::function<void(const QString&, const QVariantList&)> callback,
                                             std::function<void(bool)> onArmed)
{
    // Marshal to the owner thread for the same reason onEvent() does: the
    // registry touches (and later arms against) a QtRO replica, which only
    // works on the thread that created the node.
    return logos::runOnOwnerThread(this, [&]() -> quint64 {
        return m_consumer->onEventWhenAvailable(objectName, eventName,
                                                std::move(callback), std::move(onArmed));
    });
}

quint64 LogosAPIClient::whenObjectAvailable(const QString& objectName,
                                            std::function<void(bool)> onReady)
{
    // Same owner-thread marshalling as onEventWhenAvailable: the registry
    // touches a QtRO node that only works on the thread that created it.
    return logos::runOnOwnerThread(this, [&]() -> quint64 {
        return m_consumer->whenObjectAvailable(objectName, std::move(onReady));
    });
}

bool LogosAPIClient::cancelEventSubscription(quint64 subscriptionId)
{
    return logos::runOnOwnerThread(this, [&]() -> bool {
        return m_consumer->cancelEventSubscription(subscriptionId);
    });
}

LogosSubscriptionState LogosAPIClient::eventSubscriptionState(quint64 subscriptionId) const
{
    return logos::runOnOwnerThread(const_cast<LogosAPIClient*>(this),
                                   [&]() -> LogosSubscriptionState {
        return m_consumer->eventSubscriptionState(subscriptionId);
    });
}

QStringList LogosAPIClient::pendingEventSubscriptions() const
{
    return logos::runOnOwnerThread(const_cast<LogosAPIClient*>(this), [&]() -> QStringList {
        return m_consumer->pendingSubscriptions();
    });
}

void LogosAPIClient::onEventResponse(LogosObject* object, const QString& eventName, const QVariantList& data)
{
    qDebug() << "[LogosObject] LogosAPIClient::onEventResponse" << eventName << "-> LogosObject::emitEvent";

    if (eventName.isEmpty()) {
        qWarning() << "LogosAPIClient: Event name cannot be empty";
        return;
    }

    if (!object) {
        qWarning() << "LogosAPIClient: Cannot emit event on null object";
        return;
    }

    object->emitEvent(eventName, data);
}

void LogosAPIClient::onEventResponse(QObject* object, const QString& eventName, const QVariantList& data)
{
    qDebug() << "[LogosObject] LogosAPIClient::onEventResponse (QObject* compat)" << eventName;

    if (eventName.isEmpty()) {
        qWarning() << "LogosAPIClient: Event name cannot be empty";
        return;
    }

    if (!object) {
        qWarning() << "LogosAPIClient: Cannot emit event on null QObject";
        return;
    }

    QMetaObject::invokeMethod(object, "eventResponse",
                              Qt::DirectConnection,
                              Q_ARG(QString, eventName),
                              Q_ARG(QVariantList, data));
}

bool LogosAPIClient::informModuleToken(const QString& authToken, const QString& moduleName, const QString& token)
{
    return m_consumer->informModuleToken(authToken, moduleName, token);
}

bool LogosAPIClient::informModuleToken(const std::string& authToken, const std::string& moduleName, const std::string& token)
{
    return informModuleToken(QString::fromStdString(authToken),
                             QString::fromStdString(moduleName),
                             QString::fromStdString(token));
}

bool LogosAPIClient::informModuleToken_module(const QString& authToken, const QString& originModule, const QString& moduleName, const QString& token, int timeoutMs)
{
    // Marshal to the owner thread, exactly as requestObject/invokeRemoteMethod do.
    // This path now goes through acquireCachedObject, so it reads and mutates
    // m_objectCache — declared single-threaded, and holding thread-affine QtRO
    // handles. Before the handshake surface existed this method used an
    // uncached requestObject + release(), so it touched no shared state; routing
    // it onto the cache is what made the missing marshal reachable.
    //
    // (LogosAPIClient::informModuleToken — the 3-arg form above — has the same
    // missing marshal, but it still uses an uncached handle and predates this
    // change, so it is left alone rather than widened into this fix.)
    return logos::runOnOwnerThread(this, [&]() -> bool {
        return m_consumer->informModuleToken_module(authToken, originModule, moduleName, token, timeoutMs);
    });
}

TokenManager* LogosAPIClient::getTokenManager() const
{
    return m_token_manager;
}

QString LogosAPIClient::getToken(const QString& module_name)
{
    qDebug() << "LogosAPIClient: getToken for module:" << module_name;

    QString token = m_token_manager->getToken(module_name);
    if (!token.isEmpty()) {
        qDebug() << "LogosAPIClient: Found token for module:" << module_name;
        return token;
    }

    qDebug() << "LogosAPIClient: No token found for module:" << module_name;
    return "";
}

// ---------------------------------------------------------------------------
// nlohmann::json overloads
// ---------------------------------------------------------------------------

nlohmann::json LogosAPIClient::invokeRemoteMethod(const std::string& objectName,
                                                   const std::string& methodName,
                                                   const nlohmann::json& args,
                                                   Timeout timeout)
{
    QVariantList qArgs = nlohmannArgsToQVariantList(args);
    QVariant result = invokeRemoteMethod(
        QString::fromStdString(objectName),
        QString::fromStdString(methodName),
        qArgs, timeout);
    return qvariantToNlohmann(result);
}

void LogosAPIClient::onEvent(LogosObject* originObject, const std::string& eventName,
                              std::function<void(const std::string&, const nlohmann::json&)> callback)
{
    onEvent(originObject, QString::fromStdString(eventName),
        [cb = std::move(callback)](const QString& name, const QVariantList& data) {
            nlohmann::json jData = nlohmann::json::array();
            for (const QVariant& v : data)
                jData.push_back(qvariantToNlohmann(v));
            cb(name.toStdString(), jData);
        });
}
