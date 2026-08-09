#ifndef LOGOS_API_CONSUMER_H
#define LOGOS_API_CONSUMER_H

#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QHash>
#include <QSet>
#include <QMap>
#include <QStringList>
#include <functional>
#include <memory>
#include <string>

#include "logos_call_error.h"
#include "logos_mode.h"
#include "logos_subscription_state.h"
#include "logos_transport_config.h"

class LogosTransportConnection;
class LogosObject;
class TokenManager;
class LogosPendingSubscriptions;

/**
 * @brief LogosAPIConsumer handles connecting to module objects and invoking their methods
 * 
 * This class is responsible for the consumer/client side functionality:
 * - Connecting to module registries via the transport layer
 * - Requesting LogosObject handles
 * - Invoking methods on objects
 * - Handling events from objects
 */
class LogosAPIConsumer : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Construct a consumer connected via `transport`, honoring the
     * process-wide LogosMode.
     *
     * Transport resolution is done in one place — LogosTransportFactory —
     * by combining LogosMode + the supplied LogosTransportConfig:
     *   - LogosMode::Mock  → MockTransportConnection  (transport ignored)
     *   - LogosMode::Local → LocalTransportConnection (transport ignored)
     *   - LogosMode::Remote → wire protocol picked by `transport.protocol`
     *
     * Use this overload when the caller wants a specific transport for
     * this consumer without side-effecting the rest of the process
     * (e.g. the logoscore CLI dialing `core_service` over tcp_ssl
     * without also flipping the in-process LogosAPIProvider into
     * binding TLS).
     */
    LogosAPIConsumer(const QString& module_to_talk_to,
                     const QString& origin_module,
                     TokenManager* token_manager,
                     const LogosTransportConfig& transport,
                     QObject *parent = nullptr);

    /**
     * @brief Convenience constructor that uses the process-global default
     * LogosTransportConfig. Equivalent to passing
     * `LogosTransportConfigGlobal::getDefault()` to the explicit-transport
     * constructor above.
     */
    explicit LogosAPIConsumer(const QString& module_to_talk_to,
                              const QString& origin_module,
                              TokenManager* token_manager,
                              QObject *parent = nullptr);
    ~LogosAPIConsumer();

    /**
     * @brief Request a LogosObject handle by name
     * @return LogosObject* handle, or nullptr if failed. Caller must call release() when done.
     */
    LogosObject* requestObject(const QString& objectName, Timeout timeout = Timeout());

    bool isConnected() const;
    QString registryUrl() const;
    bool reconnect();

    QVariant invokeRemoteMethod(const QString& authToken, const QString& objectName, const QString& methodName,
                             const QVariantList& args = QVariantList(), Timeout timeout = Timeout());

    /**
     * @brief invokeRemoteMethod with an explicit error out-channel.
     *
     * Fills *err with the canonical {code, message, origin} call error when
     * the failure is detectable on this side (today: "object_unavailable"
     * when the target object/replica cannot be acquired). On success *err is
     * cleared. Failures the transport cannot yet distinguish from a void
     * result (per-dispatch errors) leave *err clear — the struct is the
     * extension point for surfacing transport-level statuses later.
     */
    QVariant invokeRemoteMethod(const QString& authToken, const QString& objectName, const QString& methodName,
                             const QVariantList& args, Timeout timeout, logos::CallError* err);

    using AsyncResultCallback = std::function<void(QVariant)>;

    /**
     * @brief Async result callback with an explicit error channel.
     *
     * Mirrors the sync `invokeRemoteMethod(..., CallError*)` overload:
     * the callback receives the same {code, message, origin} error struct,
     * so callers can distinguish "the module's source could not be acquired"
     * from "the call ran but returned an invalid QVariant". On success the
     * error is cleared (ok() == true).
     */
    using AsyncResultErrorCallback = std::function<void(QVariant, const logos::CallError&)>;

    /**
     * @brief Invoke a remote method asynchronously; result is delivered via callback
     * @param authToken Authentication token for the operation
     * @param objectName The name of the remote object
     * @param methodName The name of the method to call
     * @param args Arguments to pass to the method
     * @param callback Called when the call completes (on the caller's thread via QueuedConnection)
     * @param timeout Timeout for replica acquisition and for the remote call
     */
    void invokeRemoteMethodAsync(const QString& authToken, const QString& objectName, const QString& methodName,
                                 const QVariantList& args,
                                 AsyncResultCallback callback,
                                 Timeout timeout = Timeout());

    /**
     * @brief invokeRemoteMethodAsync with an explicit error out-channel.
     *
     * Sets the CallError to code="object_unavailable" when acquire fails
     * (matching the sync overload's semantics — see logos_call_error.h),
     * cleared on success. Callers that need to react to acquire failure
     * differently from "call returned no value" should use this overload.
     */
    void invokeRemoteMethodAsync(const QString& authToken, const QString& objectName, const QString& methodName,
                                 const QVariantList& args,
                                 AsyncResultErrorCallback callback,
                                 Timeout timeout = Timeout());

    /**
     * @brief Register an event listener via LogosObject's callback mechanism
     * @param originObject The LogosObject that will emit the event
     * @param eventName The name of the event to listen for
     * @param callback Function to call when the event is triggered
     */
    void onEvent(LogosObject* originObject, const QString& eventName,
                std::function<void(const QString&, const QVariantList&)> callback);

    /**
     * @brief Subscribe to an event on an object that MAY NOT EXIST YET.
     *
     * requestObject() + onEvent() asks "is the module there right now?" — the
     * wrong question for a subscription. A UI plugin subscribes while its
     * dependency's host process has been spawned but has not yet called
     * listen(), so the honest answer is "no", and a one-shot caller then never
     * asks again. Method calls kept working through this window only because
     * they reach the replica by a path that does not ask.
     *
     * This arms the subscription as soon as the object is reachable — now, or
     * whenever the module appears, including a mid-session package install.
     * It never blocks and never spins a nested event loop, so it is safe from a
     * GUI thread.
     *
     * Cost of waiting: on the qt_remote transport, ZERO extra polling — one
     * pending QRemoteObjectDynamicReplica, armed by the node's existing 250 ms
     * reconnect loop. On transports without deferred acquire (qt_local, mock,
     * plain) this runs ONE shared timer per consumer with 250 ms → 5 s backoff,
     * whose per-tick cost is a registry hash lookup / in-memory socket check.
     *
     * Unbounded on purpose: any finite give-up would silently break the
     * mid-session-install case. What IS bounded is the NOISE — one warning per
     * (object, event) when a subscription is first deferred, one more if it is
     * still pending after 60 s, then quiet; a qInfo when it finally arms; and a
     * loud warning if it becomes permanently impossible.
     *
     * All subscriptions to the same object share ONE handle (one replica),
     * separate from the call-path handle cache so that a call re-acquiring a
     * stale handle cannot silently kill a live subscription.
     *
     * NOT deduplicated: two identical calls produce two live subscriptions and
     * therefore two callbacks per event. Callers that must not double-deliver
     * (e.g. QML re-running Component.onCompleted) dedupe on their own side, and
     * should verify with eventSubscriptionState() rather than assuming their
     * own record is still accurate.
     *
     * WHAT THIS DOES NOT PROMISE. Arming is not retroactive and the transports
     * do not buffer, so there is a window — roughly the 50-150 ms between a
     * module's socket appearing and the replica reaching Valid — in which an
     * event the module emits is not delivered to anyone. A module that fires a
     * one-shot "ready"/"started" event synchronously inside its own init() can
     * still be missed. This is inherent to the transport, not introduced here
     * (the blocking requestObject() this replaced had exactly the same window),
     * but "subscriptions survive a late module" is not "no event can be
     * missed": a module whose startup event matters must also expose a pull
     * method the subscriber can call after arming.
     *
     * @param onArmed Optional; called with true the moment the subscription
     *                goes live, or false if it is abandoned. Never called for
     *                "not yet".
     * @return A non-zero id for cancelEventSubscription() /
     *         eventSubscriptionState(), or 0 if the arguments were refused.
     */
    quint64 onEventWhenAvailable(const QString& objectName,
                                 const QString& eventName,
                                 std::function<void(const QString&, const QVariantList&)> callback,
                                 std::function<void(bool)> onArmed = {});

    /**
     * @brief Stop tracking the subscription with this id.
     *
     * A subscription that is still PENDING leaves the registry entirely, so it
     * stops holding the retry timer up and stops the watchdog warning about a
     * subscription nobody wants. One that has already ARMED is dropped from the
     * re-arm set so a later reconnect does not resurrect it.
     *
     * Does NOT detach the callback from the shared handle — LogosObject offers
     * no per-callback removal, only clearEventSubscriptions(), which would take
     * out every other subscriber on that handle. A caller that must stop
     * delivery gates its own callback (lp_unsubscribe does).
     *
     * @return true if the id was known.
     */
    bool cancelEventSubscription(quint64 subscriptionId);

    /**
     * @brief Whether a subscription id is still pending, armed, or forgotten.
     *
     * Lets a caller that keeps its own de-duplication record check it against
     * the registry instead of trusting it — an assumed-live record that is
     * actually gone turns a re-subscribe into a silent no-op.
     */
    LogosSubscriptionState eventSubscriptionState(quint64 subscriptionId) const;

    /**
     * @brief Diagnostics: "<object>::<event>" for every subscription registered
     *        via onEventWhenAvailable() that has not armed yet.
     */
    QStringList pendingSubscriptions() const;

