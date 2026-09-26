#include "logos_protocol.h"
#include "logos_protocol_plain_network.h"
#include "self_signed_cert.h"

#include <gtest/gtest.h>

#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/write.hpp>

#include <nlohmann/json.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <array>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

char* copy(const char* text)
{
    auto* result = static_cast<char*>(std::malloc(std::strlen(text) + 1));
    std::strcpy(result, text);
    return result;
}

char* dispatch(const char* method, const char*, void*)
{
    return copy(std::strcmp(method, "answer") == 0 ? "42" : "null");
}

char* methods(void*)
{
    return copy(R"([{"name":"answer","type":"method","returnType":"int"}])");
}

int token(const char*, const char*, void*) { return LP_OK; }

struct EventResult {
    std::mutex mutex;
    std::condition_variable changed;
    std::string value;
};

void event(const char* name, const char* data, void* user)
{
    auto& result = *static_cast<EventResult*>(user);
    {
        std::lock_guard<std::mutex> lock(result.mutex);
        result.value = std::string(name) + ":" + data;
    }
    result.changed.notify_all();
}

struct DeferredState {
    lp_provider* provider = nullptr;
    lp_client* client = nullptr;
    std::atomic<bool> callbackStarted{false};
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    int status = LP_ERR_INTERNAL;
    std::string value;
};

struct AsyncDestroyState {
    lp_client* client = nullptr;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool proceed = false;
    std::atomic<bool> armed{false};
    std::atomic<bool> done{false};
    std::atomic<int> success{-1};
    std::atomic<int> events{0};
};

void onAsyncDestroyStatus(int status, unsigned long long, const char*, void* user)
{
    if (status == LP_SUB_ARMED)
        static_cast<AsyncDestroyState*>(user)->armed = true;
}

void onAsyncDestroyEvent(const char*, const char*, void* user)
{
    ++static_cast<AsyncDestroyState*>(user)->events;
}

void onAsyncDestroyResult(int success, const char*, void* user)
{
    auto& state = *static_cast<AsyncDestroyState*>(user);
    state.success = success;
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        state.entered = true;
        state.changed.notify_all();
        state.changed.wait(lock, [&] { return state.proceed; });
    }
    lp_client_destroy(state.client);
    state.client = nullptr;
    state.done = true;
}

char* deferredDispatch(const char* method, const char*, void* user)
{
    auto& state = *static_cast<DeferredState*>(user);
    if (std::strcmp(method, "deferred") != 0) return copy("true");
    // A completion is allowed to arrive before the pending-call reply.
    lp_provider_emit_event(state.provider, "__logos_call_complete__",
                           R"(["network-deferred-1","completed"])");
    return copy(R"({"__logos_pending_call__":"network-deferred-1"})");
}

void callDeferredFromEvent(const char*, const char*, void* user)
{
    auto& state = *static_cast<DeferredState*>(user);
    if (state.callbackStarted.exchange(true)) return;
    char* value = nullptr;
    char* error = nullptr;
    const int status = lp_invoke(state.client, "deferred", "[]", 500,
                                 &value, &error);
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.status = status;
        state.value = value ? value : "";
        state.done = true;
    }
    lp_string_free(value);
    lp_string_free(error);
    state.changed.notify_all();
}

struct InformedToken {
    std::mutex mutex;
    std::string module;
    std::string token;
};

int recordToken(const char* module, const char* value, void* userData)
{
    auto& informed = *static_cast<InformedToken*>(userData);
    std::lock_guard<std::mutex> lock(informed.mutex);
    informed.module = module;
    informed.token = value;
    return LP_OK;
}

std::string reservedTcpConfig()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor reservation(io, {boost::asio::ip::address_v4::loopback(), 0});
    const auto port = reservation.local_endpoint().port();
    reservation.close();
    return std::string(R"({"protocol":"tcp","host":"127.0.0.1","port":)") + std::to_string(port)
        + "}";
}

