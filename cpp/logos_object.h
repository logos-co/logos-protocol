#ifndef LOGOS_OBJECT_H
#define LOGOS_OBJECT_H

#include "logos_call_error.h"

#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QJsonArray>
#include <functional>
#include <cstdint>

/**
 * @brief Abstract interface for a module object handle.
 *
 * LogosObject decouples callers from the underlying transport mechanism.
 * Each transport (local/Qt Remote Objects/mock/JSON-RPC/...) provides its
 * own concrete subclass.  Callers interact exclusively through this
 * interface and never need to know the implementation type.
 */
class LogosObject {
public:
    virtual ~LogosObject() = default;

    /**
     * @brief Invoke a method on the remote/local module.
     * @param authToken Authentication token for the operation
     * @param methodName Method to call on the underlying module
     * @param args Arguments for the method
     * @param timeoutMs Maximum time to wait for the result
     * @return The method result, or an invalid QVariant on failure
     */
    virtual QVariant callMethod(const QString& authToken,
                                const QString& methodName,
                                const QVariantList& args,
                                int timeoutMs) = 0;

    using AsyncResultCallback = std::function<void(QVariant)>;

    /**
     * @brief Invoke a method asynchronously; result is delivered via callback.
     *
     * Returns immediately. The callback is always invoked on a subsequent
     * event-loop iteration, never synchronously inside this call.
     *
     * @param authToken Authentication token for the operation
     * @param methodName Method to call on the underlying module
     * @param args Arguments for the method
     * @param timeoutMs Maximum time to wait for the result
     * @param callback Called with the result (invalid QVariant on failure/timeout)
     */
    virtual void callMethodAsync(const QString& authToken,
                                 const QString& methodName,
                                 const QVariantList& args,
                                 int timeoutMs,
                                 AsyncResultCallback callback) = 0;

    /**
     * @brief Deliver a module token to the underlying module.
     * @param authToken Authentication token for the operation
     * @param moduleName Target module name
     * @param token The token to deliver
     * @param timeoutMs Maximum time to wait for the result
     * @return true if the token was delivered successfully
     */
    virtual bool informModuleToken(const QString& authToken,
                                   const QString& moduleName,
                                   const QString& token,
                                   int timeoutMs) = 0;

    using EventCallback = std::function<void(const QString&, const QVariantList&)>;

    /**
     * @brief Subscribe to events from this object.
     *
     * Qt-based implementations use QObject::connect internally;
     * other implementations may use a different mechanism.
     *
     * @param eventName The event name to listen for
     * @param callback  Called when the event fires
     */
    virtual void onEvent(const QString& eventName, EventCallback callback) = 0;

    /**
     * @brief Remove all event subscriptions made via onEvent().
     */
    virtual void disconnectEvents() = 0;

    /**
     * @brief Emit an event on this object.
     *
     * For Qt-based implementations this triggers the underlying
     * QObject signal so that Qt Remote Objects can replicate it.
     *
     * @param eventName The event name
     * @param data      Event payload
     */
    virtual void emitEvent(const QString& eventName, const QVariantList& data) = 0;

    /**
     * @brief Return introspection data for the methods exposed by
     *        the underlying module.
     */
    virtual QJsonArray getMethods() = 0;

    /**
     * @brief Release resources associated with this handle.
     *
     * After calling release() the object must not be used again.
     * Implementations that own the underlying resource (e.g. a
     * QRemoteObjectReplica) will delete it here.
     */
    virtual void release() = 0;

    /**
     * @brief Stable identity value suitable for use as a hash key.
     */
    virtual quintptr id() const = 0;

    /**
     * @brief Whether this handle is still usable for calls.
     *
     * A cached handle can go stale (e.g. its QRemoteObjectReplica lost its
     * source when the target module unloaded). Callers that keep a handle
     * across calls should re-acquire when this returns false. Non-owning or
     * always-live implementations may keep the default.
     */
    virtual bool isValid() const { return true; }
};

/**
 * @brief Optional extension: calls that report WHY they failed.
 *
 * LogosObject's own callMethod/callMethodAsync answer a bare QVariant() for
 * every failure — a timeout, a torn-down connection, and a module that is not
 * published all look identical to a provider that legitimately returned null.
 * That is the whole reason lp_invoke and lp_invoke_async could report success
 * for a call that never happened.
 *
 * This interface is DELIBERATELY a sibling of LogosObject rather than more
 * virtuals on it. LogosObject is an installed header (`include/logos_object.h`)
 * whose vtable is baked into every statically-linked copy of liblogos_protocol
 * in a process — one per loaded module, each pinned to its own protocol
 * revision. Appending a virtual would append a vtable slot, and a caller
 * compiled against the new header calling that slot on an object whose vtable
 * came from an older copy is undefined behaviour. Declaring a separate
 * interface and reaching it with dynamic_cast leaves LogosObject's layout,
 * size and vtable byte-for-byte unchanged, so no such pairing can exist:
 * a copy that does not know about this interface simply fails the cast.
 *
 * Consumers therefore MUST treat it as optional:
 *
 *     if (auto* ch = dynamic_cast<LogosObjectErrorChannel*>(obj))
 *         ch->callMethodWithError(...);     // real diagnosis
 *     else
 *         obj->callMethod(...);             // today's behaviour, unchanged
 *
 * Implemented by the plain (tcp/tcp_ssl), qt_remote (QtRO) and qt_local
 * transports. NOT implemented by the mock transport: MockStore always answers,
 * so there is no failure to report, and leaving MockLogosObject alone keeps the
 * one subclass whose header is installed (implementations/mock/mock_transport.h)
 * layout-identical too.
 */
class LogosObjectErrorChannel {
public:
    virtual ~LogosObjectErrorChannel() = default;

    /**
     * @brief callMethod, plus the reason on failure.
     * @param err Cleared on entry; set to the canonical {code, message, origin}
     *            on failure. May be null (then this is exactly callMethod).
     * @return The method result, or an invalid QVariant on failure.
     */
    virtual QVariant callMethodWithError(const QString& authToken,
                                         const QString& methodName,
                                         const QVariantList& args,
                                         int timeoutMs,
                                         logos::CallError* err) = 0;

    using AsyncResultErrorCallback =
        std::function<void(QVariant, const logos::CallError&)>;

    /**
     * @brief callMethodAsync, whose callback carries the reason on failure.
     *
     * Same delivery contract as LogosObject::callMethodAsync: the callback
     * fires on a subsequent event-loop iteration, never synchronously, and
     * exactly once. On success the error argument is a default-constructed
     * (ok()) CallError.
     */
    virtual void callMethodAsyncWithError(const QString& authToken,
                                          const QString& methodName,
                                          const QVariantList& args,
                                          int timeoutMs,
                                          AsyncResultErrorCallback callback) = 0;
};

#endif // LOGOS_OBJECT_H
