#ifndef LOGOS_PLAIN_LOCAL_INPROC_TRANSPORT_H
#define LOGOS_PLAIN_LOCAL_INPROC_TRANSPORT_H

// plain_local: the Qt-free in-process transport (protocol "inproc").
//
// A provider registers an endpoint under (instance, module); a client in the
// same image connects to it without a socket. Calls, events, token delivery and
// metadata keep the socket transports' semantics: calls run on the endpoint's
// workers in arrival order and results reach the caller through its own
// delivery path, never inline on the thread that asked.
//
// Every connection is bound to the principal that opened it, and the provider
// half receives that principal with each call and token.

#include "implementations/plain/rpc_connection.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace logos::plain::inproc {

struct Handlers {
    // A business call from a connection bound to `principal`.
    std::function<ResultMessage(const CallMessage&, const std::string& principal)> call;
    // informModuleToken, revokeModuleToken and metadata: never queued behind calls.
    std::function<ResultMessage(const CallMessage&, const std::string& principal)> control;
    std::function<MethodsResultMessage(const MethodsMessage&)> methods;
    std::function<bool(const TokenMessage&, const std::string& principal)> token;
};

class Endpoint;

// Methods the control lane answers rather than the call workers.
bool isControlMethod(const std::string& method);

// Registers `handlers` as `module` in `instance`. Null when that name is taken.
std::shared_ptr<Endpoint> publish(const std::string& instance, const std::string& module,
                                  Handlers handlers, std::size_t maxWorkers);

// Unregisters, fails every connection's pending calls and waits for running
// calls to return. Idempotent.
void withdraw(const std::shared_ptr<Endpoint>& endpoint);

void setMaxWorkers(const std::shared_ptr<Endpoint>& endpoint, std::size_t maxWorkers);

bool isPublished(const std::string& instance, const std::string& module);

// Emits to every connection subscribed to (object, event), or to all of the
// object's events for a non-reserved event.
void emit(const std::shared_ptr<Endpoint>& endpoint, const std::string& object,
          const std::string& event, const std::vector<RpcValue>& data);

// A connection to the endpoint registered as `module` in `instance`, bound to
// `principal`. Null, with `error` set, when nothing is registered.
std::shared_ptr<RpcConnectionBase> connect(const std::string& instance,
                                           const std::string& module,
                                           const std::string& principal,
                                           std::string& error);

} // namespace logos::plain::inproc

#endif
