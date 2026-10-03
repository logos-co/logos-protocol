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

#include <algorithm>
#include <set>
#include <future>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <type_traits>
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
                        | boost::asio::ssl::context::no_sslv3
                        | boost::asio::ssl::context::single_dh_use);
    // The Qt-side host's policy (plain_transport_host.cpp): TLS 1.2 or 1.3 with
    // forward-secret suites only. Refused rather than weakened if it won't apply.
    SSL_CTX* native = context.native_handle();
    if (!SSL_CTX_set_min_proto_version(native, TLS1_2_VERSION)
        || !SSL_CTX_set_max_proto_version(native, TLS1_3_VERSION)
        || !SSL_CTX_set1_groups_list(native, "X25519:P-256:P-384:P-521")
        || !SSL_CTX_set_ciphersuites(native, "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:"
                                             "TLS_CHACHA20_POLY1305_SHA256")
        || !SSL_CTX_set_cipher_list(native, "ECDHE+AESGCM:ECDHE+CHACHA20:DHE+AESGCM:"
                                            "DHE+CHACHA20:!aNULL:!MD5:!DSS:!RC4:!3DES"))
        throw std::runtime_error("TLS policy setup failed");
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

struct DialResult {
    std::shared_ptr<RpcConnectionBase> connection;
    std::string error;
};

template <typename Stream>
class DialAttempt : public std::enable_shared_from_this<DialAttempt<Stream>> {
public:
    DialAttempt(boost::asio::io_context& io, const LogosTransportConfig& config,
                std::shared_ptr<boost::asio::ssl::context> context)
        : io_(io), resolver_(io), context_(std::move(context)),
          stream_(makeStream(io, context_)), codec_(codecFor(config.codec)),
          host_(config.host), port_(std::to_string(config.port))
    {
        if constexpr (std::is_same_v<Stream, SslStream>) {
            if (!SSL_set_tlsext_host_name(stream_.native_handle(), host_.c_str()))
                throw std::runtime_error("TLS SNI setup failed");
            if (config.verifyPeer)
                stream_.set_verify_callback(
                    boost::asio::ssl::host_name_verification(host_));
        }
    }

    std::future<DialResult> future() { return result_.get_future(); }

    void start()
    {
        auto self = this->shared_from_this();
        resolver_.async_resolve(host_, port_,
            [self](const boost::system::error_code& ec,
                   boost::asio::ip::tcp::resolver::results_type addresses) {
                if (ec) { self->finishError(ec.message()); return; }
                boost::asio::async_connect(self->stream_.lowest_layer(), addresses,
                    [self](const boost::system::error_code& connectError,
                           const boost::asio::ip::tcp::endpoint&) {
                        if (connectError) {
                            self->finishError(connectError.message());
                            return;
                        }
                        self->connected();
                    });
            });
    }

    void abandon()
    {
        std::shared_ptr<RpcConnectionBase> connection;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            abandoned_ = true;
            connection = connection_;
            if (!delivered_) {
                delivered_ = true;
                result_.set_value({{}, "connection timed out"});
            }
        }
        if (connection) {
            connection->stop("connection timed out");
            return;
        }
        auto self = this->shared_from_this();
        boost::asio::post(io_, [self] {
            self->resolver_.cancel();
            boost::system::error_code ignored;
            self->stream_.lowest_layer().cancel(ignored);
            self->stream_.lowest_layer().close(ignored);
        });
    }

private:
    static Stream makeStream(boost::asio::io_context& io,
                             const std::shared_ptr<boost::asio::ssl::context>& context)
    {
        if constexpr (std::is_same_v<Stream, SslStream>) return Stream(io, *context);
        else return Stream(io);
    }

    void connected()
    {
        if constexpr (std::is_same_v<Stream, SslStream>) {
            auto self = this->shared_from_this();
            stream_.async_handshake(boost::asio::ssl::stream_base::client,
                [self](const boost::system::error_code& ec) {
                    if (ec) self->finishError(ec.message());
                    else self->finishSuccess();
                });
        } else finishSuccess();
    }

    void finishError(std::string error)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (delivered_) return;
        delivered_ = true;
        result_.set_value({{}, std::move(error)});
    }

    void finishSuccess()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (delivered_ || abandoned_) return;
        if constexpr (std::is_same_v<Stream, SslStream>)
            connection_ = std::make_shared<SslConnection>(std::move(stream_), codec_);
        else
            connection_ = std::make_shared<TcpConnection>(std::move(stream_), codec_);
        connection_->start();
        delivered_ = true;
        result_.set_value({connection_, {}});
    }

    boost::asio::io_context& io_;
    boost::asio::ip::tcp::resolver resolver_;
    std::shared_ptr<boost::asio::ssl::context> context_;
    Stream stream_;
    std::shared_ptr<IWireCodec> codec_;
    std::string host_;
    std::string port_;
    std::promise<DialResult> result_;
    std::mutex mutex_;
    bool abandoned_ = false;
    bool delivered_ = false;
    std::shared_ptr<RpcConnectionBase> connection_;
};

