#include "logos_protocol_plain_network.h"

#include "implementations/plain/cbor_codec.h"
#include "implementations/plain/io_context_pool.h"
#include "implementations/plain/json_codec.h"
#include "logos_reserved_events.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>

#include <openssl/ssl.h>

#include <set>
#include <future>
#include <stdexcept>
#include <utility>

namespace logos::plain::abi {
namespace {

std::shared_ptr<IWireCodec> codecFor(LogosWireCodec codec)
{
    if (codec == LogosWireCodec::Cbor) return std::make_shared<CborCodec>();
    return std::make_shared<JsonCodec>();
}

boost::asio::ssl::context tlsContext(const LogosTransportConfig& config, bool server)
{
    boost::asio::ssl::context context(server
        ? boost::asio::ssl::context::tls_server
        : boost::asio::ssl::context::tls_client);
    context.set_options(boost::asio::ssl::context::default_workarounds
                        | boost::asio::ssl::context::no_sslv2
                        | boost::asio::ssl::context::no_sslv3);
    if (!SSL_CTX_set_min_proto_version(context.native_handle(), TLS1_2_VERSION))
        throw std::runtime_error("TLS 1.2 minimum setup failed");
    if (!config.certFile.empty()) context.use_certificate_chain_file(config.certFile);
    if (!config.keyFile.empty())
        context.use_private_key_file(config.keyFile, boost::asio::ssl::context::pem);
    if (server && SSL_CTX_check_private_key(context.native_handle()) != 1)
        throw std::runtime_error("TLS certificate and private key do not match");
    if (!config.caFile.empty()) context.load_verify_file(config.caFile);
    if (!server) context.set_verify_mode(config.verifyPeer
        ? boost::asio::ssl::verify_peer : boost::asio::ssl::verify_none);
    return context;
}

} // namespace

std::shared_ptr<RpcConnectionBase> connect(const LogosTransportConfig& config,
                                           std::string& error)
{
    try {
        auto& io = IoContextPool::shared().ioContext();
        boost::asio::ip::tcp::resolver resolver(io);
        const auto addresses = resolver.resolve(config.host, std::to_string(config.port));
        if (config.protocol == LogosProtocol::Tcp) {
            TcpStream socket(io);
            boost::asio::connect(socket, addresses);
            auto result = std::make_shared<TcpConnection>(
                std::move(socket), codecFor(config.codec));
            result->start();
            return result;
        }
        if (config.protocol == LogosProtocol::TcpSsl) {
            auto context = tlsContext(config, false);
            SslStream stream(io, context);
            if (!SSL_set_tlsext_host_name(stream.native_handle(), config.host.c_str()))
                throw std::runtime_error("TLS SNI setup failed");
            if (config.verifyPeer)
                stream.set_verify_callback(
                    boost::asio::ssl::host_name_verification(config.host));
            boost::asio::connect(stream.lowest_layer(), addresses);
            stream.handshake(boost::asio::ssl::stream_base::client);
            auto result = std::make_shared<SslConnection>(
                std::move(stream), codecFor(config.codec));
            result->start();
            return result;
        }
        error = "unsupported network transport";
    } catch (const std::exception& ex) {
        error = ex.what();
    }
    return {};
}

ServerEndpoint::ServerEndpoint(LogosTransportConfig config, CallHandler call,
                               MethodsHandler methods, TokenHandler token)
    : config_(std::move(config)), call_(std::move(call)),
      methods_(std::move(methods)), token_(std::move(token)),
      worker_([this] {
          for (;;) {
              std::pair<CallMessage, CallReply> item;
              {
                  std::unique_lock<std::mutex> lock(callsMutex_);
                  callsChanged_.wait(lock, [this] {
                      return callsStopped_ || !calls_.empty();
                  });
                  if (calls_.empty() && callsStopped_) return;
                  item = std::move(calls_.front());
                  calls_.pop_front();
              }
              try {
                  item.second(call_(item.first));
              } catch (const std::exception& ex) {
                  ResultMessage failure;
                  failure.id = item.first.id;
                  failure.err = ex.what();
                  failure.errCode = "METHOD_FAILED";
                  item.second(std::move(failure));
              }
          }
      }) {}

ServerEndpoint::~ServerEndpoint() { stop(); }

bool ServerEndpoint::start()
{
    auto& io = IoContextPool::shared().ioContext();
    if (config_.protocol == LogosProtocol::Tcp) {
        tcp_ = std::make_shared<RpcServerTcp>(io, config_.host, config_.port,
                                              codecFor(config_.codec), this);
        if (tcp_->start()) return true;
        tcp_.reset();
        return false;
    }
    if (config_.protocol == LogosProtocol::TcpSsl) {
        try {
            tls_ = std::make_shared<RpcServerSsl>(io, config_.host, config_.port,
                tlsContext(config_, true), codecFor(config_.codec), this);
            if (tls_->start()) return true;
        } catch (...) {}
        tls_.reset();
    }
    return false;
}

void ServerEndpoint::stop()
{
    auto tcp = std::move(tcp_);
    auto tls = std::move(tls_);
    if (tcp) tcp->stop();
    if (tls) tls->stop();
    // The RPC layer retains a raw handler pointer. Drain its I/O thread before
    // releasing the handler or the provider callbacks captured by this endpoint.
    auto& io = IoContextPool::shared().ioContext();
    if (!io.get_executor().running_in_this_thread()) {
        auto done = std::make_shared<std::promise<void>>();
        auto waited = done->get_future();
        boost::asio::post(io, [done] { done->set_value(); });
        waited.wait();
    }
    {
        std::lock_guard<std::mutex> lock(callsMutex_);
        callsStopped_ = true;
    }
    callsChanged_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_.clear();
}

void ServerEndpoint::emit(const std::string& object, const std::string& event,
                          const std::vector<RpcValue>& data)
{
    std::vector<EventSink> sinks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = sinks_.find(object);
        if (found == sinks_.end()) return;
        std::set<const void*> seen;
        for (const auto& name : {event, std::string{}}) {
            auto group = found->second.find(name);
            if (group != found->second.end())
                for (const auto& [id, sink] : group->second)
                    if (seen.insert(id).second) sinks.push_back(sink);
            if (event.empty() || logos::isReservedEventName(event)) break;
        }
    }
    EventMessage message{object, event, data};
    for (const auto& sink : sinks) sink(message);
}

