#include "logos_protocol.h"

#include <gtest/gtest.h>

#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_context.hpp>

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

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
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    int status = LP_ERR_INTERNAL;
    std::string value;
};

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

TEST(PlainNetworkCAbi, TcpDeferredCompletionBypassesPublicEventCallback)
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor reservation(io,
        {boost::asio::ip::address_v4::loopback(), 0});
    const auto port = reservation.local_endpoint().port();
    reservation.close();
    const std::string config = std::string(R"({"protocol":"tcp","host":"127.0.0.1","port":)")
        + std::to_string(port) + "}";

    DeferredState state;
    state.provider = lp_provider_create("plain_network_deferred", ("[" + config + "]").c_str());
    ASSERT_NE(state.provider, nullptr);
    ASSERT_EQ(lp_provider_save_token(state.provider, "network_test", "secret"), LP_OK);
    ASSERT_EQ(lp_provider_register(state.provider, deferredDispatch, methods, token,
                                   &state), LP_OK);
    ASSERT_EQ(lp_token_save("plain_network_deferred", "secret"), LP_OK);
    state.client = lp_client_create("plain_network_deferred", "network_test",
                                     config.c_str(), config.c_str());
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
    EXPECT_TRUE(state.done);
    EXPECT_EQ(state.status, LP_OK);
    EXPECT_EQ(state.value, R"("completed")");

    lp_unsubscribe(subscription);
    lp_client_destroy(state.client);
    lp_provider_destroy(state.provider);
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
        + std::to_string(port) + R"(,"verifyPeer":false})";
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
        + std::to_string(port) + R"(,"verifyPeer":false})";
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