public slots:
    bool informModuleToken(const QString& authToken, const QString& moduleName, const QString& token);
    // Delivers a token to `originModule`, preferring its handshake surface (see
    // logos::handshakeObjectName) so a target that is still running its
    // initializer is still reachable; falls back to the business object for
    // modules built before that surface existed. timeoutMs bounds the fallback
    // acquire and the call; the default preserves the historical 20s.
    bool informModuleToken_module(const QString& authToken, const QString& originModule, const QString& moduleName, const QString& token, int timeoutMs = 20000);
    // timeoutMs bounds the whole handshake: the capability_module acquire plus
    // the requestModule call on it share one deadline. Defaulted so existing
    // callers are source-compatible and keep today's behaviour; pass the
    // caller's own budget to make a short bound real (see the definition).
    std::string requestModule(const std::string& authToken, const std::string& originModule, const std::string& targetModule, int timeoutMs = 20000);

private:
    // Deliver via the target's business object (the pre-handshake path). Used
    // when the target publishes no handshake surface, and when its handshake
    // surface refused the push because the target is still initializing.
    // Deliberately NOT a slot: it is an internal step of informModuleToken_module,
    // not a separate remote entry point.
    bool informModuleTokenViaBusinessObject(const QString& authToken, const QString& originModule, const QString& moduleName, const QString& token, int timeoutMs);

    // Get a cached remote-object handle for objectName, (re)acquiring via the
    // transport if absent or stale. Acquiring a QtRO replica (acquireDynamic +
    // waitForSource) is expensive, so invokeRemoteMethod reuses one handle per
    // object instead of acquiring + release()ing on every call.
    LogosObject* acquireCachedObject(const QString& objectName, int timeoutMs);
    // Release and drop every cached handle (destructor / reconnect).
    void clearObjectCache();

    std::unique_ptr<LogosTransportConnection> m_transport;
    QString m_registryUrl;
    QMap<QString, QString> m_tokens;
    TokenManager* m_token_manager;
    // Object-handle cache keyed by object name. Single-threaded: touched only on
    // the consumer's event-loop thread.
    QHash<QString, LogosObject*> m_objectCache;
    // Handshake object names known to be absent. acquireCachedObject caches only
    // successes, so without this a module built before the handshake surface
    // existed would pay the full probe budget on every single grant. Cleared by
    // clearObjectCache() so a reconnect or a reloaded module is re-probed.
    QSet<QString> m_noHandshakeSurface;

    // Deferred event subscriptions (onEventWhenAvailable). Appended LAST and
    // held by pointer on purpose: an opaque forward declaration keeps this
    // header's size stable and keeps every piece of the registry's state as
    // instance state. It must never become a function-local or file-scope
    // static in a header — PE has no symbol interposition, so a static inside
    // an inline function is per-IMAGE, and Basecamp would get one registry per
    // DLL (the same shape as the three-TokenManagers bug). Owned; deleted in
    // the destructor. Defined in logos_api_consumer.cpp.
    LogosPendingSubscriptions* m_pendingSubs = nullptr;
};

#endif // LOGOS_API_CONSUMER_H