void ServerEndpoint::onCall(const CallMessage& request, CallReply reply)
{
    {
        std::lock_guard<std::mutex> lock(callsMutex_);
        if (callsStopped_) return;
        calls_.emplace_back(request, std::move(reply));
    }
    callsChanged_.notify_one();
}

void ServerEndpoint::onMethods(const MethodsMessage& request, MethodsReply reply)
{
    reply(methods_(request));
}

void ServerEndpoint::onSubscribe(const SubscribeMessage& request, EventSink sink,
                                 const void* connectionId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_[request.object][request.eventName][connectionId] = std::move(sink);
}

void ServerEndpoint::onUnsubscribe(const UnsubscribeMessage& request,
                                   const void* connectionId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto object = sinks_.find(request.object);
    if (object == sinks_.end()) return;
    auto event = object->second.find(request.eventName);
    if (event == object->second.end()) return;
    event->second.erase(connectionId);
    if (event->second.empty()) object->second.erase(event);
    if (object->second.empty()) sinks_.erase(object);
}

void ServerEndpoint::onConnectionClosed(const void* connectionId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto object = sinks_.begin(); object != sinks_.end();) {
        for (auto event = object->second.begin(); event != object->second.end();) {
            event->second.erase(connectionId);
            if (event->second.empty()) event = object->second.erase(event);
            else ++event;
        }
        if (object->second.empty()) object = sinks_.erase(object);
        else ++object;
    }
}

void ServerEndpoint::onToken(const TokenMessage& request)
{
    token_(request);
}

} // namespace logos::plain::abi