// Detector: over tcp the consumer-half token went down the target's connection
// instead of to capability_module.
TEST(PlainNetworkCAbi, InformModuleTokenReachesCapabilityModuleOverTcp)
{
    const std::string capabilityConfig = reservedTcpConfig();
    const std::string otherConfig = reservedTcpConfig();
    InformedToken atCapability;
    InformedToken atOther;
    lp_provider* capability =
        lp_provider_create("capability_module", ("[" + capabilityConfig + "]").c_str());
    lp_provider* other = lp_provider_create("inform_other_tcp", ("[" + otherConfig + "]").c_str());
    ASSERT_NE(capability, nullptr);
    ASSERT_NE(other, nullptr);
    for (lp_provider* provider : {capability, other})
        ASSERT_EQ(lp_provider_save_token(provider, "core", "core-secret"), LP_OK);
    ASSERT_EQ(lp_provider_register(capability, dispatch, methods, recordToken, &atCapability),
              LP_OK);
    ASSERT_EQ(lp_provider_register(other, dispatch, methods, recordToken, &atOther), LP_OK);

    lp_client* client = lp_client_create("inform_other_tcp", "core", otherConfig.c_str(),
                                         capabilityConfig.c_str());
    ASSERT_NE(client, nullptr);
    EXPECT_EQ(lp_inform_module_token(client, "core-secret", "loaded_module", "loaded-token"),
              LP_OK);
    lp_client_destroy(client);
    lp_provider_destroy(other);
    lp_provider_destroy(capability);

    EXPECT_EQ(atCapability.module, "loaded_module");
    EXPECT_EQ(atCapability.token, "loaded-token");
    EXPECT_TRUE(atOther.module.empty()) << "the token went down the target's connection";
}

TEST(PlainNetworkCAbi, TcpCallAndEventWithoutQt)
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor reservation(io,
        {boost::asio::ip::address_v4::loopback(), 0});
    const auto port = reservation.local_endpoint().port();
    reservation.close();
    const std::string config = std::string(R"({"protocol":"tcp","host":"127.0.0.1","port":)")
        + std::to_string(port) + "}";
    const std::string set = "[" + config + "]";

    lp_provider* provider = lp_provider_create("plain_network_probe", set.c_str());
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_save_token(provider, "network_test", "secret"), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, dispatch, methods, token, nullptr), LP_OK);
    ASSERT_EQ(lp_token_save("plain_network_probe", "secret"), LP_OK);

    lp_client* client = lp_client_create("plain_network_probe", "network_test",
                                         config.c_str(), config.c_str());
    ASSERT_NE(client, nullptr);
    char* result = nullptr;
    char* error = nullptr;
    EXPECT_EQ(lp_invoke(client, "answer", "[]", 3000, &result, &error), LP_OK)
        << (error ? error : "");
    ASSERT_NE(result, nullptr);
    EXPECT_STREQ(result, "42");
    lp_string_free(result);
    lp_string_free(error);

    EventResult received;
    lp_subscription* subscription = lp_subscribe(client, "tick", event, &received);
    ASSERT_NE(subscription, nullptr);
    for (int attempt = 0; attempt < 20; ++attempt) {
        ASSERT_EQ(lp_provider_emit_event(provider, "tick", "[1]"), LP_OK);
        std::unique_lock<std::mutex> lock(received.mutex);
        if (received.changed.wait_for(lock, std::chrono::milliseconds(100), [&] {
                return !received.value.empty();
            })) break;
    }
    EXPECT_EQ(received.value, "tick:[1]");

    lp_unsubscribe(subscription);
    lp_client_destroy(client);
    lp_provider_destroy(provider);
}

