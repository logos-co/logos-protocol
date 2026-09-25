// One dispatch gate per provider, shared by every listener, and a control lane
// for token delivery and metadata that never waits behind a business call.
#include "logos_protocol.h"

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

char* copyText(const std::string& value)
{
    auto* result = static_cast<char*>(std::malloc(value.size() + 1));
    std::memcpy(result, value.c_str(), value.size() + 1);
    return result;
}

void useInstance(const std::string& prefix)
{
#ifdef _WIN32
    const std::string instance = prefix + std::to_string(::_getpid());
    ASSERT_EQ(::_putenv_s("LOGOS_INSTANCE_ID", instance.c_str()), 0);
#else
    const std::string instance = prefix + std::to_string(::getpid());
    ASSERT_EQ(::setenv("LOGOS_INSTANCE_ID", instance.c_str(), 1), 0);
#endif
}

std::uint16_t freePort()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor reservation(
        io, {boost::asio::ip::make_address("127.0.0.1"), 0});
    return reservation.local_endpoint().port();
}

struct Overlap {
    std::atomic<int> running{0};
    std::atomic<int> peak{0};
    std::mutex mutex;
    std::condition_variable changed;
    bool blocking = false;
    bool released = false;
    bool blocked = false;
    std::string tokenModule;
};

char* overlapDispatch(const char* method, const char*, void* userData)
{
    auto& overlap = *static_cast<Overlap*>(userData);
    const int now = ++overlap.running;
    int seen = overlap.peak.load();
    while (now > seen && !overlap.peak.compare_exchange_weak(seen, now)) {}
    if (std::strcmp(method, "block") == 0) {
        std::unique_lock<std::mutex> lock(overlap.mutex);
        overlap.blocked = true;
        overlap.changed.notify_all();
        overlap.changed.wait_for(lock, std::chrono::seconds(5), [&] { return overlap.released; });
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    --overlap.running;
    return copyText("true");
}

char* overlapMethods(void*)
{
    return copyText(R"json([{"type":"method","name":"work","signature":"work()","returnType":"bool","isInvokable":true,"parameters":[]}])json");
}

int overlapToken(const char* module, const char*, void* userData)
{
    auto& overlap = *static_cast<Overlap*>(userData);
    std::lock_guard<std::mutex> lock(overlap.mutex);
    overlap.tokenModule = module;
    return LP_OK;
}

void callRepeatedly(const char* target, const char* transport, int calls,
                    std::atomic<int>& succeeded)
{
    lp_client* client = lp_client_create(target, "caller", transport, nullptr);
    ASSERT_NE(client, nullptr);
    for (int call = 0; call < calls; ++call) {
        char* result = nullptr;
        char* error = nullptr;
        if (lp_invoke(client, "work", "[]", 5000, &result, &error) == LP_OK) ++succeeded;
        lp_string_free(result);
        lp_string_free(error);
    }
    lp_client_destroy(client);
}

// Detector: each listener ran its own worker, so a module limited to one call
// ran one over the local socket and another over tcp at the same time (peak 2 on 7b5adc1).
TEST(PlainDispatchGate, ASingleProviderNeverOverlapsCallsAcrossListeners)
{
    useInstance("gate_single_");
    const std::uint16_t port = freePort();
    const std::string tcp = R"({"protocol":"tcp","host":"127.0.0.1","port":)"
        + std::to_string(port) + "}";
    Overlap overlap;
    lp_provider* provider = lp_provider_create(
        "gate_single", (R"([{"protocol":"qt_remote_plain"},)" + tcp + "]").c_str());
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_set_max_concurrent_calls(provider, 1), LP_OK);
    ASSERT_EQ(lp_provider_save_token(provider, "caller", "secret"), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, overlapDispatch, overlapMethods, overlapToken,
                                   &overlap), LP_OK);
    ASSERT_EQ(lp_token_save("gate_single", "secret"), LP_OK);

    std::atomic<int> succeeded{0};
    std::vector<std::thread> callers;
    for (int i = 0; i < 4; ++i) {
        callers.emplace_back([&] {
            callRepeatedly("gate_single", R"({"protocol":"qt_remote_plain"})", 5, succeeded);
        });
        callers.emplace_back([&] { callRepeatedly("gate_single", tcp.c_str(), 5, succeeded); });
    }
    for (auto& caller : callers) caller.join();
    lp_provider_destroy(provider);

    EXPECT_EQ(succeeded.load(), 40);
    EXPECT_EQ(overlap.peak.load(), 1);
}

