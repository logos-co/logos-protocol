#include "logos_protocol.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

char* copyString(const std::string& value)
{
    auto* result = static_cast<char*>(std::malloc(value.size() + 1));
    std::memcpy(result, value.c_str(), value.size() + 1);
    return result;
}

struct Fixture {
    std::string informedModule;
    std::string informedToken;
};

char* dispatch(const char* method, const char* argsJson, void*)
{
    const auto args = nlohmann::json::parse(argsJson);
    if (std::strcmp(method, "echo") == 0)
        return copyString(nlohmann::json("plain:" + args.at(0).get<std::string>()).dump());
    return copyString("null");
}

char* methods(void*)
{
    return copyString(R"json([{"type":"method","name":"echo","signature":"echo(string)","returnType":"string","isInvokable":true,"parameters":[]}])json");
}

int token(const char* module, const char* value, void* userData)
{
    auto* fixture = static_cast<Fixture*>(userData);
    fixture->informedModule = module;
    fixture->informedToken = value;
    return LP_OK;
}

struct EventResult {
    std::mutex mutex;
    std::condition_variable changed;
    bool received = false;
    std::string name;
    std::string data;
};

void onEvent(const char* name, const char* data, void* userData)
{
    auto* result = static_cast<EventResult*>(userData);
    {
        std::lock_guard<std::mutex> lock(result->mutex);
        result->received = true;
        result->name = name;
        result->data = data;
    }
    result->changed.notify_all();
}

bool waitUntil(const std::function<bool()>& predicate,
               std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

struct StatusResult {
    std::mutex mutex;
    std::vector<std::pair<int, unsigned long long>> edges;
};

void onStatus(int status, unsigned long long generation, const char*, void* userData)
{
    auto* result = static_cast<StatusResult*>(userData);
    std::lock_guard<std::mutex> lock(result->mutex);
    result->edges.emplace_back(status, generation);
}

bool sawStatus(StatusResult& result, int status, unsigned long long generation)
{
    std::lock_guard<std::mutex> lock(result.mutex);
    return std::find(result.edges.begin(), result.edges.end(),
                     std::pair<int, unsigned long long>{status, generation})
        != result.edges.end();
}

TEST(QtRemotePlainCabiTest, ProviderClientTokenIntrospectionAndEventNeedNoQt)
{
#ifndef _WIN32
    const std::string instance = "qtro_cabi_" + std::to_string(::getpid());
    ::setenv("LOGOS_INSTANCE_ID", instance.c_str(), 1);
#endif
    Fixture fixture;
    lp_provider* provider = lp_provider_create(
        "plain_fixture", R"([{"protocol":"qt_remote_plain"}])");
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_save_token(provider, "core", "anchor"), LP_OK);
    ASSERT_EQ(lp_provider_save_token(provider, "caller", "secret"), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, dispatch, methods, token, &fixture), LP_OK);

    ASSERT_EQ(lp_token_save("plain_fixture", "secret"), LP_OK);
    lp_client* client = lp_client_create(
        "plain_fixture", "caller",
        R"({"protocol":"qt_remote_plain"})",
        R"({"protocol":"qt_remote_plain"})");
    ASSERT_NE(client, nullptr);

    char* result = nullptr;
    char* error = nullptr;
    ASSERT_EQ(lp_invoke(client, "echo", R"(["hello"])", 1000, &result, &error), LP_OK)
        << (error ? error : "");
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(nlohmann::json::parse(result), "plain:hello");
    lp_string_free(result);
    lp_string_free(error);

    char* interface = lp_get_methods(client);
    ASSERT_NE(interface, nullptr);
    EXPECT_EQ(nlohmann::json::parse(interface).at(0).at("name"), "echo");
    lp_string_free(interface);

    EXPECT_EQ(lp_inform_module_token(client, "anchor", "peer", "peer-token"), LP_OK);
    EXPECT_EQ(fixture.informedModule, "peer");
    EXPECT_EQ(fixture.informedToken, "peer-token");

    EventResult event;
    lp_subscription* subscription = lp_subscribe(client, "tick", onEvent, &event);
    ASSERT_NE(subscription, nullptr);
    ASSERT_TRUE(waitUntil([&] { return lp_client_subscription_generation(client) == 1; }));
    ASSERT_EQ(lp_provider_emit_event(provider, "tick", R"(["payload",7])"), LP_OK);
    {
        std::unique_lock<std::mutex> lock(event.mutex);
        ASSERT_TRUE(event.changed.wait_for(lock, std::chrono::seconds(1), [&] {
            return event.received;
        }));
    }
    EXPECT_EQ(event.name, "tick");
    EXPECT_EQ(nlohmann::json::parse(event.data),
              nlohmann::json::array({"payload", 7}));

    lp_unsubscribe(subscription);
    lp_client_destroy(client);
    lp_provider_destroy(provider);
}