void runDeferredCompletionFromCallback(bool tls)
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor reservation(io,
        {boost::asio::ip::address_v4::loopback(), 0});
    const auto port = reservation.local_endpoint().port();
    reservation.close();
    const auto directory = std::filesystem::temp_directory_path()
        / ("logos-plain-deferred-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    if (tls) {
        std::filesystem::create_directories(directory);
        ASSERT_TRUE(writeSelfSignedCert(directory / "cert.pem", directory / "key.pem"));
    }
    nlohmann::json clientConfig = {
        {"protocol", tls ? "tcp_ssl" : "tcp"},
        {"host", "127.0.0.1"}, {"port", port}, {"verify_peer", false}};
    nlohmann::json serverConfig = clientConfig;
    if (tls) {
        serverConfig["cert_file"] = (directory / "cert.pem").string();
        serverConfig["key_file"] = (directory / "key.pem").string();
    }
    const std::string clientJson = clientConfig.dump();
    const std::string serverSet = nlohmann::json::array({serverConfig}).dump();

    DeferredState state;
    state.provider = lp_provider_create("plain_network_deferred", serverSet.c_str());
    ASSERT_NE(state.provider, nullptr);
    ASSERT_EQ(lp_provider_save_token(state.provider, "network_test", "secret"), LP_OK);
    ASSERT_EQ(lp_provider_register(state.provider, deferredDispatch, methods, token,
                                   &state), LP_OK);
    ASSERT_EQ(lp_token_save("plain_network_deferred", "secret"), LP_OK);
    state.client = lp_client_create("plain_network_deferred", "network_test",
                                     clientJson.c_str(), clientJson.c_str());
    ASSERT_NE(state.client, nullptr);

    char* ordinary = nullptr;
    char* error = nullptr;
    EXPECT_EQ(lp_invoke(state.client, "deferred", "[]", 500, &ordinary, &error), LP_OK)
        << (error ? error : "");
    ASSERT_NE(ordinary, nullptr);
    EXPECT_STREQ(ordinary, R"("completed")");
    lp_string_free(ordinary);
    lp_string_free(error);

    lp_subscription* subscription = lp_subscribe(
        state.client, "tick", callDeferredFromEvent, &state);
    ASSERT_NE(subscription, nullptr);
    for (int attempt = 0; attempt < 20; ++attempt) {
        ASSERT_EQ(lp_provider_emit_event(state.provider, "tick", "[]"), LP_OK);
        std::unique_lock<std::mutex> lock(state.mutex);
        if (state.changed.wait_for(lock, std::chrono::milliseconds(100), [&] {
            return state.done;
        })) break;
    }
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        EXPECT_TRUE(state.done);
        EXPECT_EQ(state.status, LP_OK);
        EXPECT_EQ(state.value, R"("completed")");
    }

    lp_unsubscribe(subscription);
    lp_client_destroy(state.client);
    lp_provider_destroy(state.provider);
    if (tls) std::filesystem::remove_all(directory);
}

TEST(PlainNetworkCAbi, TcpDeferredCompletionBypassesPublicEventCallback)
{
    runDeferredCompletionFromCallback(false);
}

TEST(PlainNetworkCAbi, TlsDeferredCompletionBypassesPublicEventCallback)
{
    runDeferredCompletionFromCallback(true);
}

void runAsyncResultDestroyWithQueuedNetworkEvent(bool tls)
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor reservation(io,
        {boost::asio::ip::address_v4::loopback(), 0});
    const auto port = reservation.local_endpoint().port();
    reservation.close();
    const auto directory = std::filesystem::temp_directory_path()
        / ("logos-plain-async-destroy-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    if (tls) {
        std::filesystem::create_directories(directory);
        ASSERT_TRUE(writeSelfSignedCert(directory / "cert.pem", directory / "key.pem"));
    }
    nlohmann::json clientConfig = {
        {"protocol", tls ? "tcp_ssl" : "tcp"},
        {"host", "127.0.0.1"}, {"port", port}, {"verify_peer", false}};
    nlohmann::json serverConfig = clientConfig;
    if (tls) {
        serverConfig["cert_file"] = (directory / "cert.pem").string();
        serverConfig["key_file"] = (directory / "key.pem").string();
    }
    const std::string clientJson = clientConfig.dump();
    const std::string serverSet = nlohmann::json::array({serverConfig}).dump();

    lp_provider* provider = lp_provider_create("plain_async_destroy", serverSet.c_str());
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_save_token(provider, "network_test", "secret"), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, dispatch, methods, token, nullptr), LP_OK);
    ASSERT_EQ(lp_token_save("plain_async_destroy", "secret"), LP_OK);
    AsyncDestroyState state;
    state.client = lp_client_create("plain_async_destroy", "network_test",
                                     clientJson.c_str(), clientJson.c_str());
    ASSERT_NE(state.client, nullptr);
    ASSERT_EQ(lp_client_set_subscription_status_cb(
        state.client, onAsyncDestroyStatus, &state), 1);
    lp_subscription* subscription = lp_subscribe(
        state.client, "tick", onAsyncDestroyEvent, &state);
    ASSERT_NE(subscription, nullptr);
    for (int attempt = 0; attempt < 200 && !state.armed; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(state.armed);
    ASSERT_EQ(lp_invoke_async(state.client, "answer", "[]", 1000,
                              onAsyncDestroyResult, &state), LP_OK);
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        ASSERT_TRUE(state.changed.wait_for(lock, std::chrono::seconds(2), [&] {
            return state.entered;
        }));
    }
    ASSERT_EQ(lp_provider_emit_event(provider, "tick", "[]"), LP_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.proceed = true;
    }
    state.changed.notify_all();
    if (!state.done) {
        for (int attempt = 0; attempt < 200 && !state.done; ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!state.done) {
        ADD_FAILURE() << "Async-result destruction blocked behind network event";
        std::_Exit(2); // Each GTest case runs in a separate CTest process.
    }
    EXPECT_EQ(state.success.load(), 1);
    EXPECT_EQ(state.events.load(), 0);
    lp_unsubscribe(subscription);
    lp_provider_destroy(provider);
    if (tls) std::filesystem::remove_all(directory);
}

