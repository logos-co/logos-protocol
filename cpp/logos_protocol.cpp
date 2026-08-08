#include "logos_protocol.h"

#include "logos_api_client.h"
#include "logos_call_error.h"
#include "logos_json_convert.h"
#include "logos_mode.h"
#include "logos_object.h"
#include "logos_thread_marshal.h"
#include "logos_transport_config.h"
#include "logos_transport_config_json.h"
#include "logos_transport_factory.h"
#include "logos_types.h"
#include "token_manager.h"

#include <nlohmann/json.hpp>

#include <QCoreApplication>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaType>
#include <QString>
#include <QThread>
#include <QVariant>
#include <QVariantList>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace {

// Heap-copy a std::string for handing across the C boundary.
// Counterpart of lp_string_free (which is plain free()).
char* lpStrdup(const std::string& s)
{
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, s.data(), s.size() + 1);
    return out;
}

std::string makeErrorJson(const char* code, const std::string& message,
                          const std::string& origin)
{
    nlohmann::json e;
    e["code"] = code;
    e["message"] = message;
    e["origin"] = origin;
    return e.dump();
}

// Parse a single-transport JSON object (the lp_* shape) by reusing the
// transport-set parser (which expects an array). NULL / empty / "null"
// fall back to the process default.
bool parseTransportJson(const char* transport_json, LogosTransportConfig& out)
{
    if (!transport_json || !*transport_json
        || std::strcmp(transport_json, "null") == 0) {
        out = LogosTransportConfigGlobal::getDefault();
        return true;
    }
    const LogosTransportSet set = logos::transportSetFromJsonString(
        std::string("[") + transport_json + "]");
    if (set.empty()) return false;  // parse error (parser yields empty set)
    out = set.front();
    return true;
}

// Callback guard shared between an lp handle and its in-flight callbacks.
// Invocations hold the mutex while calling user code; teardown takes the
// mutex and clears `alive`, so once lp_client_destroy / lp_unsubscribe
// returns, no further user callback can fire (the cancellation contract in
// logos_protocol.h). recursive_mutex so a callback may itself unsubscribe.
struct CbGuard {
    std::recursive_mutex mutex;
    bool alive = true;
};

Timeout lpTimeout(int timeout_ms)
{
    return timeout_ms > 0 ? Timeout(timeout_ms) : Timeout();
}

// Parse args_json (NULL → empty array) into a QVariantList.
// Returns false (+fills error) when args_json is not a JSON array.
bool parseArgs(const char* args_json, const QString& origin,
               QVariantList& out, std::string& error)
{
    if (!args_json || !*args_json) return true;
    nlohmann::json parsed = nlohmann::json::parse(args_json, nullptr,
                                                  /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        error = makeErrorJson("invalid_args",
                              "args_json must be a JSON array",
                              origin.toStdString());
        return false;
    }
    out = logos::nlohmannArgsToQVariantList(parsed);
    return true;
}

} // namespace

struct lp_client {
    LogosAPIClient* client = nullptr;
    QString target;
    QString origin;
    std::shared_ptr<CbGuard> guard;
};

struct lp_subscription {
    std::shared_ptr<CbGuard> guard;
    // Enough to un-register from the consumer's pending registry on
    // lp_unsubscribe. The client guard is what makes that safe: a subscription
    // can outlive lp_client_destroy, and dereferencing `owner` then would be a
    // use-after-free.
    LogosAPIClient* owner = nullptr;
    std::shared_ptr<CbGuard> ownerGuard;
    quint64 id = 0;
};

struct lp_provider {
    std::string moduleName;
    std::string transportSetJson;
    lp_dispatch_cb dispatch = nullptr;
    lp_getmethods_cb getMethods = nullptr;
    lp_token_cb onToken = nullptr;
    void* userData = nullptr;
};