// The bound holds across listeners too (peak 3 against a bound of 2 on 7b5adc1).
TEST(PlainDispatchGate, AMultiProviderIsBoundedAcrossListeners)
{
    useInstance("gate_multi_");
    const std::uint16_t port = freePort();
    const std::string tcp = R"({"protocol":"tcp","host":"127.0.0.1","port":)"
        + std::to_string(port) + "}";
    Overlap overlap;
    lp_provider* provider = lp_provider_create(
        "gate_multi", (R"([{"protocol":"qt_remote_plain"},)" + tcp + "]").c_str());
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_set_max_concurrent_calls(provider, 2), LP_OK);
    ASSERT_EQ(lp_provider_save_token(provider, "caller", "secret"), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, overlapDispatch, overlapMethods, overlapToken,
                                   &overlap), LP_OK);
    ASSERT_EQ(lp_token_save("gate_multi", "secret"), LP_OK);

    std::atomic<int> succeeded{0};
    std::vector<std::thread> callers;
    for (int i = 0; i < 4; ++i) {
        callers.emplace_back([&] {
            callRepeatedly("gate_multi", R"({"protocol":"qt_remote_plain"})", 5, succeeded);
        });
        callers.emplace_back([&] { callRepeatedly("gate_multi", tcp.c_str(), 5, succeeded); });
    }
    for (auto& caller : callers) caller.join();
    lp_provider_destroy(provider);

    EXPECT_EQ(succeeded.load(), 40);
    EXPECT_LE(overlap.peak.load(), 2);
}

// Detector: the handshake's informModuleToken queued behind the call pool, so
// with one call running a token push waited for it and failed (-3 after 2.0 s on 7b5adc1).
TEST(PlainDispatchGate, ATokenPushIsNotQueuedBehindABusinessCall)
{
    useInstance("gate_control_");
    Overlap overlap;
    lp_provider* provider = lp_provider_create("gate_control",
                                               R"([{"protocol":"qt_remote_plain"}])");
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_set_max_concurrent_calls(provider, 1), LP_OK);
    ASSERT_EQ(lp_provider_save_token(provider, "core", "anchor"), LP_OK);
    ASSERT_EQ(lp_provider_save_token(provider, "caller", "secret"), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, overlapDispatch, overlapMethods, overlapToken,
                                   &overlap), LP_OK);
    ASSERT_EQ(lp_token_save("gate_control", "secret"), LP_OK);

    lp_client* blocker = lp_client_create("gate_control", "caller", nullptr, nullptr);
    ASSERT_NE(blocker, nullptr);
    std::thread blocked([&] {
        char* result = nullptr;
        char* error = nullptr;
        lp_invoke(blocker, "block", "[]", 8000, &result, &error);
        lp_string_free(result);
        lp_string_free(error);
    });
    {
        std::unique_lock<std::mutex> lock(overlap.mutex);
        ASSERT_TRUE(overlap.changed.wait_for(lock, std::chrono::seconds(5),
                                             [&] { return overlap.blocked; }));
    }

    ASSERT_EQ(lp_grant_host_services(R"(["token_delivery"])"), LP_OK);
    const auto started = std::chrono::steady_clock::now();
    const int pushed = lp_inform_module_token_to(nullptr, "anchor", "gate_control", "peer",
                                                 "peer-token", 1000);
    const auto took = std::chrono::steady_clock::now() - started;
    ASSERT_EQ(lp_grant_host_services("[]"), LP_OK);

    char* interface = lp_get_methods(blocker);
    EXPECT_NE(interface, nullptr);
    lp_string_free(interface);

    {
        std::lock_guard<std::mutex> lock(overlap.mutex);
        overlap.released = true;
        overlap.changed.notify_all();
    }
    blocked.join();
    lp_client_destroy(blocker);
    lp_provider_destroy(provider);

    EXPECT_EQ(pushed, LP_OK);
    EXPECT_LT(took, std::chrono::milliseconds(900));
    std::lock_guard<std::mutex> lock(overlap.mutex);
    EXPECT_EQ(overlap.tokenModule, "peer");
}

} // namespace