template <typename Stream>
std::shared_ptr<RpcConnectionBase> dial(boost::asio::io_context& io,
                                       const LogosTransportConfig& config,
                                       std::chrono::milliseconds timeout,
                                       std::string& error,
                                       const std::atomic<bool>* alive,
                                       std::shared_ptr<boost::asio::ssl::context> context = {})
{
    if (timeout <= std::chrono::milliseconds::zero()) {
        error = "connection timed out";
        return {};
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto attempt = std::make_shared<DialAttempt<Stream>>(io, config, std::move(context));
    auto future = attempt->future();
    attempt->start();
    for (;;) {
        if (alive && !alive->load()) {
            attempt->abandon();
            error = "client destroyed";
            return {};
        }
        const auto nextCheck = std::min(deadline,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(10));
        if (future.wait_until(nextCheck) == std::future_status::ready) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            attempt->abandon();
            error = "connection timed out";
            return {};
        }
    }
    auto result = future.get();
    if (alive && !alive->load()) {
        attempt->abandon();
        error = "client destroyed";
        return {};
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        attempt->abandon();
        error = "connection timed out";
        return {};
    }
    error = std::move(result.error);
    return std::move(result.connection);
}

} // namespace

std::shared_ptr<RpcConnectionBase> connect(const LogosTransportConfig& config,
                                           std::chrono::milliseconds timeout,
                                           std::string& error,
                                           const std::atomic<bool>* alive)
{
    try {
        auto& io = IoContextPool::shared().ioContext();
        if (config.protocol == LogosProtocol::Tcp)
            return dial<TcpStream>(io, config, timeout, error, alive);
        if (config.protocol == LogosProtocol::TcpSsl) {
            auto context = std::make_shared<boost::asio::ssl::context>(
                tlsContext(config, false));
            return dial<SslStream>(io, config, timeout, error, alive,
                                   std::move(context));
        }
        error = "unsupported network transport";
    } catch (const std::exception& ex) {
        error = ex.what();
    }
    return {};
}

bool isControlMethod(const std::string& method)
{
    return method == "informModuleToken" || method == "revokeModuleToken"
        || method == "getPluginMethods" || method == "getPluginEvents"
        || method == "getPluginInterface";
}

namespace {
constexpr std::chrono::seconds kBusinessIdle{5};
}

EndpointWorkers::EndpointWorkers(std::function<std::size_t()> capacity)
    : capacity_(capacity ? std::move(capacity) : [] { return std::size_t{1}; })
    , controlThread_([this] { controlLoop(); })
{
}

EndpointWorkers::~EndpointWorkers() { stop(); }

void EndpointWorkers::control(std::function<void()> job)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) return;
        controlJobs_.push_back(std::move(job));
    }
    changed_.notify_all();
}

void EndpointWorkers::business(std::function<void()> job)
{
    const std::size_t capacity = std::max<std::size_t>(1, capacity_());
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return;
    businessJobs_.push_back(std::move(job));
    if (businessIdle_ < businessJobs_.size() && businessWorkers_ < capacity) {
        // Detached: stop() waits for the count to reach zero instead of joining,
        // so threads that exited while idle leave nothing behind.
        std::thread([this] { businessLoop(); }).detach();
        ++businessWorkers_;
    }
    changed_.notify_all();
}

void EndpointWorkers::controlLoop()
{
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        changed_.wait(lock, [this] { return stopped_ || !controlJobs_.empty(); });
        if (controlJobs_.empty()) return; // stopped and drained
        auto job = std::move(controlJobs_.front());
        controlJobs_.pop_front();
        lock.unlock();
        try { job(); } catch (...) {}
        job = nullptr;
        lock.lock();
    }
}

void EndpointWorkers::businessLoop()
{
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        ++businessIdle_;
        const bool woke = changed_.wait_for(lock, kBusinessIdle,
            [this] { return stopped_ || !businessJobs_.empty(); });
        --businessIdle_;
        if (businessJobs_.empty()) {
            // Idle too long, or stopped with nothing left: this thread is done.
            if (stopped_ || !woke) break;
            continue;
        }
        auto job = std::move(businessJobs_.front());
        businessJobs_.pop_front();
        lock.unlock();
        try { job(); } catch (...) {}
        job = nullptr;
        lock.lock();
    }
    --businessWorkers_;
    workersDone_.notify_all();
}