TEST(QtRemotePlainCabiTest, DeferredSubscriptionReconnectsAndManualPolicyHolds)
{
#ifndef _WIN32
    const std::string instance = "qtro_cabi_restart_" + std::to_string(::getpid());
    ::setenv("LOGOS_INSTANCE_ID", instance.c_str(), 1);
#endif
    ASSERT_EQ(lp_token_save("restart_fixture", "secret"), LP_OK);
    lp_client* client = lp_client_create(
        "restart_fixture", "caller",
        R"({"protocol":"qt_remote_plain"})",
        R"({"protocol":"qt_remote_plain"})");
    ASSERT_NE(client, nullptr);

    StatusResult status;
    ASSERT_EQ(lp_client_set_subscription_status_cb(client, onStatus, &status), 1);
    EventResult event;
    lp_subscription* subscription = lp_subscribe(client, "tick", onEvent, &event);
    ASSERT_NE(subscription, nullptr);
    char* pending = lp_pending_subscriptions(client);
    ASSERT_NE(pending, nullptr);
    EXPECT_EQ(nlohmann::json::parse(pending),
              nlohmann::json::array({"restart_fixture::tick"}));
    lp_string_free(pending);

    Fixture fixture;
    auto makeProvider = [&]() {
        lp_provider* provider = lp_provider_create(
            "restart_fixture", R"([{"protocol":"qt_remote_plain"}])");
        if (!provider) return provider;
        if (lp_provider_save_token(provider, "caller", "secret") != LP_OK
            || lp_provider_register(provider, dispatch, methods, token, &fixture) != LP_OK) {
            lp_provider_destroy(provider);
            return static_cast<lp_provider*>(nullptr);
        }
        return provider;
    };

    lp_provider* provider = makeProvider();
    ASSERT_NE(provider, nullptr);
    ASSERT_TRUE(waitUntil([&] { return sawStatus(status, LP_SUB_ARMED, 1); }));
    lp_provider_destroy(provider);
    ASSERT_TRUE(waitUntil([&] { return sawStatus(status, LP_SUB_LOST, 1); }));

    provider = makeProvider();
    ASSERT_NE(provider, nullptr);
    ASSERT_TRUE(waitUntil([&] { return sawStatus(status, LP_SUB_ARMED, 2); }));

    ASSERT_EQ(lp_client_set_subscription_options(client, R"({"restart":"manual"})"), 1);
    lp_provider_destroy(provider);
    ASSERT_TRUE(waitUntil([&] { return sawStatus(status, LP_SUB_HELD, 2); }));
    provider = makeProvider();
    ASSERT_NE(provider, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(lp_client_subscription_generation(client), 2u);
    ASSERT_EQ(lp_client_rearm_subscriptions(client), 1);
    ASSERT_TRUE(waitUntil([&] { return sawStatus(status, LP_SUB_ARMED, 3); }));

    lp_unsubscribe(subscription);
    lp_client_destroy(client);
    lp_provider_destroy(provider);
}

} // namespace