extern "C" {

/* ---------------------------------------------------------------- version */

const char* lp_protocol_version(void)
{
    return LOGOS_PROTOCOL_VERSION_STRING;
}

int lp_protocol_abi_major(void)
{
    return LOGOS_PROTOCOL_VERSION_MAJOR;
}

/* ----------------------------------------------------------------- memory */

void lp_string_free(char* s)
{
    std::free(s);
}

/* ----------------------------------------------------- mode / transports */

int lp_set_mode(const char* mode)
{
    if (!mode) return LP_ERR_INVALID_ARG;
    if (std::strcmp(mode, "remote") == 0) {
        LogosModeConfig::setMode(LogosMode::Remote);
    } else if (std::strcmp(mode, "local") == 0) {
        LogosModeConfig::setMode(LogosMode::Local);
    } else if (std::strcmp(mode, "mock") == 0) {
        LogosModeConfig::setMode(LogosMode::Mock);
    } else {
        return LP_ERR_INVALID_ARG;
    }
    return LP_OK;
}

const char* lp_get_mode(void)
{
    switch (LogosModeConfig::getMode()) {
    case LogosMode::Local: return "local";
    case LogosMode::Mock:  return "mock";
    case LogosMode::Remote: break;
    }
    return "remote";
}

int lp_set_default_transport(const char* transport_json)
{
    if (!transport_json) return LP_ERR_INVALID_ARG;
    LogosTransportConfig cfg;
    if (!parseTransportJson(transport_json, cfg)) return LP_ERR_INVALID_ARG;
    LogosTransportConfigGlobal::setDefault(cfg);
    return LP_OK;
}

/* ---------------------------------------------------------------- clients */

lp_client* lp_client_create(const char* target_module,
                            const char* origin_module,
                            const char* target_transport_json,
                            const char* capability_transport_json)
{
    if (!target_module || !*target_module || !origin_module) return nullptr;

    LogosTransportConfig targetCfg;
    LogosTransportConfig capabilityCfg;
    if (!parseTransportJson(target_transport_json, targetCfg)) return nullptr;
    if (!parseTransportJson(capability_transport_json, capabilityCfg)) return nullptr;

    // Same registration LogosAPI's constructor performs — lp-only consumers
    // never construct a LogosAPI, so do it here (idempotent).
    qRegisterMetaType<LogosResult>("LogosResult");

    auto* handle = new lp_client();
    handle->target = QString::fromUtf8(target_module);
    handle->origin = QString::fromUtf8(origin_module);
    handle->guard = std::make_shared<CbGuard>();

    // No QObject parent: the handle owns the client.
    //
    // Construction picks the owner thread every later call marshals onto
    // (logos::runOnOwnerThread), and for a Qt-affine transport it also picks
    // the thread that owns the QRemoteObjectNode and its QLocalSocket. Those
    // only work on a thread running a Qt event loop, so we construct on the Qt
    // main thread rather than on whichever thread happened to call first.
    //
    // Callers reach lp_client_create through a lazily-created wrapper (the
    // generated bind_<iface>() → LpClient::ensure()), so "whichever thread
    // called first" is genuinely arbitrary: a module whose first outbound call
    // comes from an HTTP handler used to bind its whole transport to that
    // worker thread. The worker only pumps events while blocked inside a call,
    // so replica acquisition never completed and every call burned its full
    // 20s timeout — silently, since a failed acquire returns an empty result.
    // The Qt path never had this: LogosAPI::getClient marshals construction to
    // the LogosAPI's thread, which is the main thread. This gives the lp path
    // the same anchor.
    //
    // Plain (Tcp/TcpSsl) and mock transports are Qt-free and thread-agnostic —
    // they keep the calling thread, so a worker-thread consumer stays off the
    // main thread's back.
    const bool qtAffine = LogosTransportFactory::needsQtEventLoop(targetCfg)
                       || LogosTransportFactory::needsQtEventLoop(capabilityCfg);
    auto construct = [&]() -> LogosAPIClient* {
        return new LogosAPIClient(handle->target, handle->origin,
                                  &TokenManager::instance(),
                                  targetCfg, capabilityCfg);
    };
    if (qtAffine && !QCoreApplication::instance()) {
        // Nothing to anchor to. The transport will misbehave for the reasons
        // above; say so once rather than let it surface as a mute timeout.
        qWarning() << "lp_client_create: creating a Qt-affine client for"
                   << handle->target
                   << "with no QCoreApplication — the QtRO transport needs a "
                      "Qt event loop; use a plain (tcp) transport in Qt-free "
                      "hosts";
    }
    handle->client = qtAffine ? logos::runOnQtMainThread(construct) : construct();
    return handle;
}

void lp_client_destroy(lp_client* client)
{
    if (!client) return;
    {
        // Block until no user callback is mid-flight, then forbid new ones.
        std::lock_guard<std::recursive_mutex> lock(client->guard->mutex);
        client->guard->alive = false;
    }
    // The client and its consumers own Qt transport objects (for QtRO: a node
    // and its QLocalSocket, with socket notifiers) that belong to the owner
    // thread. Destroying them from another thread makes Qt disable a notifier
    // cross-thread and closes the fd under the owner's event dispatcher, which
    // faults. Foreign-thread destroys are real: any binding that keeps a client
    // share inside a worker (an event subscription moved into a Rust worker
    // thread, say) runs this on that worker when the last share drops.
    //
    // deleteLater() hands the destruction to the owner thread, matching the
    // marshaling every call path already does (logos::runOnOwnerThread). A
    // *blocking* marshal is not usable here: the owner thread is typically the
    // module's dispatch thread, and it may be blocked joining the very worker
    // running this destroy — that would deadlock. Deferring instead is
    // invisible to callers because the guard above, not the delete, is what
    // enforces the ABI's "no callbacks after this returns" contract.
    //
    // If the owner's event loop never runs again (a process already tearing
    // down), the deferred delete never fires and the client leaks. That is the
    // deliberate trade: a leak at exit beats a crash.
    if (client->client) {
        if (client->client->thread() == QThread::currentThread())
            delete client->client;
        else
            client->client->deleteLater();
    }
    delete client;
}

/* ----------------------------------------------------------------- invoke */

int lp_invoke(lp_client* client,
              const char* method,
              const char* args_json,
              int timeout_ms,
              char** out_result_json,
              char** out_error_json)
{
    if (out_result_json) *out_result_json = nullptr;
    if (out_error_json) *out_error_json = nullptr;
    if (!client || !client->client || !method || !*method) {
        if (out_error_json)
            *out_error_json = lpStrdup(makeErrorJson(
                "invalid_arg", "client and method are required", ""));
        return LP_ERR_INVALID_ARG;
    }

    QVariantList args;
    std::string error;
    if (!parseArgs(args_json, client->origin, args, error)) {
        if (out_error_json) *out_error_json = lpStrdup(error);
        return LP_ERR_INVALID_ARG;
    }

    logos::CallError callErr;
    const QVariant result = client->client->invokeRemoteMethod(
        client->target, QString::fromUtf8(method), args, lpTimeout(timeout_ms), &callErr);

    if (!callErr.ok()) {
        if (out_error_json)
            *out_error_json = lpStrdup(makeErrorJson(
                callErr.code.c_str(), callErr.message, callErr.origin));
        return LP_ERR_UNAVAILABLE;
    }

    if (out_result_json)
        *out_result_json = lpStrdup(logos::qvariantToNlohmann(result).dump());
    return LP_OK;
}

int lp_invoke_async(lp_client* client,
                    const char* method,
                    const char* args_json,
                    int timeout_ms,
                    lp_result_cb cb,
                    void* user_data)
{
    if (!client || !client->client || !method || !*method || !cb)
        return LP_ERR_INVALID_ARG;

    QVariantList args;
    std::string error;
    if (!parseArgs(args_json, client->origin, args, error))
        return LP_ERR_INVALID_ARG;

    std::shared_ptr<CbGuard> guard = client->guard;
    // A TWO-argument lambda: invocable only as LogosAPIClient's
    // AsyncResultErrorCallback, so it binds to the CallError-aware overload and
    // never to the value-only one sitting next to it. That overload is what
    // makes `ok == 0` reachable at all — this used to subscribe with the
    // value-only one and hard-code cb(1, ...), so a call to a module that is
    // not loaded reached the callback as a SUCCESS carrying a default value,
    // contradicting both lp_result_cb's documented contract and the sync twin
    // lp_invoke (which returns LP_ERR_UNAVAILABLE + out_error_json).
    //
    // The failure shape is deliberately identical to lp_invoke's
    // out_error_json — the same makeErrorJson({code, message, origin}) — so the
    // two entry points report the same event the same way, and a caller can
    // parse one decoder for both.
    client->client->invokeRemoteMethodAsync(
        client->target, QString::fromUtf8(method), args,
        [guard, cb, user_data](QVariant result, const logos::CallError& err) {
            std::lock_guard<std::recursive_mutex> lock(guard->mutex);
            if (!guard->alive) return;  // client destroyed: drop the result
            if (!err.ok()) {
                const std::string json = makeErrorJson(err.code.c_str(),
                                                       err.message, err.origin);
                cb(0, json.c_str(), user_data);
                return;
            }
            const std::string json = logos::qvariantToNlohmann(result).dump();
            cb(1, json.c_str(), user_data);
        },
        lpTimeout(timeout_ms));
    return LP_OK;
}

/* ------------------------------------------------------------- subscribe */

lp_subscription* lp_subscribe(lp_client* client,
                              const char* event_name,
                              lp_event_cb cb,
                              void* user_data)
{
    if (!client || !client->client || !event_name || !*event_name || !cb)
        return nullptr;

    // Deliberately NOT requestObject() + onEvent().
    //
    // That pair asks "is the target module reachable at this instant?", and
    // every caller that reaches here asks it at the worst possible instant: a
    // module's init(), a UI backend's onContextReady(), a generated
    // `dep.onSomething(...)` wrapper — all of which run while the dependency's
    // host process has been spawned but has not called listen() yet. The old
    // code returned nullptr there, the generated wrapper turned that into a
    // `false` its documented example discards, and the subscription was never
    // attempted again for the life of the process: method calls worked, events
    // silently never arrived.
    //
    // onEventWhenAvailable() returns a handle that arms when the module shows
    // up (including a mid-session install), warns once when it defers, logs
    // when it arms, and says so loudly if it ever becomes impossible.
    auto* sub = new lp_subscription();
    sub->guard = std::make_shared<CbGuard>();
    sub->owner = client->client;
    sub->ownerGuard = client->guard;

    std::shared_ptr<CbGuard> subGuard = sub->guard;
    std::shared_ptr<CbGuard> clientGuard = client->guard;
    sub->id = client->client->onEventWhenAvailable(
        client->target, QString::fromUtf8(event_name),
        [subGuard, clientGuard, cb, user_data](const QString& name,
                                               const QVariantList& data) {
            std::lock_guard<std::recursive_mutex> subLock(subGuard->mutex);
            if (!subGuard->alive) return;  // unsubscribed
            std::lock_guard<std::recursive_mutex> clientLock(clientGuard->mutex);
            if (!clientGuard->alive) return;  // client destroyed
            nlohmann::json payload = nlohmann::json::array();
            for (const QVariant& v : data)
                payload.push_back(logos::qvariantToNlohmann(v));
            const std::string json = payload.dump();
            const QByteArray nameUtf8 = name.toUtf8();
            cb(nameUtf8.constData(), json.c_str(), user_data);
        });

    if (!sub->id) {
        // The consumer refused the arguments (empty object/event name, or a
        // null callback). Returning the handle anyway would hand the caller
        // something that can never fire, while the ABI documents NULL as the
        // one signal that the arguments were refused — a silent dead
        // subscription, which is the exact failure this whole change removes.
        //
        // Defensive, and not reachable today: the guard at the top of this
        // function already rejects an empty event name and a null callback, and
        // lp_client_create rejects an empty target, so the three inputs that
        // make onEventWhenAvailable() return 0 cannot all arrive here. Hence no
        // test drives it — the two contracts simply have to agree, and one of
        // them changing is how they would stop agreeing.
        delete sub;
        return nullptr;
    }
    return sub;
}

void lp_unsubscribe(lp_subscription* sub)
{
    if (!sub) return;
    {
        // The underlying transport keeps its listener; this guard makes it
        // inert, which is what the ABI promises ("the callback will not
        // fire again").
        std::lock_guard<std::recursive_mutex> lock(sub->guard->mutex);
        sub->guard->alive = false;
    }
    // Stop the consumer tracking it too. Without this an unsubscribed-while-
    // pending subscription stays in the registry forever: it holds the retry
    // timer up, keeps emitting the 3 s / 60 s "still not reachable" warnings
    // about a subscription nobody wants, and shows up in the
    // pendingEventSubscriptions() diagnostics this whole change relies on for
    // its own credibility.
    if (sub->id && sub->owner && sub->ownerGuard) {
        // POSTED, and deliberately NOT under ownerGuard->mutex.
        //
        // cancelEventSubscription() marshals to the owner thread with a
        // BLOCKING queued connection, and the delivery callback installed by
        // lp_subscribe takes this very mutex ON that thread (ownerGuard and the
        // callback's clientGuard are the same CbGuard). Taking it here and then
        // waiting for the owner thread is a lock-order inversion that
        // deadlocks; and it hangs outright once the owner's event loop has
        // stopped, which is exactly when a Rust EventSubscription drops.
        //
        // Posting instead: the lambda runs ON the owner thread, so the marshal
        // inside cancelEventSubscription() becomes a direct call, and taking
        // the guard there cannot wait on anyone. Qt drops posted events for a
        // destroyed QObject, so a client torn down before delivery simply means
        // the cancel never runs — which is correct, since the registry died
        // with it.
        auto ownerGuard = sub->ownerGuard;
        LogosAPIClient* owner = sub->owner;
        const quint64 id = sub->id;
        std::lock_guard<std::recursive_mutex> ownerLock(ownerGuard->mutex);
        if (ownerGuard->alive) {
            // The guard is held across the POST but not across the cancel.
            // That distinction is the whole fix, and both halves are load-bearing:
            //
            //  - It must be HELD here, because QMetaObject::invokeMethod
            //    dereferences `owner` (it reads object->thread()) before the
            //    lambda can run, so an `alive` check inside the lambda is
            //    unreachable — lp_client_destroy sets alive=false and deletes
            //    the client synchronously, and this struct's own contract says
            //    a subscription may outlive it. Checking inside was a
            //    use-after-free.
            //  - It must NOT be held across cancelEventSubscription(), which
            //    marshals to the owner thread with a BLOCKING queued connection
            //    while that thread's delivery callback takes this same mutex —
            //    a lock-order inversion that deadlocks, and hangs outright once
            //    that event loop has stopped.
            //
            // Posting never waits on the owner thread, so holding the mutex
            // across it cannot invert. Only the blocking marshal had to move.
            QMetaObject::invokeMethod(owner, [ownerGuard, owner, id]() {
                std::lock_guard<std::recursive_mutex> lock(ownerGuard->mutex);
                if (ownerGuard->alive)
                    owner->cancelEventSubscription(id);
            }, Qt::QueuedConnection);
        }
    }
    delete sub;
}

char* lp_pending_subscriptions(lp_client* client)
{
    if (!client || !client->client) return nullptr;
    nlohmann::json out = nlohmann::json::array();
    for (const QString& entry : client->client->pendingEventSubscriptions())
        out.push_back(entry.toStdString());
    return lpStrdup(out.dump());
}

/* ------------------------------------------------------------ introspect */

char* lp_get_methods(lp_client* client)
{
    if (!client || !client->client) return nullptr;
    LogosObject* object = client->client->requestObject(client->target);
    if (!object) return nullptr;
    const QJsonArray methods = object->getMethods();
    const QByteArray json =
        QJsonDocument(methods).toJson(QJsonDocument::Compact);
    return lpStrdup(std::string(json.constData(),
                                static_cast<size_t>(json.size())));
}

/* ----------------------------------------------------------------- tokens */

char* lp_token_get(const char* module_name)
{
    if (!module_name) return nullptr;
    const QString token =
        TokenManager::instance().getToken(QString::fromUtf8(module_name));
    if (token.isEmpty()) return nullptr;
    return lpStrdup(token.toStdString());
}

int lp_token_save(const char* module_name, const char* token)
{
    if (!module_name || !token) return LP_ERR_INVALID_ARG;
    TokenManager::instance().saveToken(QString::fromUtf8(module_name),
                                       QString::fromUtf8(token));
    return LP_OK;
}

int lp_inform_module_token(lp_client* client,
                           const char* auth_token,
                           const char* module_name,
                           const char* token)
{
    if (!client || !client->client || !auth_token || !module_name || !token)
        return LP_ERR_INVALID_ARG;
    const bool ok = client->client->informModuleToken(
        QString::fromUtf8(auth_token), QString::fromUtf8(module_name),
        QString::fromUtf8(token));
    return ok ? LP_OK : LP_ERR_INTERNAL;
}

/* -------------------------------------------------- provider (groundwork) */

lp_provider* lp_provider_create(const char* module_name,
                                const char* transport_set_json)
{
    if (!module_name || !*module_name) return nullptr;
    auto* provider = new lp_provider();
    provider->moduleName = module_name;
    provider->transportSetJson =
        transport_set_json ? transport_set_json : "[]";
    return provider;
}

void lp_provider_destroy(lp_provider* provider)
{
    delete provider;
}

int lp_provider_register(lp_provider* provider,
                         lp_dispatch_cb dispatch,
                         lp_getmethods_cb get_methods,
                         lp_token_cb on_token,
                         void* user_data)
{
    if (!provider || !dispatch) return LP_ERR_INVALID_ARG;
    provider->dispatch = dispatch;
    provider->getMethods = get_methods;
    provider->onToken = on_token;
    provider->userData = user_data;
    return LP_OK;
}

int lp_provider_emit_event(lp_provider* provider,
                           const char* event_name,
                           const char* data_json)
{
    (void)event_name;
    (void)data_json;
    if (!provider) return LP_ERR_INVALID_ARG;
    // Groundwork only: serving a provider over the transports through the
    // C ABI lands with the common cdylib module-impl ABI (module authoring
    // phase). The registered callbacks above define the contract today.
    return LP_ERR_UNSUPPORTED;
}

int lp_provider_save_token(lp_provider* provider,
                           const char* module_name,
                           const char* token)
{
    (void)module_name;
    (void)token;
    if (!provider) return LP_ERR_INVALID_ARG;
    return LP_ERR_UNSUPPORTED;
}

} // extern "C"