void EndpointWorkers::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
    }
    changed_.notify_all();
    if (controlThread_.joinable()) controlThread_.join();
    std::unique_lock<std::mutex> lock(mutex_);
    workersDone_.wait(lock, [this] { return businessWorkers_ == 0; });
}

void SinkTable::subscribe(const std::string& object, const std::string& event,
                          const void* connection, IncomingCallHandler::EventSink sink)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_[object][event][connection] = std::move(sink);
}

void SinkTable::unsubscribe(const std::string& object, const std::string& event,
                            const void* connection)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = sinks_.find(object);
    if (found == sinks_.end()) return;
    auto named = found->second.find(event);
    if (named == found->second.end()) return;
    named->second.erase(connection);
    if (named->second.empty()) found->second.erase(named);
    if (found->second.empty()) sinks_.erase(found);
}

void SinkTable::dropConnection(const void* connection)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto object = sinks_.begin(); object != sinks_.end();) {
        for (auto event = object->second.begin(); event != object->second.end();) {
            event->second.erase(connection);
            if (event->second.empty()) event = object->second.erase(event);
            else ++event;
        }
        if (object->second.empty()) object = sinks_.erase(object);
        else ++object;
    }
}

void SinkTable::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_.clear();
}

void SinkTable::emit(const std::string& object, const std::string& event,
                     const std::vector<RpcValue>& data)
{
    std::vector<IncomingCallHandler::EventSink> sinks;
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

ServerEndpoint::ServerEndpoint(LogosTransportConfig config, CallHandler call,
                               MethodsHandler methods, TokenHandler token,
                               std::function<std::size_t()> capacity,
                               GatePlaceSource reserve)
    : config_(std::move(config)), call_(std::move(call)),
      methods_(std::move(methods)), token_(std::move(token)),
      reserve_(std::move(reserve)), workers_(std::move(capacity)) {}

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

LogosTransportConfig ServerEndpoint::bound() const
{
    LogosTransportConfig out = config_;
    if (tcp_) out.port = tcp_->boundPort();
    if (tls_) out.port = tls_->boundPort();
    return out;
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
    workers_.stop();
    sinks_.clear();
}

void ServerEndpoint::emit(const std::string& object, const std::string& event,
                          const std::vector<RpcValue>& data)
{
    sinks_.emit(object, event, data);
}

void ServerEndpoint::onCall(const CallMessage& request, CallReply reply)
{
    const bool control = isControlMethod(request.method);
    // Taken here, on the I/O thread, so business calls keep their arrival order.
    std::shared_ptr<GatePlace> place = !control && reserve_ ? reserve_() : nullptr;
    auto job = [this, request, reply = std::move(reply), place = std::move(place)]() mutable {
        try {
            reply(call_(request, std::move(place)));
        } catch (const std::exception& ex) {
            ResultMessage failure;
            failure.id = request.id;
            failure.err = ex.what();
            failure.errCode = "METHOD_FAILED";
            reply(std::move(failure));
        }
    };
    if (control) workers_.control(std::move(job));
    else workers_.business(std::move(job));
}

// On the control lane: a module's getMethods or token handler may be slow, and
// the I/O thread serves every endpoint in the process.
void ServerEndpoint::onMethods(const MethodsMessage& request, MethodsReply reply)
{
    workers_.control([this, request, reply = std::move(reply)] {
        try {
            reply(methods_(request));
        } catch (const std::exception& ex) {
            MethodsResultMessage failure;
            failure.id = request.id;
            failure.err = ex.what();
            reply(std::move(failure));
        }
    });
}

void ServerEndpoint::onSubscribe(const SubscribeMessage& request, EventSink sink,
                                 const void* connectionId)
{
    sinks_.subscribe(request.object, request.eventName, connectionId, std::move(sink));
}

void ServerEndpoint::onUnsubscribe(const UnsubscribeMessage& request,
                                   const void* connectionId)
{
    sinks_.unsubscribe(request.object, request.eventName, connectionId);
}

void ServerEndpoint::onConnectionClosed(const void* connectionId)
{
    sinks_.dropConnection(connectionId);
}

void ServerEndpoint::onToken(const TokenMessage& request)
{
    workers_.control([this, request] { token_(request); });
}

} // namespace logos::plain::abi