TEST(PlainNetworkCAbi, TcpAsyncResultCanDestroyClientWithQueuedEvent)
{
    runAsyncResultDestroyWithQueuedNetworkEvent(false);
}

TEST(PlainNetworkCAbi, TlsAsyncResultCanDestroyClientWithQueuedEvent)
{
    runAsyncResultDestroyWithQueuedNetworkEvent(true);
}

struct TcpArrivals {
    std::mutex mutex;
    std::vector<long long> seen;
    std::atomic<int> done{0};
    std::atomic<int> succeeded{0};
};

char* recordTcpArrival(const char*, const char* argsJson, void* user)
{
    auto& arrivals = *static_cast<TcpArrivals*>(user);
    const auto args = nlohmann::json::parse(argsJson ? argsJson : "[]", nullptr, false);
    std::lock_guard<std::mutex> lock(arrivals.mutex);
    if (args.is_array() && !args.empty() && args[0].is_number_integer())
        arrivals.seen.push_back(args[0].get<long long>());
    return copy("true");
}

void onTcpArrivalResult(int ok, const char*, void* user)
{
    auto& arrivals = *static_cast<TcpArrivals*>(user);
    if (ok) ++arrivals.succeeded;
    ++arrivals.done;
}

// Detector: over TCP as well, each async call wrote its request whenever its worker got there.
TEST(PlainNetworkCAbi, TcpAsyncCallsReachTheProviderInTheOrderMade)
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor reservation(io,
        {boost::asio::ip::address_v4::loopback(), 0});
    const auto port = reservation.local_endpoint().port();
    reservation.close();
    const std::string config = std::string(R"({"protocol":"tcp","host":"127.0.0.1","port":)")
        + std::to_string(port) + "}";
    TcpArrivals arrivals;
    lp_provider* provider = lp_provider_create("plain_async_order", ("[" + config + "]").c_str());
    ASSERT_NE(provider, nullptr);
    // One call at a time, so the provider sees the wire order (as the local twin does).
    ASSERT_EQ(lp_provider_set_max_concurrent_calls(provider, 1), LP_OK);
    ASSERT_EQ(lp_provider_save_token(provider, "network_test", "secret"), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, recordTcpArrival, methods, token, &arrivals), LP_OK);
    ASSERT_EQ(lp_token_save("plain_async_order", "secret"), LP_OK);
    lp_client* client = lp_client_create("plain_async_order", "network_test",
                                         config.c_str(), config.c_str());
    ASSERT_NE(client, nullptr);

    constexpr int kCalls = 300;
    for (int i = 0; i < kCalls; ++i) {
        const std::string args = "[" + std::to_string(i) + "]";
        ASSERT_EQ(lp_invoke_async(client, "answer", args.c_str(), 10000,
                                  onTcpArrivalResult, &arrivals), LP_OK);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (arrivals.done.load() < kCalls && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    lp_client_destroy(client);
    lp_provider_destroy(provider);

    EXPECT_EQ(arrivals.succeeded.load(), kCalls);
    std::lock_guard<std::mutex> lock(arrivals.mutex);
    ASSERT_EQ(arrivals.seen.size(), static_cast<std::size_t>(kCalls));
    int inversions = 0;
    for (std::size_t i = 1; i < arrivals.seen.size(); ++i)
        if (arrivals.seen[i] < arrivals.seen[i - 1]) ++inversions;
    EXPECT_EQ(inversions, 0) << "requests reached the provider out of the order they were made";
}

std::uint16_t freePort()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor reservation(io,
        {boost::asio::ip::address_v4::loopback(), 0});
    return reservation.local_endpoint().port();
}

