#ifndef LOGOS_PLAIN_RPC_MESSAGE_H
#define LOGOS_PLAIN_RPC_MESSAGE_H

#include "rpc_value.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace logos::plain {

// -----------------------------------------------------------------------------
// Wire message definitions.
//
// Each on-wire frame is [4-byte big-endian length][1-byte type tag][payload].
// The payload encoding is decided by the codec (JSON today, CBOR planned).
//
// `MessageType` doubles as the 1-byte type tag so the codec can decode the
// right struct without peeking inside the payload. Keep the values stable —
// they're on the wire.
// -----------------------------------------------------------------------------

enum class MessageType : uint8_t {
    Call        = 1,
    Result      = 2,
    Subscribe   = 3,
    Unsubscribe = 4,
    Event       = 5,
    Token       = 6,
    Methods     = 7,
    MethodsResult = 8,
    // Session frames (tls_tcp only), never decoded by an IWireCodec: their
    // payload is a JSON object (Hello/HelloAck) or empty (Ping/Pong). They go
    // only to a peer that opened a tls_tcp session, so older peers never see them.
    Hello       = 9,
    HelloAck    = 10,
    Ping        = 11,
    Pong        = 12,
};

struct MethodMetadata {
    std::string name;
    std::string signature;
    std::string returnType;
    bool        isInvokable = true;
    RpcList     parameters; // list of {name, type} maps — schema-flexible
};

// Call <module>.<method>(args...). The response is a Result with the same id.
struct CallMessage {
    uint64_t              id;
    std::string           authToken;
    std::string           object;
    std::string           method;
    std::vector<RpcValue> args;
};

// Response to a Call (or Methods) message, matched by id.
struct ResultMessage {
    uint64_t    id;
    bool        ok = false;
    RpcValue    value;   // present when ok
    std::string err;     // present when !ok
    std::string errCode; // present when !ok
};

// Subscribe / unsubscribe to a named event on an object.
// After Subscribe, the peer pushes EventMessage frames whenever the provider
// emits that event until an Unsubscribe arrives (or the connection closes).
// eventName "" means "all events on this object" (wildcard).
struct SubscribeMessage {
    std::string object;
    std::string eventName;
};
struct UnsubscribeMessage {
    std::string object;
    std::string eventName;
};

// Fire-and-forget event delivery from provider → subscriber.
struct EventMessage {
    std::string           object;
    std::string           eventName;
    std::vector<RpcValue> data;
};

// Authorization token that the consumer wants registered for a specific
// module name. Mirrors LogosObject::informModuleToken today.
struct TokenMessage {
    std::string authToken;
    std::string moduleName;
    std::string token;
};

// Query the set of methods a published object exposes. Response is a
// MethodsResult keyed to the same id.
struct MethodsMessage {
    uint64_t    id;
    std::string authToken;
    std::string object;
};
struct MethodsResultMessage {
    uint64_t                    id;
    bool                        ok = false;
    std::vector<MethodMetadata> methods;
    std::string                 err;
};

// Tagged union of every message the wire stack knows.
using AnyMessage = std::variant<
    CallMessage,
    ResultMessage,
    SubscribeMessage,
    UnsubscribeMessage,
    EventMessage,
    TokenMessage,
    MethodsMessage,
    MethodsResultMessage
>;

// Lookup: AnyMessage variant ↔ MessageType tag.
MessageType messageTypeOf(const AnyMessage& m);

} // namespace logos::plain

#endif // LOGOS_PLAIN_RPC_MESSAGE_H
