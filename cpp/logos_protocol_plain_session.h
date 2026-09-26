#ifndef LOGOS_PROTOCOL_PLAIN_SESSION_H
#define LOGOS_PROTOCOL_PLAIN_SESSION_H

// tls_tcp: a session between two runtimes. Mutual TLS 1.3 with trust anchors
// the embedder supplies (no files, no resumption, no early data), then one
// Hello from the client before anything else. The server's authenticator
// turns the Hello and what TLS proved into the caller document bound to the
// connection; no per-call token ever crosses a session.

#include "logos_protocol_plain_network.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace logos::plain::abi {

// The session wire version carried by Hello and HelloAck. A peer with another
// major is refused; a later minor is accepted.
constexpr int kSessionWireVersion = 1;

// Largest Hello or HelloAck accepted before a session exists.
constexpr std::uint32_t kPreAuthFrameLimit = 8 * 1024;

struct SessionOptions {
    std::uint32_t maxFrame = kMaxFrameLength;
    std::chrono::milliseconds handshakeTimeout{10000};
    std::chrono::milliseconds helloTimeout{10000};
    std::chrono::milliseconds keepaliveInterval{15000};
    std::chrono::milliseconds keepaliveTimeout{45000};
    std::chrono::milliseconds maxLifetime{24 * 60 * 60 * 1000};
    std::size_t maxSessions = 256;
    std::size_t maxPending = 64;
    std::size_t maxPendingPerAddress = 8;
    std::size_t maxConcurrentAuth = 4;
    std::uint16_t portMin = 0;
    std::uint16_t portMax = 0;
};

// The keys lp_provider_set_session_options takes; an unknown key is an error.
bool parseSessionOptions(const std::string& text, SessionOptions& out, std::string& error);

struct TlsCredential {
    std::string certChainPem; // the leaf first, then any intermediates
    std::string keyPem;
    bool empty() const { return certChainPem.empty() || keyPem.empty(); }
};

// Whether a PEM chain and key make a usable session credential.
bool validateCredential(const TlsCredential& credential, std::string& error);

// Whether a caller document may be bound to a session: a remote module or a
// remote operator, never the host.
bool isSessionCaller(const nlohmann::json& caller);

// Does `filter` select a session with this caller document and metadata?
// Every key must equal the same key in `meta` or in `caller`, except
// "generation_below", which compares with meta.generation.
bool sessionMatches(const nlohmann::json& filter, const nlohmann::json& caller,
                    const nlohmann::json& meta);

class SessionEndpoint final : public IncomingCallHandler {
public:
    // Takes the authentication request document and returns the reply document:
    // {"caller":{...},"lifetime_ms":n,"session":{...}} or {"error":"..."}.
    using Authenticator = std::function<std::string(const std::string& requestJson)>;
    using SessionCall = std::function<ResultMessage(const CallMessage&, const std::string& callerJson,
                                                    std::shared_ptr<GatePlace>)>;
    using MethodsHandler = std::function<MethodsResultMessage(const MethodsMessage&)>;

    SessionEndpoint(LogosTransportConfig config, SessionOptions options,
                    TlsCredential credential, std::function<std::string()> anchorsPem,
                    Authenticator authenticate, SessionCall call, MethodsHandler methods,
                    std::function<std::size_t()> capacity, GatePlaceSource reserve = {});
    ~SessionEndpoint() override;

    bool start(std::string& error);
    void stop();
    void emit(const std::string& object, const std::string& event,
              const std::vector<RpcValue>& data);
    LogosTransportConfig bound() const;
    int closeSessions(const nlohmann::json& filter, const std::string& reason);
    int extendSessions(const nlohmann::json& filter, std::chrono::milliseconds lifetime);
    // Used by sessions accepted from now on.
    bool replaceCredential(TlsCredential credential, std::string& error);
    // While on, a client chain the anchors refuse still reaches the authenticator
    // when it is self-consistent (a leaf and the self-signed root that issued it),
    // with "anchored": false. Applies to handshakes that start afterwards.
    void setUnanchoredAdmission(bool enabled);

    void onCall(const CallMessage& request, CallReply reply) override;
    void onCall(const CallMessage& request, CallReply reply, const void* connectionId) override;
    void onMethods(const MethodsMessage& request, MethodsReply reply) override;
    void onMethods(const MethodsMessage& request, MethodsReply reply,
                   const void* connectionId) override;
    void onSubscribe(const SubscribeMessage& request, EventSink sink,
                     const void* connectionId) override;
    void onUnsubscribe(const UnsubscribeMessage& request, const void* connectionId) override;
    void onConnectionClosed(const void* connectionId) override;
    void onToken(const TokenMessage& request) override;
    void onToken(const TokenMessage& request, const void* connectionId) override;

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

struct ClientSessionConfig {
    std::string target;
    LogosWireCodec codec = LogosWireCodec::Json;
    TlsCredential credential;
    // {"target","timeout_ms"} -> {"addresses":[...],"port":n,"server_pin":"sha256:...",
    //                             "anchors":"<PEM>"} or {"error":"..."}; or, with
    // "unanchored":true and no pin or anchors, any self-consistent chain passes and
    // the hello hook (told "anchored":false) decides.
    std::function<std::string(const std::string&)> dial;
    // {"target","peer_chain":[...],"exporter"} -> the Hello object, or {"error":"..."}
    std::function<std::string(const std::string&)> hello;
    SessionOptions options;
};

std::shared_ptr<RpcConnectionBase> connectSession(const ClientSessionConfig& config,
                                                  std::chrono::milliseconds timeout,
                                                  std::string& error,
                                                  const std::atomic<bool>* alive = nullptr);

} // namespace logos::plain::abi

#endif
