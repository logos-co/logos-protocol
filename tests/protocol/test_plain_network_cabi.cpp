#include "logos_protocol.h"

#include <gtest/gtest.h>

#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_context.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

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

} // namespace
