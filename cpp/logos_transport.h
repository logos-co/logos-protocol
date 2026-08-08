#ifndef LOGOS_TRANSPORT_H
#define LOGOS_TRANSPORT_H

#include <QString>
#include <QVariant>
#include <QVariantList>

#include <functional>

class QObject;
class LogosObject;

/**
 * @brief Abstract interface for the provider/server side of module transport.
 *
 * Implementations handle how a module object is made available to consumers
 * (e.g. via in-process registry, Qt Remote Objects, or other mechanisms).
 */
class LogosTransportHost {
public:
    virtual ~LogosTransportHost() = default;

    /**
     * @brief Publish an object so consumers can discover and invoke it
     * @param name The name to publish the object under
     * @param object The QObject to publish (provider side remains Qt-based)
     * @return true if publishing succeeded
     */
    virtual bool publishObject(const QString& name, QObject* object) = 0;

    /**
     * @brief Remove a previously published object
     * @param name The name the object was published under
     */
    virtual void unpublishObject(const QString& name) = 0;

    /**
     * @brief URL this backend will (or does) listen on for the named object.
     *
     * Used by LogosAPIProvider so it doesn't need to bake the URL scheme
     * into LogosInstance — each backend owns its own addressing.
     *
     * Default implementation returns the deterministic local-socket URL
     * ("local:logos_<mod>_<instance>") for back-compat with existing
     * qt_remote code; network-transport backends override.
     */
    virtual QString bindUrl(const QString& instanceId,
                            const QString& moduleName);
};

/**
 * @brief Abstract interface for the consumer/client side of module transport.
 *
 * Implementations handle how a consumer connects to a host and acquires
 * LogosObject handles.  Method invocation, event subscription, and lifecycle
 * management are handled by LogosObject itself.
 */
class LogosTransportConnection {
public:
    virtual ~LogosTransportConnection() = default;

    /**
     * @brief Establish connection to the host/registry
     * @return true if connection succeeded
     */
    virtual bool connectToHost() = 0;

    /**
     * @brief Check if currently connected
     */
    virtual bool isConnected() const = 0;

    /**
     * @brief Tear down and re-establish the connection
     * @return true if reconnection succeeded
     */
    virtual bool reconnect() = 0;

    /**
     * @brief Acquire a LogosObject handle to a named object from the host.
     *
     * The returned LogosObject encapsulates method invocation, event
     * handling and lifecycle.  Call LogosObject::release() when done.
     *
     * @param objectName The published name of the object
     * @param timeoutMs Maximum time to wait for the object to become available
     * @return LogosObject* handle, or nullptr on failure.
     */
    virtual LogosObject* requestObject(const QString& objectName, int timeoutMs) = 0;

    /**
     * @brief URL this backend expects to connect to for the named object.
     *
     * Mirror of LogosTransportHost::bindUrl on the consumer side. Default
     * returns the deterministic local-socket URL; network-transport
     * backends override.
     */
    virtual QString endpointUrl(const QString& instanceId,
                                const QString& moduleName);
};

/**
 * @brief Optional extension: acquire a handle WITHOUT blocking, delivered when
 *        (and if) the object becomes reachable.
 *
 * requestObject() answers "is it there RIGHT NOW", and on the qt_remote
 * transport it answers by sitting in QRemoteObjectReplica::waitForSource() —
 * a nested event loop, for up to `timeoutMs`. Neither answer is usable from a
 * GUI thread during startup, when the module's host process has been spawned
 * but has not called listen() yet. A caller that needs a *subscription* rather
 * than an immediate call has no reason to ask the now-question at all: it can
 * wait, as long as waiting costs nothing and is not silent.
 *
 * This is DELIBERATELY a sibling interface rather than another virtual on
 * LogosTransportConnection, for exactly the reason spelled out on
 * LogosObjectErrorChannel (logos_object.h): logos_transport.h is an installed
 * header whose vtable is baked into every statically-linked copy of
 * liblogos_protocol in a process, one per loaded module, each pinned to its own
 * protocol revision. Appending a virtual would append a vtable slot, and a
 * caller compiled against the new header calling that slot on a transport whose
 * vtable came from an older copy is undefined behaviour. A separate interface
 * reached with dynamic_cast leaves LogosTransportConnection's layout, size and
 * vtable byte-for-byte unchanged — an older copy simply fails the cast.
 *
 * Consumers MUST therefore treat it as optional:
 *
 *     if (auto* async = dynamic_cast<LogosTransportAsyncAcquire*>(conn)) {
 *         if (async->requestObjectWhenAvailable(name, cb)) return;  // armed
 *     }
 *     // ... else retry requestObject() yourself, and say that you are.
 *
 * Implemented by qt_remote, where the wait is free: QRemoteObjectNode already
 * retries a dropped/absent connection every 250 ms for the life of the process
 * (QRemoteObjectNodePrivate::onShouldReconnect), so a pending acquire adds one
 * replica object and no timer at all. The other transports do not implement it
 * — their requestObject() is a registry hash lookup or an in-memory socket
 * check, cheap enough for the caller to poll.
 */
class LogosTransportAsyncAcquire {
public:
    virtual ~LogosTransportAsyncAcquire() = default;

    /**
     * @brief Callback for a deferred acquire.
     *
     * Invoked with a LogosObject* the caller owns (release() when done), or
     * with nullptr when the object can be proven to be permanently
     * unreachable on this connection (e.g. a source signature mismatch).
     * nullptr means GIVE UP — it is never used for "not there yet".
     */
    using AcquireCallback = std::function<void(LogosObject*)>;

    /**
     * @brief Register interest in `objectName` and return immediately.
     *
     * Never blocks and never spins a nested event loop.
     *
     * @return true  the request was accepted; `onReady` will be invoked AT MOST
     *               once, on a later event-loop turn, never synchronously from
     *               inside this call and never from inside the transport's own
     *               read stack.
     * @return false the transport declined (it cannot defer, or is not in a
     *               state to try). `onReady` is NOT invoked, now or ever, and
     *               the caller owns the retry.
     *
     * AT MOST once, not exactly once. An accepted request is CANCELLED —
     * silently, with no callback — if the connection is torn down or rebuilt
     * (reconnect) or the transport is destroyed, because the in-flight acquires
     * belong to the connection that owns them. A caller that must not be left
     * waiting forever therefore cannot treat acceptance as a guaranteed answer:
     * it has to re-issue after a reconnect, or carry its own deadline. Both are
     * what LogosAPIConsumer's pending-subscription registry does — reconnected()
     * re-issues every entry it still holds, which is why a subscription made
     * through onEventWhenAvailable() survives something the raw transport call
     * does not.
     */
    virtual bool requestObjectWhenAvailable(const QString& objectName,
                                            AcquireCallback onReady) = 0;
};

#endif // LOGOS_TRANSPORT_H
