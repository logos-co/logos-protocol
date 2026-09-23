#ifndef LOGOS_PROTOCOL_PLAIN_NETWORK_H
#define LOGOS_PROTOCOL_PLAIN_NETWORK_H

#include "logos_transport_config.h"
#include "implementations/plain/rpc_server.h"

#include <functional>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace logos::plain::abi {

std::shared_ptr<RpcConnectionBase> connect(const LogosTransportConfig& config,
                                           std::chrono::milliseconds timeout,
                                           std::string& error,
                                           const std::atomic<bool>* alive = nullptr);

class ServerEndpoint final : public IncomingCallHandler {
public:
    using CallHandler = std::function<ResultMessage(const CallMessage&)>;
    using MethodsHandler = std::function<MethodsResultMessage(const MethodsMessage&)>;
    using TokenHandler = std::function<void(const TokenMessage&)>;

    ServerEndpoint(LogosTransportConfig config, CallHandler call,
                   MethodsHandler methods, TokenHandler token);
    ~ServerEndpoint() override;
    bool start();
    void stop();
    void emit(const std::string& object, const std::string& event,
              const std::vector<RpcValue>& data);

    void onCall(const CallMessage& request, CallReply reply) override;
    void onMethods(const MethodsMessage& request, MethodsReply reply) override;
    void onSubscribe(const SubscribeMessage& request, EventSink sink,
                     const void* connectionId) override;
    void onUnsubscribe(const UnsubscribeMessage& request,
                       const void* connectionId) override;
    void onConnectionClosed(const void* connectionId) override;
    void onToken(const TokenMessage& request) override;

private:
    LogosTransportConfig config_;
    CallHandler call_;
    MethodsHandler methods_;
    TokenHandler token_;
    std::shared_ptr<RpcServerTcp> tcp_;
    std::shared_ptr<RpcServerSsl> tls_;
    std::mutex mutex_;
    std::map<std::string, std::map<std::string, std::map<const void*, EventSink>>> sinks_;
    std::mutex callsMutex_;
    std::condition_variable callsChanged_;
    std::deque<std::pair<CallMessage, CallReply>> calls_;
    bool callsStopped_ = false;
    std::thread worker_;
};

} // namespace logos::plain::abi

#endif