std::string networkConfig(const char* protocol, std::uint16_t port, const char* extra = "")
{
    return std::string(R"({"protocol":")") + protocol + R"(","host":"127.0.0.1","port":)"
        + std::to_string(port) + extra + "}";
}

std::filesystem::path certificateDirectory(const char* name, bool rsa = false)
{
    const auto directory = std::filesystem::temp_directory_path()
        / (std::string(name) + "_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    EXPECT_TRUE(writeSelfSignedCert(directory / "cert.pem", directory / "key.pem", rsa));
    return directory;
}

std::string tlsServerConfig(std::uint16_t port, const std::filesystem::path& directory)
{
    nlohmann::json config = nlohmann::json::parse(networkConfig("tcp_ssl", port));
    config["cert_file"] = (directory / "cert.pem").string();
    config["key_file"] = (directory / "key.pem").string();
    return config.dump();
}

bool waitForEvent(lp_provider* provider, EventResult& received, std::chrono::seconds budget)
{
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (provider) EXPECT_EQ(lp_provider_emit_event(provider, "tick", "[1]"), LP_OK);
        std::unique_lock<std::mutex> lock(received.mutex);
        if (received.changed.wait_for(lock, std::chrono::milliseconds(200),
                                      [&] { return !received.value.empty(); }))
            return true;
    }
    return false;
}

// Forwards bytes both ways, each chunk late by `delay`: a slow network path.
class DelayingProxy {
public:
    DelayingProxy(std::uint16_t target, std::chrono::milliseconds delay)
        : acceptor_(io_, {boost::asio::ip::address_v4::loopback(), 0}),
          target_(target), delay_(delay), thread_([this] { acceptLoop(); }) {}

    ~DelayingProxy()
    {
        // Closing the acceptor does not wake a blocked accept() on Linux.
        stopped_ = true;
        boost::system::error_code ignored;
        boost::asio::ip::tcp::socket wake(io_);
        wake.connect(acceptor_.local_endpoint(), ignored);
        thread_.join();
        acceptor_.close(ignored);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& socket : sockets_)
                socket->shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
        }
        for (auto& pump : pumps_) pump.join();
    }

    std::uint16_t port() const { return acceptor_.local_endpoint().port(); }

private:
    using Socket = std::shared_ptr<boost::asio::ip::tcp::socket>;

    void acceptLoop()
    {
        for (;;) {
            auto client = std::make_shared<boost::asio::ip::tcp::socket>(io_);
            boost::system::error_code error;
            acceptor_.accept(*client, error);
            if (error || stopped_) return;
            auto server = std::make_shared<boost::asio::ip::tcp::socket>(io_);
            server->connect({boost::asio::ip::address_v4::loopback(), target_}, error);
            if (error) continue;
            std::lock_guard<std::mutex> lock(mutex_);
            sockets_.push_back(client);
            sockets_.push_back(server);
            pumps_.emplace_back([this, client, server] { pump(client, server); });
            pumps_.emplace_back([this, client, server] { pump(server, client); });
        }
    }

    void pump(const Socket& from, const Socket& to)
    {
        std::array<char, 16384> buffer;
        boost::system::error_code error;
        for (;;) {
            const std::size_t count = from->read_some(boost::asio::buffer(buffer), error);
            if (error) break;
            std::this_thread::sleep_for(delay_);
            boost::asio::write(*to, boost::asio::buffer(buffer.data(), count), error);
            if (error) break;
        }
        to->shutdown(boost::asio::ip::tcp::socket::shutdown_send, error);
    }

    boost::asio::io_context io_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::uint16_t target_;
    std::chrono::milliseconds delay_;
    std::atomic<bool> stopped_{false};
    std::mutex mutex_;
    std::vector<Socket> sockets_;
    std::vector<std::thread> pumps_;
    std::thread thread_;
};

// Detector: a subscription dialled with 250 ms for DNS, TCP and TLS together,
// so behind a path slower than that a subscribe-only client never connected.
TEST(PlainNetworkCAbi, ASubscriptionConnectsOverASlowTlsPath)
{
    const auto certificates = certificateDirectory("plain_slow_tls");
    const std::uint16_t port = freePort();
    const std::string set = "[" + tlsServerConfig(port, certificates) + "]";
    lp_provider* provider = lp_provider_create("plain_slow_tls", set.c_str());
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_save_token(provider, "network_test", "secret"), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, dispatch, methods, token, nullptr), LP_OK);
    ASSERT_EQ(lp_token_save("plain_slow_tls", "secret"), LP_OK);

    DelayingProxy proxy(port, std::chrono::milliseconds(200));
    const std::string config = networkConfig("tcp_ssl", proxy.port(), R"(,"verify_peer":false)");
    lp_client* client = lp_client_create("plain_slow_tls", "network_test",
                                         config.c_str(), config.c_str());
    ASSERT_NE(client, nullptr);
    EventResult received;
    lp_subscription* subscription = lp_subscribe(client, "tick", event, &received);
    ASSERT_NE(subscription, nullptr);
    EXPECT_TRUE(waitForEvent(provider, received, std::chrono::seconds(6)))
        << "no event arrived through a 200 ms path";
    lp_unsubscribe(subscription);
    lp_client_destroy(client);
    lp_provider_destroy(provider);
    std::filesystem::remove_all(certificates);
}

