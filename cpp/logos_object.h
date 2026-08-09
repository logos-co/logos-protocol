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
 *
 * THREAD SAFETY, precisely — the two halves are not the same answer.
 *
 *   * CALLS are safe to make from several threads at once. The transports
 *     serialize what has to be serialized internally.
 *
 *   * release() IS NOT SAFE AGAINST A CONCURRENT CALL AT THIS LEVEL. release()
 *     destroys the object, so a call still executing on another thread is left
 *     dereferencing freed memory. Write callers as if that is always true: every
 *     call on a handle must have RETURNED before release() is entered. The
 *     common shape that trips it is a handle shared with a worker thread and
 *     released on teardown without waiting for the worker. Wait for it.
 *
 *     WHAT EACH TRANSPORT ACTUALLY DOES, because the answer is no longer uniform
 *     and the difference is not something a caller should rely on:
 *
 *       - PLAIN: safe. The object counts the callers inside it, release() drops
 *         the owner's reference rather than deleting, and the LAST call to leave
 *         destroys the object. A call that had entered before release() was
 *         called therefore runs to completion against a live object. release()
 *         itself still returns immediately and waits for nothing. Two shapes
 *         remain caller errors even there — STARTING a call at or after
 *         release() (its first act is to touch storage that may already be
 *         freed), and `delete obj` in place of release() with a call in flight —
 *         and both are reported, aborting in debug builds, whenever the object
 *         still exists to notice. See plain_logos_object.cpp.
 *       - QT REMOTE / QT LOCAL / MOCK: not safe. release() ends in `delete
 *         this`, with no counting and no detector.
 *
 *     So the INTERFACE contract is the strict one above. A transport may be
 *     kinder than the contract; code written against the contract is correct on
 *     all of them, and code written against the plain transport's behaviour
 *     breaks the day it is handed a QtRO handle.
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
     * Returns immediately. The callback is invoked EXACTLY ONCE, always from a
     * later stack — never synchronously inside this call, and never on a
     * transport's IO thread.
     *
     * WHERE it runs, which is a property of the process and not of the call:
     * when the process runs a Qt event loop the callback is queued onto it and
     * therefore lands on the Qt thread, which is what every Qt host sees. A
     * transport that supports Qt-free hosts (the plain transport) delivers on
     * its own dedicated delivery thread when the process has no such loop,
     * rather than dropping the callback — which is what it used to do, making
     * "exactly once" mean "never" in a Qt-free process. Callers that need their
     * own thread affinity must hop themselves; callers in a Qt host see no
     * change.
     *
     * THE ONE EXCEPTION, stated rather than glossed: a callback that becomes
     * ready in a Qt process AFTER QCoreApplication has been destroyed is
     * dropped. There is nowhere left to deliver it that is worth the cost —
     * running user code on a side thread while the objects it closes over are
     * being torn down is a worse outcome than silence — so "exactly once" holds
     * for the whole life of a Qt-free process, and up to application teardown
     * in a Qt one.
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
     *
     * "Must not be used again" is about STARTING something new, and it is
     * absolute: no call, no event subscription, no second release(), on any
     * thread, ever.
     *
     * Calls that are ALREADY RUNNING when release() is entered are a separate
     * question, and the answer is per-transport — see the thread-safety note on
     * this class. Write callers to the strict rule (order release() after every
     * call has returned); the plain transport happens to survive the race and
     * the Qt ones do not.
     *
     * release() does not wait. On every transport it returns without blocking on
     * in-flight work; on the plain transport that means the underlying object can
     * outlive the release() call by as long as the slowest call still inside it
     * takes to finish, which is bounded by that call's own timeout.
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
     * Same delivery contract as LogosObject::callMethodAsync — including where
     * it runs and including the post-QCoreApplication-teardown exception
     * documented there: the callback fires from a later stack, never
     * synchronously, never on a transport IO thread, and exactly once, in a
     * process with no Qt event loop as much as in one that has it. On success
     * the error argument is a default-constructed (ok()) CallError.
     */
    virtual void callMethodAsyncWithError(const QString& authToken,
                                          const QString& methodName,
                                          const QVariantList& args,
                                          int timeoutMs,
                                          AsyncResultErrorCallback callback) = 0;
};

#endif // LOGOS_OBJECT_H
