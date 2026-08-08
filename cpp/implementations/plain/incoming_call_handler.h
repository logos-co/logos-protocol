#ifndef LOGOS_PLAIN_INCOMING_CALL_HANDLER_H
#define LOGOS_PLAIN_INCOMING_CALL_HANDLER_H

#include "rpc_message.h"

#include <functional>

namespace logos::plain {

// -----------------------------------------------------------------------------
// IncomingCallHandler — provider-side dispatch hook.
//
// rpc_connection hands inbound Call / Methods / Subscribe / Unsubscribe /
// Token messages to a handler that the Qt-boundary layer implements. The
// handler is what talks to the published QObject (ModuleProxy); this
// interface deliberately speaks only plain C++ types so the wire stack
// stays Qt-free.
//
// The reply callbacks can be invoked synchronously (from inside the
// handler) or asynchronously from a different thread — rpc_connection
// serializes the actual frame write internally.
// -----------------------------------------------------------------------------
class IncomingCallHandler {
public:
    virtual ~IncomingCallHandler() = default;

    using CallReply     = std::function<void(ResultMessage)>;
    using MethodsReply  = std::function<void(MethodsResultMessage)>;
    using EventSink     = std::function<void(EventMessage)>;

    virtual void onCall(const CallMessage& req, CallReply reply) = 0;

    virtual void onMethods(const MethodsMessage& req, MethodsReply reply) = 0;

    // `sink` stays alive until onUnsubscribe fires or the connection
    // dies. The handler must call `sink(evt)` on every matching emission.
    //
    // `connectionId` is an opaque per-connection token (the rpc layer
    // passes the connection's `this` pointer). The handler keys sinks
    // by it so a subsequent onUnsubscribe / onConnectionClosed can
    // remove only the sinks belonging to that connection — sub/unsub
    // frames don't carry a subscriber identifier on the wire.
    //
    // ONE SINK PER (object, event, connection), AND THAT IS THE CONTRACT, not a
    // simplification waiting to be lifted. Every sink a handler could build for a
    // given connection is the same thing — "write this frame back down that
    // socket" — so a second Subscribe for a pair this connection already has is
    // an idempotent re-assertion, and a handler is right to overwrite. It follows
    // that a CONSUMER with several logical subscribers behind one connection owns
    // the demultiplexing: it fans one delivery out locally (see
    // RpcConnection::sendSubscribe) and sends Unsubscribe only when the last of
    // them is gone, because Unsubscribe means "this connection wants no more of
    // that event at all". A consumer that unsubscribes per-subscriber silences
    // its own siblings, and no host-side bookkeeping can tell that apart from a
    // genuine unsubscribe.
    virtual void onSubscribe(const SubscribeMessage& req, EventSink sink,
                             const void* connectionId) = 0;

    virtual void onUnsubscribe(const UnsubscribeMessage& req,
                               const void* connectionId) = 0;

    // Called when a connection is torn down (graceful close or error)
    // so the handler can drop any sinks still keyed to it. Without
    // this, a dropped client leaks subscriptions and the host keeps
    // fanning events into dead sinks.
    virtual void onConnectionClosed(const void* connectionId) = 0;

    virtual void onToken(const TokenMessage& req) = 0;
};

} // namespace logos::plain

#endif // LOGOS_PLAIN_INCOMING_CALL_HANDLER_H