// Detector: an event whose CBOR text was not valid UTF-8 threw out of the
// consumer's event thread and aborted the process; the Qt client shows U+FFFD.
// Detector: over tcp too, introspection of a provider built before
// logos-cpp-sdk #71 failed instead of asking its two older calls.
TEST(PlainNetworkCAbi, IntrospectionFallsBackForProvidersBeforeGetPluginInterface)
{
    using namespace logos::plain;
    LogosTransportConfig serverConfig;
    serverConfig.protocol = LogosProtocol::Tcp;
    serverConfig.port = freePort();
    const auto entries = [](const char* name, const char* type) {
        RpcMap entry;
        entry.emplace("name", RpcValue{name});
        if (type) entry.emplace("type", RpcValue{type});
        return RpcValue{RpcList{{RpcValue{std::move(entry)}}}};
    };
    logos::plain::abi::ServerEndpoint endpoint(serverConfig,
        [&](const CallMessage& request, std::shared_ptr<logos::plain::abi::GatePlace>) {
            ResultMessage result;
            result.id = request.id;
            result.ok = request.method != "getPluginInterface";
            if (!result.ok) {
                result.err = "no such method";
                result.errCode = "METHOD_NOT_FOUND";
            } else if (request.method == "getPluginMethods") {
                result.value = entries("legacyCall", "method");
            } else if (request.method == "getPluginEvents") {
                result.value = entries("legacyEvent", nullptr);
            }
            return result;
        },
        [](const MethodsMessage& request) {
            MethodsResultMessage result;
            result.id = request.id;
            result.ok = true;
            return result;
        },
        [](const TokenMessage&) {});
    ASSERT_TRUE(endpoint.start());

    const std::string config = networkConfig("tcp", serverConfig.port);
    lp_client* client = lp_client_create("legacy_network_provider", "network_test",
                                         config.c_str(), config.c_str());
    ASSERT_NE(client, nullptr);
    char* interface = lp_get_methods(client);
    lp_client_destroy(client);
    endpoint.stop();
    ASSERT_NE(interface, nullptr);
    const auto described = nlohmann::json::parse(interface);
    lp_string_free(interface);
    ASSERT_EQ(described.size(), 2u);
    EXPECT_EQ(described[0].at("name"), "legacyCall");
    EXPECT_EQ(described[1].at("name"), "legacyEvent");
    EXPECT_EQ(described[1].at("type"), "event");
}

TEST(PlainNetworkCAbi, AnEventWithInvalidUtf8ArrivesReplaced)
{
    using namespace logos::plain;
    LogosTransportConfig serverConfig;
    serverConfig.protocol = LogosProtocol::Tcp;
    serverConfig.port = freePort();
    serverConfig.codec = LogosWireCodec::Cbor;
    logos::plain::abi::ServerEndpoint endpoint(serverConfig,
        [](const CallMessage& request, std::shared_ptr<logos::plain::abi::GatePlace>) {
            ResultMessage result;
            result.id = request.id;
            result.ok = true;
            return result;
        },
        [](const MethodsMessage& request) {
            MethodsResultMessage result;
            result.id = request.id;
            result.ok = true;
            return result;
        },
        [](const TokenMessage&) {});
    ASSERT_TRUE(endpoint.start());

    const std::string config = networkConfig("tcp", serverConfig.port, R"(,"codec":"cbor")");
    lp_client* client = lp_client_create("plain_utf8_probe", "network_test",
                                         config.c_str(), config.c_str());
    ASSERT_NE(client, nullptr);
    EventResult received;
    lp_subscription* subscription = lp_subscribe(client, "tick", event, &received);
    ASSERT_NE(subscription, nullptr);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool arrived = false;
    while (!arrived && std::chrono::steady_clock::now() < deadline) {
        endpoint.emit("plain_utf8_probe", "tick", {RpcValue{std::string("bad \xff\xfe")}});
        std::unique_lock<std::mutex> lock(received.mutex);
        arrived = received.changed.wait_for(lock, std::chrono::milliseconds(200),
                                            [&] { return !received.value.empty(); });
    }
    ASSERT_TRUE(arrived);
    EXPECT_NE(received.value.find("\xEF\xBF\xBD"), std::string::npos) << received.value;
    lp_unsubscribe(subscription);
    lp_client_destroy(client);
    endpoint.stop();
}

struct SlowHandlers {
    std::atomic<int> methods{0};
    std::atomic<int> tokens{0};
};

char* slowMethods(void* user)
{
    ++static_cast<SlowHandlers*>(user)->methods;
    std::this_thread::sleep_for(std::chrono::seconds(2));
    return copy("[]");
}

int slowToken(const char*, const char*, void* user)
{
    ++static_cast<SlowHandlers*>(user)->tokens;
    std::this_thread::sleep_for(std::chrono::seconds(2));
    return LP_OK;
}

// Detector: getMethods and token delivery ran on the I/O thread every network
// endpoint in the process shares, so one slow module stalled the others.
TEST(PlainNetworkCAbi, ASlowModuleHandlerDoesNotStallAnotherEndpoint)
{
    const std::uint16_t slowPort = freePort();
    const std::uint16_t fastPort = freePort();
    const std::string slowConfig = networkConfig("tcp", slowPort);
    const std::string fastConfig = networkConfig("tcp", fastPort);
    SlowHandlers slow;
    lp_provider* slowProvider = lp_provider_create("plain_slow_handlers",
                                                   ("[" + slowConfig + "]").c_str());
    ASSERT_NE(slowProvider, nullptr);
    ASSERT_EQ(lp_provider_save_token(slowProvider, "network_test", "secret"), LP_OK);
    // A token push is taken only under the provider's own credential.
    ASSERT_EQ(lp_provider_save_token(slowProvider, "core", "core_secret"), LP_OK);
    ASSERT_EQ(lp_provider_register(slowProvider, dispatch, slowMethods, slowToken, &slow),
              LP_OK);
    lp_provider* fastProvider = lp_provider_create("plain_fast_handlers",
                                                   ("[" + fastConfig + "]").c_str());
    ASSERT_NE(fastProvider, nullptr);
    ASSERT_EQ(lp_provider_save_token(fastProvider, "network_test", "secret"), LP_OK);
    ASSERT_EQ(lp_provider_register(fastProvider, dispatch, methods, token, nullptr), LP_OK);
    ASSERT_EQ(lp_token_save("plain_fast_handlers", "secret"), LP_OK);
    lp_client* fast = lp_client_create("plain_fast_handlers", "network_test",
                                       fastConfig.c_str(), fastConfig.c_str());
    ASSERT_NE(fast, nullptr);

    const auto timedAnswer = [&] {
        char* result = nullptr;
        char* error = nullptr;
        const auto started = std::chrono::steady_clock::now();
        EXPECT_EQ(lp_invoke(fast, "answer", "[]", 5000, &result, &error), LP_OK)
            << (error ? error : "");
        lp_string_free(result);
        lp_string_free(error);
        return std::chrono::steady_clock::now() - started;
    };
    timedAnswer(); // connected first: what is timed below is dispatch, not a dial
    const auto waitFor = [](const std::atomic<int>& counter, int count) {
        for (int i = 0; i < 300 && counter.load() < count; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return counter.load() >= count;
    };

    // A token pushed to the slow module, as a Qt-side network client pushes it.
    LogosTransportConfig raw;
    raw.protocol = LogosProtocol::Tcp;
    raw.port = slowPort;
    std::string dialError;
    auto tokenWire = logos::plain::abi::connect(raw, std::chrono::seconds(2), dialError);
    ASSERT_TRUE(tokenWire) << dialError;
    tokenWire->sendToken({"core_secret", "someone", "their_token"});
    ASSERT_TRUE(waitFor(slow.tokens, 1));
    EXPECT_LT(timedAnswer(), std::chrono::milliseconds(800)) << "a token handler stalled it";
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    tokenWire->stop("done");

    // Its method list, as that client asks for it.
    auto wire = logos::plain::abi::connect(raw, std::chrono::seconds(2), dialError);
    ASSERT_TRUE(wire) << dialError;
    const int listedBefore = slow.methods.load();
    auto listed = wire->sendMethods({wire->nextId(), "secret", "plain_slow_handlers"});
    ASSERT_TRUE(waitFor(slow.methods, listedBefore + 1));
    EXPECT_LT(timedAnswer(), std::chrono::milliseconds(800)) << "getMethods stalled it";
    listed.wait_for(std::chrono::seconds(5));
    wire->stop("done");

    lp_client_destroy(fast);
    lp_provider_destroy(slowProvider);
    lp_provider_destroy(fastProvider);
}

// Detector: the TLS server set only a TLS 1.2 minimum and negotiated static-RSA
// suites without forward secrecy, which the Qt-side host refused.
TEST(PlainNetworkCAbi, TlsServerRefusesSuitesWithoutForwardSecrecy)
{
    const auto certificates = certificateDirectory("plain_tls_policy", true);
    const std::uint16_t port = freePort();
    const std::string set = "[" + tlsServerConfig(port, certificates) + "]";
    lp_provider* provider = lp_provider_create("plain_tls_policy", set.c_str());
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_register(provider, dispatch, methods, token, nullptr), LP_OK);

    const auto handshake = [port](const char* cipherList) {
        boost::asio::io_context io;
        boost::asio::ssl::context context(boost::asio::ssl::context::tls_client);
        SSL_CTX_set_max_proto_version(context.native_handle(), TLS1_2_VERSION);
        EXPECT_EQ(SSL_CTX_set_cipher_list(context.native_handle(), cipherList), 1);
        context.set_verify_mode(boost::asio::ssl::verify_none);
        boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream(io, context);
        boost::system::error_code error;
        stream.lowest_layer().connect({boost::asio::ip::address_v4::loopback(), port}, error);
        if (!error) stream.handshake(boost::asio::ssl::stream_base::client, error);
        return !error;
    };
    EXPECT_FALSE(handshake("AES128-GCM-SHA256:AES256-GCM-SHA384"))
        << "static-RSA key exchange was negotiated";
    EXPECT_TRUE(handshake("ECDHE-RSA-AES128-GCM-SHA256")) << "a forward-secret suite was refused";
    lp_provider_destroy(provider);
    std::filesystem::remove_all(certificates);
}

TEST(PlainNetworkCAbi, SilentTlsPeerCannotOutliveInvocationDeadline)
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor listener(io,
        {boost::asio::ip::address_v4::loopback(), 0});
    const auto port = listener.local_endpoint().port();
    std::thread peer([&] {
        boost::asio::ip::tcp::socket socket(io);
        listener.accept(socket);
        std::this_thread::sleep_for(std::chrono::seconds(2));
    });

    const std::string config = std::string(R"({"protocol":"tcp_ssl","host":"127.0.0.1","port":)")
        + std::to_string(port) + R"(,"verify_peer":false})";
    EXPECT_EQ(lp_token_save("plain_tls_deadline", "secret"), LP_OK);
    lp_client* client = lp_client_create("plain_tls_deadline", "network_test",
                                         config.c_str(), config.c_str());
    EXPECT_NE(client, nullptr);
    char* value = nullptr;
    char* error = nullptr;
    const auto started = std::chrono::steady_clock::now();
    const int status = lp_invoke(client, "echo", "[]", 150, &value, &error);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    lp_string_free(value);
    lp_string_free(error);
    lp_client_destroy(client);
    peer.join();

    EXPECT_EQ(status, LP_ERR_UNAVAILABLE);
    EXPECT_LT(elapsed, std::chrono::seconds(1));
}

TEST(PlainNetworkCAbi, ClientDestroyCancelsSubscriptionDialToSilentTlsPeer)
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor listener(io,
        {boost::asio::ip::address_v4::loopback(), 0});
    const auto port = listener.local_endpoint().port();
    std::atomic<bool> accepted{false};
    std::thread peer([&] {
        boost::asio::ip::tcp::socket socket(io);
        listener.accept(socket);
        accepted = true;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    });

    const std::string config = std::string(R"({"protocol":"tcp_ssl","host":"127.0.0.1","port":)")
        + std::to_string(port) + R"(,"verify_peer":false})";
    lp_client* client = lp_client_create("plain_tls_subscription", "network_test",
                                         config.c_str(), config.c_str());
    EXPECT_NE(client, nullptr);
    EventResult eventState;
    lp_subscription* subscription = lp_subscribe(client, "tick", event, &eventState);
    EXPECT_NE(subscription, nullptr);
    for (int attempt = 0; attempt < 100 && !accepted; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto started = std::chrono::steady_clock::now();
    lp_client_destroy(client);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    lp_unsubscribe(subscription);
    peer.join();

    EXPECT_TRUE(accepted);
    EXPECT_LT(elapsed, std::chrono::seconds(1));
}

} // namespace
