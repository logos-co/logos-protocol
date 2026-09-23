#include "logos_protocol.h"
#include "logos_codec.h"
#include "implementations/qt_remote_plain/qtro_transport.h"

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
#include <numeric>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
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

struct ValidatorFixture {
    std::string acceptedToken;
    std::string transport;
};

int validateToken(const char* value, const char* transport, void* userData)
{
    auto* fixture = static_cast<ValidatorFixture*>(userData);
    fixture->transport = transport ? transport : "";
    return value && fixture->acceptedToken == value ? LP_OK : LP_ERR_UNAVAILABLE;
}

char* dispatch(const char* method, const char* argsJson, void*)
{
    const auto args = nlohmann::json::parse(argsJson);
    if (std::strcmp(method, "echo") == 0)
        return copyString(nlohmann::json("plain:" + args.at(0).get<std::string>()).dump());
    if (std::strcmp(method, "echoBytes") == 0)
        return copyString(args.at(0).dump());
    if (std::strcmp(method, "mapCollision") == 0
        || std::strcmp(method, "resultCollision") == 0
        || std::strcmp(method, "lidlResult") == 0)
        return copyString(R"({"success":true,"value":42,"error":null})");
    return copyString("null");
}

char* methods(void*)
{
    return copyString(R"json([{"type":"method","name":"echo","signature":"echo(string)","returnType":"string","isInvokable":true,"parameters":[]},{"type":"method","name":"echoBytes","signature":"echoBytes(QByteArray)","returnType":"QByteArray","isInvokable":true,"parameters":[]},{"type":"method","name":"mapCollision","signature":"mapCollision()","returnType":"QVariantMap","isInvokable":true,"parameters":[]},{"type":"method","name":"resultCollision","signature":"resultCollision()","returnType":"LogosResult","isInvokable":true,"parameters":[]},{"type":"method","name":"lidlResult","signature":"lidlResult()","returnType":"result","isInvokable":true,"parameters":[]}])json");
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

struct ReentrantEventResult {
    lp_client* client = nullptr;
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    int status = LP_ERR_INTERNAL;
    std::string value;
};

struct BlockingEventResult {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
};

void onBlockingEvent(const char*, const char*, void* userData)
{
    auto* result = static_cast<BlockingEventResult*>(userData);
    std::unique_lock<std::mutex> lock(result->mutex);
    result->entered = true;
    result->changed.notify_all();
    result->changed.wait(lock, [&] { return result->release; });
}

struct DeferredFixture {
    lp_provider* provider = nullptr;
};

char* deferredDispatch(const char* method, const char*, void* userData)
{
    auto* fixture = static_cast<DeferredFixture*>(userData);
    if (std::strcmp(method, "deferred") != 0) return copyString("null");
    lp_provider* provider = fixture->provider;
    std::thread([provider] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        (void)lp_provider_emit_event(
            provider, "__logos_call_complete__", R"(["deferred-1","complete"])");
    }).detach();
    return copyString(R"({"__logos_pending_call__":"deferred-1"})");
}

char* deferredMethods(void*)
{
    return copyString(R"json([{"type":"method","name":"deferred","signature":"deferred()","returnType":"string","isInvokable":true,"parameters":[]}])json");
}

struct DestroyEventResult {
    lp_client* client = nullptr;
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
};

struct BlockingDispatchFixture {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
};

char* blockingDispatch(const char*, const char*, void* userData)
{
    auto* fixture = static_cast<BlockingDispatchFixture*>(userData);
    std::unique_lock<std::mutex> lock(fixture->mutex);
    fixture->entered = true;
    fixture->changed.notify_all();
    fixture->changed.wait(lock, [&] { return fixture->release; });
    return copyString("true");
}

char* blockingMethods(void*)
{
    return copyString(R"json([{"type":"method","name":"block","signature":"block()","returnType":"bool","isInvokable":true,"parameters":[]}])json");
}

void onDestroyingEvent(const char*, const char*, void* userData)
{
    auto* result = static_cast<DestroyEventResult*>(userData);
    lp_client_destroy(result->client);
    {
        std::lock_guard<std::mutex> lock(result->mutex);
        result->done = true;
    }
    result->changed.notify_all();
}

void onReentrantEvent(const char*, const char*, void* userData)
{
    auto* result = static_cast<ReentrantEventResult*>(userData);
    char* value = nullptr;
    char* error = nullptr;
    const int status = lp_invoke(
        result->client, "echo", R"(["from-event"])", 1000, &value, &error);
    {
        std::lock_guard<std::mutex> lock(result->mutex);
        result->status = status;
        result->value = value ? value : "";
        result->done = true;
    }
    lp_string_free(value);
    lp_string_free(error);
    result->changed.notify_all();
}

struct StatusResult {
    std::mutex mutex;
    std::vector<std::pair<int, unsigned long long>> edges;
};

struct DisconnectDuringEventResult {
    lp_client* client = nullptr;
    bool destroyInCallback = false;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool proceed = false;
    std::atomic<bool> done{false};
};

void onDisconnectStatus(int, unsigned long long, const char*, void*) {}

void onDisconnectingEvent(const char*, const char*, void* userData)
{
    auto* result = static_cast<DisconnectDuringEventResult*>(userData);
    {
        std::unique_lock<std::mutex> lock(result->mutex);
        result->entered = true;
        result->changed.notify_all();
        result->changed.wait(lock, [&] { return result->proceed; });
    }
    char* value = nullptr;
    char* error = nullptr;
    (void)lp_invoke(result->client, "echo", R"(["after-disconnect"])",
                    200, &value, &error);
    lp_string_free(value);
    lp_string_free(error);
    if (result->destroyInCallback) lp_client_destroy(result->client);
    result->done = true;
}

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

void setInstanceId(const std::string& prefix)
{
#ifdef _WIN32
    const std::string instance = prefix + std::to_string(::_getpid());
    ASSERT_EQ(::_putenv_s("LOGOS_INSTANCE_ID", instance.c_str()), 0);
#else
    const std::string instance = prefix + std::to_string(::getpid());
    ASSERT_EQ(::setenv("LOGOS_INSTANCE_ID", instance.c_str(), 1), 0);
#endif
}

void runDisconnectDuringEvent(bool withStatus, bool manualRestart,
                              bool destroyInCallback,
                              const std::string& id)
{
    setInstanceId(id);
    Fixture fixture;
    lp_provider* provider = lp_provider_create("d", nullptr);
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_save_token(provider, "caller", "secret"), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, dispatch, methods, token, &fixture), LP_OK);
    ASSERT_EQ(lp_token_save("d", "secret"), LP_OK);
    lp_client* client = lp_client_create("d", "caller", nullptr, nullptr);
    ASSERT_NE(client, nullptr);
    if (manualRestart)
        ASSERT_EQ(lp_client_set_subscription_options(
            client, R"({"restart":"manual"})"), 1);
    if (withStatus)
        ASSERT_EQ(lp_client_set_subscription_status_cb(
            client, onDisconnectStatus, nullptr), 1);
    DisconnectDuringEventResult event;
    event.client = client;
    event.destroyInCallback = destroyInCallback;
    lp_subscription* subscription = lp_subscribe(
        client, "tick", onDisconnectingEvent, &event);
    ASSERT_NE(subscription, nullptr);
    ASSERT_TRUE(waitUntil([&] { return lp_client_subscription_generation(client) == 1; }));
    ASSERT_EQ(lp_provider_emit_event(provider, "tick", "[]"), LP_OK);
    {
        std::unique_lock<std::mutex> lock(event.mutex);
        ASSERT_TRUE(event.changed.wait_for(lock, std::chrono::seconds(2), [&] {
            return event.entered;
        }));
    }
    lp_provider_destroy(provider);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        std::lock_guard<std::mutex> lock(event.mutex);
        event.proceed = true;
    }
    event.changed.notify_all();
    ASSERT_TRUE(waitUntil([&] { return event.done.load(); }, std::chrono::seconds(2)));
    lp_unsubscribe(subscription);
    if (!destroyInCallback) lp_client_destroy(client);
}

enum class DestroyCallbackKind { AsyncResult, SubscriptionStatus };
enum class QueuedWork { None, Event, Disconnect };

struct DestroyFromCallbackResult {
    lp_client* client = nullptr;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool proceed = false;
    std::atomic<bool> done{false};
    std::atomic<int> asyncSuccess{-1};
    std::atomic<int> deliveredEvents{0};
};

void destroyAfterQueuedWork(DestroyFromCallbackResult* result)
{
    {
        std::unique_lock<std::mutex> lock(result->mutex);
        result->entered = true;
        result->changed.notify_all();
        result->changed.wait(lock, [&] { return result->proceed; });
    }
    lp_client_destroy(result->client);
    result->client = nullptr;
    result->done = true;
}

void onDestroyingAsyncResult(int success, const char*, void* userData)
{
    auto* result = static_cast<DestroyFromCallbackResult*>(userData);
    result->asyncSuccess = success;
    destroyAfterQueuedWork(result);
}

void onDestroyingStatus(int status, unsigned long long, const char*, void* userData)
{
    if (status == LP_SUB_ARMED)
        destroyAfterQueuedWork(static_cast<DestroyFromCallbackResult*>(userData));
}

void onQueuedEvent(const char*, const char*, void* userData)
{
    ++static_cast<DestroyFromCallbackResult*>(userData)->deliveredEvents;
}

void runDestroyWithQueuedCallback(DestroyCallbackKind kind, QueuedWork queued,
                                  const std::string& id)
{
    setInstanceId(id);
    Fixture fixture;
    lp_provider* provider = lp_provider_create("destroy_queued_fixture", nullptr);
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_save_token(provider, "caller", "secret"), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, dispatch, methods, token, &fixture), LP_OK);
    ASSERT_EQ(lp_token_save("destroy_queued_fixture", "secret"), LP_OK);
    DestroyFromCallbackResult result;
    result.client = lp_client_create("destroy_queued_fixture", "caller", nullptr, nullptr);
    ASSERT_NE(result.client, nullptr);
    ASSERT_EQ(lp_client_set_subscription_status_cb(result.client,
        kind == DestroyCallbackKind::SubscriptionStatus
            ? onDestroyingStatus : onDisconnectStatus, &result), 1);
    lp_subscription* subscription = lp_subscribe(
        result.client, "tick", onQueuedEvent, &result);
    ASSERT_NE(subscription, nullptr);
    if (kind == DestroyCallbackKind::AsyncResult) {
        ASSERT_TRUE(waitUntil([&] {
            return lp_client_subscription_generation(result.client) == 1;
        }));
        ASSERT_EQ(lp_invoke_async(result.client, "echo", R"(["async"])", 1000,
                                  onDestroyingAsyncResult, &result), LP_OK);
    }
    {
        std::unique_lock<std::mutex> lock(result.mutex);
        ASSERT_TRUE(result.changed.wait_for(lock, std::chrono::seconds(2), [&] {
            return result.entered;
        }));
    }
    if (queued == QueuedWork::Event)
        ASSERT_EQ(lp_provider_emit_event(provider, "tick", "[]"), LP_OK);
    else if (queued == QueuedWork::Disconnect) {
        lp_provider_destroy(provider);
        provider = nullptr;
    }
    // Give the reader/status worker time to queue work behind callbackMutex.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    {
        std::lock_guard<std::mutex> lock(result.mutex);
        result.proceed = true;
    }
    result.changed.notify_all();
    if (!waitUntil([&] { return result.done.load(); },
                   std::chrono::seconds(2))) {
        ADD_FAILURE() << "Client destruction blocked behind a queued callback";
        std::_Exit(2); // Each GTest case runs in its own CTest process.
    }
    if (kind == DestroyCallbackKind::AsyncResult)
        EXPECT_EQ(result.asyncSuccess.load(), 1);
    EXPECT_EQ(result.deliveredEvents.load(), 0);
    lp_unsubscribe(subscription);
    lp_provider_destroy(provider);
}

TEST(QtRemotePlainCabiTest, ProviderClientTokenIntrospectionAndEventNeedNoQt)
{
    setInstanceId("qtro_cabi_");
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

    std::vector<std::uint8_t> allBytes(256);
    std::iota(allBytes.begin(), allBytes.end(), std::uint8_t{0});
    const auto taggedBytes = logos::bytesToJson(allBytes);
    const auto bytesArgs = nlohmann::json::array({taggedBytes}).dump();
    result = nullptr;
    error = nullptr;
    ASSERT_EQ(lp_invoke(client, "echoBytes", bytesArgs.c_str(), 1000, &result, &error), LP_OK)
        << (error ? error : "");
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(nlohmann::json::parse(result), taggedBytes);
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
    lp_subscription* subscription = lp_subscribe(client, "", onEvent, &event);
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

TEST(QtRemotePlainCabiTest, ProviderCanValidatePersistentTokensOnDemand)
{
    setInstanceId("qtro_cabi_validator_");
    Fixture fixture;
    ValidatorFixture validator{"operator-secret", {}};
    lp_provider* provider = lp_provider_create(
        "validator_fixture", R"([{"protocol":"qt_remote_plain"}])");
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_set_token_validator(provider, validateToken, &validator), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, dispatch, methods, token, &fixture), LP_OK);

    ASSERT_EQ(lp_token_save("validator_fixture", "operator-secret"), LP_OK);
    lp_client* client = lp_client_create(
        "validator_fixture", "external_client",
        R"({"protocol":"qt_remote_plain"})",
        R"({"protocol":"qt_remote_plain"})");
    ASSERT_NE(client, nullptr);

    char* result = nullptr;
    char* error = nullptr;
    ASSERT_EQ(lp_invoke(client, "echo", R"(["hello"])", 1000, &result, &error), LP_OK)
        << (error ? error : "");
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(nlohmann::json::parse(result), "plain:hello");
    EXPECT_EQ(validator.transport, "local");

    lp_string_free(result);
    lp_string_free(error);
    lp_client_destroy(client);
    lp_provider_destroy(provider);
}

struct AffinityFixture {
    std::mutex mutex;
    std::set<std::thread::id> threads;
    std::atomic<int> running{0};
    std::atomic<int> peak{0};
};

char* affinityDispatch(const char*, const char*, void* userData)
{
    auto& fixture = *static_cast<AffinityFixture*>(userData);
    const int now = ++fixture.running;
    int seen = fixture.peak.load();
    while (now > seen && !fixture.peak.compare_exchange_weak(seen, now)) {}
    {
        std::lock_guard<std::mutex> lock(fixture.mutex);
        fixture.threads.insert(std::this_thread::get_id());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    --fixture.running;
    return copyString("true");
}

// A host that runs a module single-threaded gets every call on one thread.
TEST(QtRemotePlainCabiTest, AProviderLimitedToOneCallRunsEveryCallOnOneThread)
{
    setInstanceId("qtro_cabi_affinity_");
    AffinityFixture fixture;
    lp_provider* provider = lp_provider_create("affinity_fixture", nullptr);
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_set_max_concurrent_calls(provider, 1), LP_OK);
    ASSERT_EQ(lp_provider_save_token(provider, "caller", "secret"), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, affinityDispatch, methods, token, &fixture), LP_OK);
    ASSERT_EQ(lp_token_save("affinity_fixture", "secret"), LP_OK);

    std::vector<std::thread> callers;
    std::atomic<int> succeeded{0};
    for (int i = 0; i < 8; ++i) {
        callers.emplace_back([&] {
            lp_client* client = lp_client_create("affinity_fixture", "caller", nullptr, nullptr);
            for (int call = 0; call < 5; ++call) {
                char* result = nullptr;
                char* error = nullptr;
                if (lp_invoke(client, "work", "[]", 5000, &result, &error) == LP_OK)
                    ++succeeded;
                lp_string_free(result);
                lp_string_free(error);
            }
            lp_client_destroy(client);
        });
    }
    for (auto& caller : callers) caller.join();
    lp_provider_destroy(provider);

    EXPECT_EQ(succeeded.load(), 40);
    EXPECT_EQ(fixture.peak.load(), 1);
    EXPECT_EQ(fixture.threads.size(), 1u);
}

struct AsyncThreads {
    std::mutex mutex;
    std::set<std::thread::id> callbackThreads;
    std::atomic<int> done{0};
    std::atomic<int> succeeded{0};
};

void onCountedResult(int success, const char*, void* userData)
{
    auto& threads = *static_cast<AsyncThreads*>(userData);
    {
        std::lock_guard<std::mutex> lock(threads.mutex);
        threads.callbackThreads.insert(std::this_thread::get_id());
    }
    if (success) ++threads.succeeded;
    ++threads.done;
}

// Detector: every lp_invoke_async call started a thread of its own.
TEST(QtRemotePlainCabiTest, AsyncCallsShareABoundedSetOfThreads)
{
    setInstanceId("qtro_cabi_async_pool_");
    AffinityFixture fixture;
    lp_provider* provider = lp_provider_create("async_pool_fixture", nullptr);
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_save_token(provider, "caller", "secret"), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, affinityDispatch, methods, token, &fixture), LP_OK);
    ASSERT_EQ(lp_token_save("async_pool_fixture", "secret"), LP_OK);
    lp_client* client = lp_client_create("async_pool_fixture", "caller", nullptr, nullptr);
    ASSERT_NE(client, nullptr);

    AsyncThreads threads;
    constexpr int kCalls = 200;
    for (int i = 0; i < kCalls; ++i)
        ASSERT_EQ(lp_invoke_async(client, "work", "[]", 10000, onCountedResult, &threads), LP_OK);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (threads.done.load() < kCalls && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    lp_client_destroy(client);
    lp_provider_destroy(provider);

    EXPECT_EQ(threads.succeeded.load(), kCalls);
    EXPECT_LE(threads.callbackThreads.size(), 16u);
}

// Detector: lp_invoke failed at once when the provider was not listening yet;
// the Qt C ABI waits for it within the call's timeout.
TEST(QtRemotePlainCabiTest, ACallReachesAProviderThatStartsWithinItsTimeout)
{
    setInstanceId("qtro_cabi_late_provider_");
    Fixture fixture;
    ASSERT_EQ(lp_token_save("late_fixture", "secret"), LP_OK);
    lp_client* client = lp_client_create("late_fixture", "caller", nullptr, nullptr);
    ASSERT_NE(client, nullptr);
    lp_provider* provider = nullptr;
    std::thread starter([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        provider = lp_provider_create("late_fixture", nullptr);
        lp_provider_save_token(provider, "caller", "secret");
        lp_provider_register(provider, dispatch, methods, token, &fixture);
    });

    char* result = nullptr;
    char* error = nullptr;
    const int status = lp_invoke(client, "echo", R"(["late"])", 3000, &result, &error);
    starter.join();
    EXPECT_EQ(status, LP_OK) << (error ? error : "");
    if (result) EXPECT_EQ(nlohmann::json::parse(result), "plain:late");
    lp_string_free(result);
    lp_string_free(error);
    lp_client_destroy(client);
    lp_provider_destroy(provider);
}

struct CapabilityFixture {
    lp_provider* target = nullptr;
    std::atomic<int> requests{0};
};

// capability_module's requestModule: each token issued to a caller replaces
// the previous one at the target.
char* capabilityDispatch(const char* method, const char* argsJson, void* userData)
{
    auto& capability = *static_cast<CapabilityFixture*>(userData);
    if (std::strcmp(method, "requestModule") != 0) return copyString("null");
    const auto args = nlohmann::json::parse(argsJson);
    const std::string issued = "issued-" + std::to_string(++capability.requests);
    lp_provider_save_token(capability.target, args.at(0).get<std::string>().c_str(),
                           issued.c_str());
    return copyString(nlohmann::json(issued).dump());
}

// Detector: every concurrent first call requested its own token, and each
// token revoked the one before it, so most of those calls were refused.
TEST(QtRemotePlainCabiTest, ConcurrentFirstCallsShareOneTokenRequest)
{
    setInstanceId("qtro_cabi_token_flight_");
    Fixture fixture;
    CapabilityFixture capability;
    lp_provider* target = lp_provider_create("flight_target", nullptr);
    ASSERT_NE(target, nullptr);
    ASSERT_EQ(lp_provider_register(target, dispatch, methods, token, &fixture), LP_OK);
    capability.target = target;
    lp_provider* issuer = lp_provider_create("capability_module", nullptr);
    ASSERT_NE(issuer, nullptr);
    ASSERT_EQ(lp_provider_save_token(issuer, "flight_caller", "cap-secret"), LP_OK);
    ASSERT_EQ(lp_provider_register(issuer, capabilityDispatch, methods, token, &capability),
              LP_OK);
    ASSERT_EQ(lp_token_save("capability_module", "cap-secret"), LP_OK);

    constexpr int kCallers = 8;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<int> succeeded{0};
    std::vector<std::thread> callers;
    for (int i = 0; i < kCallers; ++i) {
        callers.emplace_back([&] {
            lp_client* client = lp_client_create("flight_target", "flight_caller",
                                                 nullptr, nullptr);
            ++ready;
            while (!go) std::this_thread::yield();
            char* result = nullptr;
            char* error = nullptr;
            if (lp_invoke(client, "echo", R"(["x"])", 5000, &result, &error) == LP_OK)
                ++succeeded;
            lp_string_free(result);
            lp_string_free(error);
            lp_client_destroy(client);
        });
    }
    while (ready.load() < kCallers) std::this_thread::yield();
    go = true;
    for (auto& caller : callers) caller.join();
    lp_provider_destroy(issuer);
    lp_provider_destroy(target);

    EXPECT_EQ(succeeded.load(), kCallers);
    EXPECT_EQ(capability.requests.load(), 1);
}

struct EventLog {
    std::mutex mutex;
    std::vector<std::string> data;
    std::vector<std::pair<int, std::size_t>> statuses;   // status, events seen by then
};

void onSlowEvent(const char*, const char* data, void* userData)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto& log = *static_cast<EventLog*>(userData);
    std::lock_guard<std::mutex> lock(log.mutex);
    log.data.emplace_back(data);
}

void onLoggedStatus(int status, unsigned long long, const char*, void* userData)
{
    auto& log = *static_cast<EventLog*>(userData);
    std::lock_guard<std::mutex> lock(log.mutex);
    log.statuses.emplace_back(status, log.data.size());
}

bool logged(EventLog& log, int status)
{
    std::lock_guard<std::mutex> lock(log.mutex);
    return std::any_of(log.statuses.begin(), log.statuses.end(),
                       [&](const auto& entry) { return entry.first == status; });
}

lp_provider* startEventProvider(const char* name, Fixture& fixture)
{
    lp_provider* provider = lp_provider_create(name, nullptr);
    if (!provider) return nullptr;
    lp_provider_save_token(provider, "caller", "secret");
    lp_provider_register(provider, dispatch, methods, token, &fixture);
    return provider;
}

// Detector: a lost connection marked the subscription unarmed before the
// events it had already received were delivered, so those were dropped.
TEST(QtRemotePlainCabiTest, EventsReceivedBeforeALossAreDeliveredBeforeIt)
{
    setInstanceId("qtro_cabi_loss_order_");
    Fixture fixture;
    lp_provider* provider = startEventProvider("loss_fixture", fixture);
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_token_save("loss_fixture", "secret"), LP_OK);
    lp_client* client = lp_client_create("loss_fixture", "caller", nullptr, nullptr);
    EventLog log;
    ASSERT_EQ(lp_client_set_subscription_status_cb(client, onLoggedStatus, &log), 1);
    lp_subscription* subscription = lp_subscribe(client, "tick", onSlowEvent, &log);
    ASSERT_NE(subscription, nullptr);
    ASSERT_TRUE(waitUntil([&] { return logged(log, LP_SUB_ARMED); }));

    constexpr int kEvents = 40;
    for (int i = 0; i < kEvents; ++i)
        ASSERT_EQ(lp_provider_emit_event(provider, "tick", ("[" + std::to_string(i) + "]").c_str()),
                  LP_OK);
    // All of them reach the client well before its 10 ms callbacks finish.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    lp_provider_destroy(provider);
    ASSERT_TRUE(waitUntil([&] { return logged(log, LP_SUB_LOST); }));

    {
        std::lock_guard<std::mutex> lock(log.mutex);
        ASSERT_EQ(log.data.size(), static_cast<std::size_t>(kEvents));
        for (int i = 0; i < kEvents; ++i)
            EXPECT_EQ(log.data[i], "[" + std::to_string(i) + "]");
        for (const auto& [status, seen] : log.statuses)
            if (status == LP_SUB_LOST) EXPECT_EQ(seen, static_cast<std::size_t>(kEvents));
    }
    lp_unsubscribe(subscription);
    lp_client_destroy(client);
}

// Detector: arming a subscription taken after the last one was dropped
// advanced the generation, reporting a gap that never happened.
TEST(QtRemotePlainCabiTest, ResubscribingWithoutALossKeepsTheGeneration)
{
    setInstanceId("qtro_cabi_resubscribe_");
    Fixture fixture;
    lp_provider* provider = startEventProvider("resubscribe_fixture", fixture);
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_token_save("resubscribe_fixture", "secret"), LP_OK);
    lp_client* client = lp_client_create("resubscribe_fixture", "caller", nullptr, nullptr);
    EventLog first;
    lp_subscription* subscription = lp_subscribe(client, "tick", onSlowEvent, &first);
    ASSERT_TRUE(waitUntil([&] { return lp_client_subscription_generation(client) == 1; }));
    lp_unsubscribe(subscription);

    EventLog second;
    subscription = lp_subscribe(client, "tick", onSlowEvent, &second);
    ASSERT_TRUE(waitUntil([&] {
        (void)lp_provider_emit_event(provider, "tick", "[1]");
        std::lock_guard<std::mutex> lock(second.mutex);
        return !second.data.empty();
    }));
    EXPECT_EQ(lp_client_subscription_generation(client), 1u);
    lp_unsubscribe(subscription);
    lp_client_destroy(client);
    lp_provider_destroy(provider);
}

// Detector: re-arming backed off to 5 s, so a provider back after a few
// seconds went unnoticed for seconds more.
TEST(QtRemotePlainCabiTest, ARestartedProviderIsReArmedPromptly)
{
    setInstanceId("qtro_cabi_rearm_");
    Fixture fixture;
    lp_provider* provider = startEventProvider("rearm_fixture", fixture);
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_token_save("rearm_fixture", "secret"), LP_OK);
    lp_client* client = lp_client_create("rearm_fixture", "caller", nullptr, nullptr);
    EventLog log;
    lp_subscription* subscription = lp_subscribe(client, "tick", onSlowEvent, &log);
    ASSERT_TRUE(waitUntil([&] { return lp_client_subscription_generation(client) == 1; }));

    lp_provider_destroy(provider);
    // Long enough for the old backoff to be waiting several seconds.
    std::this_thread::sleep_for(std::chrono::milliseconds(5500));
    provider = startEventProvider("rearm_fixture", fixture);
    ASSERT_NE(provider, nullptr);
    const auto restarted = std::chrono::steady_clock::now();
    ASSERT_TRUE(waitUntil([&] { return lp_client_subscription_generation(client) == 2; },
                          std::chrono::seconds(10)));
    EXPECT_LT(std::chrono::steady_clock::now() - restarted, std::chrono::milliseconds(1500));
    lp_unsubscribe(subscription);
    lp_client_destroy(client);
    lp_provider_destroy(provider);
}

// Detector: invalid UTF-8 in a name, or a non-string restart option, threw
// across the C ABI and terminated the process.
TEST(QtRemotePlainCabiTest, MalformedNamesAndOptionsFailWithoutAborting)
{
    setInstanceId("qtro_cabi_malformed_");
    Fixture fixture;
    lp_provider* provider = startEventProvider("malformed_fixture", fixture);
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_token_save("malformed_fixture", "secret"), LP_OK);
    lp_client* client = lp_client_create("malformed_fixture", "caller", nullptr, nullptr);
    ASSERT_NE(client, nullptr);

    char* result = nullptr;
    char* error = nullptr;
    (void)lp_invoke(client, "\xff\xfe", "[]", 2000, &result, &error);
    lp_string_free(result);
    lp_string_free(error);
    EXPECT_EQ(lp_provider_emit_event(provider, "\xff", "[]"), LP_OK);
    lp_subscription* subscription = lp_subscribe(client, "\xff", onSlowEvent, nullptr);
    EXPECT_NE(subscription, nullptr);
    // As the Qt C ABI: only unparseable JSON is refused.
    EXPECT_EQ(lp_client_set_subscription_options(client, R"({"restart":1})"), 1);
    EXPECT_EQ(lp_client_set_subscription_options(client, "[]"), 1);
    EXPECT_EQ(lp_client_set_subscription_options(client, "{"), 0);
    lp_unsubscribe(subscription);
    lp_client_destroy(client);
    lp_provider_destroy(provider);
}

struct ReplayDestroy {
    lp_client* client = nullptr;
    lp_subscription* subscription = nullptr;
    std::atomic<int> destroyed{0};
};

void destroyOnReplay(int status, unsigned long long, const char*, void* userData)
{
    auto& replay = *static_cast<ReplayDestroy*>(userData);
    if (status != LP_SUB_ARMED || !replay.client) return;
    lp_client* client = replay.client;
    replay.client = nullptr;
    lp_unsubscribe(replay.subscription);
    lp_client_destroy(client);
    ++replay.destroyed;
    // The callback's own code keeps running after the destroy.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

// Detector (under a guard allocator): the replay that installing a status
// callback runs used the client after the callback destroyed it.
TEST(QtRemotePlainCabiTest, AStatusReplayCanDestroyTheClient)
{
    setInstanceId("qtro_cabi_replay_destroy_");
    Fixture fixture;
    lp_provider* provider = startEventProvider("replay_fixture", fixture);
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_token_save("replay_fixture", "secret"), LP_OK);
    for (int round = 0; round < 5; ++round) {
        ReplayDestroy replay;
        replay.client = lp_client_create("replay_fixture", "caller", nullptr, nullptr);
        replay.subscription = lp_subscribe(replay.client, "tick", onSlowEvent, nullptr);
        ASSERT_TRUE(waitUntil([&] {
            return lp_client_subscription_generation(replay.client) == 1;
        }));
        ASSERT_EQ(lp_client_set_subscription_status_cb(replay.client, destroyOnReplay, &replay), 1);
        EXPECT_EQ(replay.destroyed.load(), 1);
    }
    lp_provider_destroy(provider);
}

TEST(QtRemotePlainCabiTest, EventCallbackCanSynchronouslyCallTheSameProvider)
{
    setInstanceId("qtro_cabi_reentrant_event_");
    Fixture fixture;
    lp_provider* provider = lp_provider_create("reentrant_fixture", nullptr);
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_save_token(provider, "caller", "secret"), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, dispatch, methods, token, &fixture), LP_OK);
    ASSERT_EQ(lp_token_save("reentrant_fixture", "secret"), LP_OK);

    lp_client* client = lp_client_create("reentrant_fixture", "caller", nullptr, nullptr);
    ASSERT_NE(client, nullptr);
    ReentrantEventResult event;
    event.client = client;
    lp_subscription* subscription = lp_subscribe(client, "tick", onReentrantEvent, &event);
    ASSERT_NE(subscription, nullptr);
    ASSERT_TRUE(waitUntil([&] { return lp_client_subscription_generation(client) == 1; }));
    ASSERT_EQ(lp_provider_emit_event(provider, "tick", "[]"), LP_OK);
    {
        std::unique_lock<std::mutex> lock(event.mutex);
        ASSERT_TRUE(event.changed.wait_for(lock, std::chrono::seconds(2), [&] {
            return event.done;
        }));
    }
    EXPECT_EQ(event.status, LP_OK);
    EXPECT_EQ(nlohmann::json::parse(event.value), "plain:from-event");

    lp_unsubscribe(subscription);
    lp_client_destroy(client);
    lp_provider_destroy(provider);
}

TEST(QtRemotePlainCabiTest, DisconnectDuringCallbackWithStatusAndAutomaticRestartFinishes)
{
    runDisconnectDuringEvent(true, false, false, "dsa");
}

TEST(QtRemotePlainCabiTest, DisconnectDuringCallbackWithStatusAndManualRestartFinishes)
{
    runDisconnectDuringEvent(true, true, false, "dsm");
}

TEST(QtRemotePlainCabiTest, DisconnectDuringCallbackWithoutStatusAndAutomaticRestartFinishes)
{
    runDisconnectDuringEvent(false, false, false, "dna");
}

TEST(QtRemotePlainCabiTest, DisconnectDuringCallbackWithoutStatusAndManualRestartFinishes)
{
    runDisconnectDuringEvent(false, true, false, "dnm");
}

TEST(QtRemotePlainCabiTest, CallbackCanDestroyClientAfterDisconnectWithAutomaticRestart)
{
    runDisconnectDuringEvent(true, false, true, "dda");
}

TEST(QtRemotePlainCabiTest, CallbackCanDestroyClientAfterDisconnectWithManualRestart)
{
    runDisconnectDuringEvent(true, true, true, "ddm");
}

TEST(QtRemotePlainCabiTest, CallbackCanDestroyClientAfterDisconnectWithoutStatusCallback)
{
    runDisconnectDuringEvent(false, false, true, "ddn");
}

TEST(QtRemotePlainCabiTest, AsyncResultCanDestroyClientWithQueuedEvent)
{
    runDestroyWithQueuedCallback(DestroyCallbackKind::AsyncResult,
                                 QueuedWork::Event, "dae");
}

TEST(QtRemotePlainCabiTest, AsyncResultCanDestroyClientWithQueuedDisconnectStatus)
{
    runDestroyWithQueuedCallback(DestroyCallbackKind::AsyncResult,
                                 QueuedWork::Disconnect, "dad");
}

TEST(QtRemotePlainCabiTest, StatusCanDestroyClientWithQueuedEvent)
{
    runDestroyWithQueuedCallback(DestroyCallbackKind::SubscriptionStatus,
                                 QueuedWork::Event, "dse");
}

TEST(QtRemotePlainCabiTest, AsyncResultCanDestroyClientWithoutQueuedWork)
{
    runDestroyWithQueuedCallback(DestroyCallbackKind::AsyncResult,
                                 QueuedWork::None, "dan");
}

TEST(QtRemotePlainCabiTest, StatusCanDestroyClientWithoutQueuedWork)
{
    runDestroyWithQueuedCallback(DestroyCallbackKind::SubscriptionStatus,
                                 QueuedWork::None, "dsn");
}

TEST(QtRemotePlainCabiTest, SlowEventCallbackDoesNotBlockConcurrentReplies)
{
    setInstanceId("qtro_cabi_slow_event_");
    Fixture fixture;
    lp_provider* provider = lp_provider_create("slow_event_fixture", nullptr);
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_save_token(provider, "caller", "secret"), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, dispatch, methods, token, &fixture), LP_OK);
    ASSERT_EQ(lp_token_save("slow_event_fixture", "secret"), LP_OK);

    lp_client* client = lp_client_create("slow_event_fixture", "caller", nullptr, nullptr);
    ASSERT_NE(client, nullptr);
    BlockingEventResult event;
    lp_subscription* subscription = lp_subscribe(client, "tick", onBlockingEvent, &event);
    ASSERT_NE(subscription, nullptr);
    ASSERT_TRUE(waitUntil([&] { return lp_client_subscription_generation(client) == 1; }));
    ASSERT_EQ(lp_provider_emit_event(provider, "tick", "[]"), LP_OK);
    {
        std::unique_lock<std::mutex> lock(event.mutex);
        ASSERT_TRUE(event.changed.wait_for(lock, std::chrono::seconds(1), [&] {
            return event.entered;
        }));
    }

    char* value = nullptr;
    char* error = nullptr;
    EXPECT_EQ(lp_invoke(client, "echo", R"(["while-blocked"])", 1000, &value, &error),
              LP_OK) << (error ? error : "");
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(nlohmann::json::parse(value), "plain:while-blocked");
    lp_string_free(value);
    lp_string_free(error);

    {
        std::lock_guard<std::mutex> lock(event.mutex);
        event.release = true;
    }
    event.changed.notify_all();
    lp_unsubscribe(subscription);
    lp_client_destroy(client);
    lp_provider_destroy(provider);
}

TEST(QtRemotePlainCabiTest, DeferredCompletionBypassesBlockedPublicCallback)
{
    setInstanceId("qtro_cabi_deferred_event_");
    DeferredFixture fixture;
    fixture.provider = lp_provider_create("deferred_event_fixture", nullptr);
    ASSERT_NE(fixture.provider, nullptr);
    ASSERT_EQ(lp_provider_save_token(fixture.provider, "caller", "secret"), LP_OK);
    ASSERT_EQ(lp_provider_register(fixture.provider, deferredDispatch, deferredMethods,
                                   token, &fixture), LP_OK);
    ASSERT_EQ(lp_token_save("deferred_event_fixture", "secret"), LP_OK);

    lp_client* client = lp_client_create("deferred_event_fixture", "caller", nullptr, nullptr);
    ASSERT_NE(client, nullptr);
    ReentrantEventResult event;
    event.client = client;
    auto invokeDeferred = [](const char*, const char*, void* userData) {
        auto* result = static_cast<ReentrantEventResult*>(userData);
        char* value = nullptr;
        char* error = nullptr;
        const int status = lp_invoke(result->client, "deferred", "[]", 1000,
                                     &value, &error);
        {
            std::lock_guard<std::mutex> lock(result->mutex);
            result->status = status;
            result->value = value ? value : "";
            result->done = true;
        }
        lp_string_free(value);
        lp_string_free(error);
        result->changed.notify_all();
    };
    lp_subscription* subscription = lp_subscribe(client, "tick", invokeDeferred, &event);
    ASSERT_NE(subscription, nullptr);
    ASSERT_TRUE(waitUntil([&] { return lp_client_subscription_generation(client) == 1; }));
    ASSERT_EQ(lp_provider_emit_event(fixture.provider, "tick", "[]"), LP_OK);
    {
        std::unique_lock<std::mutex> lock(event.mutex);
        ASSERT_TRUE(event.changed.wait_for(lock, std::chrono::seconds(2), [&] {
            return event.done;
        }));
    }
    EXPECT_EQ(event.status, LP_OK);
    EXPECT_EQ(nlohmann::json::parse(event.value), "complete");

    lp_unsubscribe(subscription);
    lp_client_destroy(client);
    lp_provider_destroy(fixture.provider);
}

TEST(QtRemotePlainCabiTest, EventCallbackCanDestroyItsClient)
{
    setInstanceId("qtro_cabi_callback_destroy_");
    Fixture fixture;
    lp_provider* provider = lp_provider_create("destroy_fixture", nullptr);
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_save_token(provider, "caller", "secret"), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, dispatch, methods, token, &fixture), LP_OK);
    ASSERT_EQ(lp_token_save("destroy_fixture", "secret"), LP_OK);

    DestroyEventResult event;
    event.client = lp_client_create("destroy_fixture", "caller", nullptr, nullptr);
    ASSERT_NE(event.client, nullptr);
    lp_subscription* subscription = lp_subscribe(
        event.client, "tick", onDestroyingEvent, &event);
    ASSERT_NE(subscription, nullptr);
    ASSERT_TRUE(waitUntil([&] {
        return lp_client_subscription_generation(event.client) == 1;
    }));
    ASSERT_EQ(lp_provider_emit_event(provider, "tick", "[]"), LP_OK);
    {
        std::unique_lock<std::mutex> lock(event.mutex);
        ASSERT_TRUE(event.changed.wait_for(lock, std::chrono::seconds(2), [&] {
            return event.done;
        }));
    }

    lp_unsubscribe(subscription);
    lp_provider_destroy(provider);
}

TEST(QtRemotePlainCabiTest, ProviderDestroyWaitsForOutstandingDispatch)
{
    // Keep the socket name short enough for macOS sun_path even when Nix sets
    // TMPDIR to a long per-build directory.
    setInstanceId("qtro_drain_");
    BlockingDispatchFixture fixture;
    lp_provider* provider = lp_provider_create("provider_destroy_fixture", nullptr);
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_save_token(provider, "caller", "secret"), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, blockingDispatch, blockingMethods,
                                   nullptr, &fixture), LP_OK);
    ASSERT_EQ(lp_token_save("provider_destroy_fixture", "secret"), LP_OK);
    lp_client* client = lp_client_create(
        "provider_destroy_fixture", "caller", nullptr, nullptr);
    ASSERT_NE(client, nullptr);

    std::thread call([&] {
        char* value = nullptr;
        char* error = nullptr;
        (void)lp_invoke(client, "block", "[]", 2000, &value, &error);
        lp_string_free(value);
        lp_string_free(error);
    });
    {
        std::unique_lock<std::mutex> lock(fixture.mutex);
        ASSERT_TRUE(fixture.changed.wait_for(lock, std::chrono::seconds(1), [&] {
            return fixture.entered;
        }));
    }

    std::atomic<bool> destroyed{false};
    std::thread destroyer([&] {
        lp_provider_destroy(provider);
        destroyed = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(destroyed.load());
    {
        std::lock_guard<std::mutex> lock(fixture.mutex);
        fixture.release = true;
    }
    fixture.changed.notify_all();
    destroyer.join();
    EXPECT_TRUE(destroyed.load());
    call.join();
    lp_client_destroy(client);
}

TEST(QtRemotePlainCabiTest, ProviderPublishesBusinessObjectOnlyAfterActivation)
{
    setInstanceId("qtro_cabi_staged_provider_");
    Fixture fixture;
    lp_provider* provider = lp_provider_create("staged_fixture", nullptr);
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_prepare(provider, dispatch, methods, token, &fixture), LP_OK);

    logos::qt_remote_plain::Client wire;
    std::string error;
    ASSERT_TRUE(wire.connect("local:logos_staged_fixture_" +
        std::string(std::getenv("LOGOS_INSTANCE_ID")), std::chrono::seconds(1), &error)) << error;
    EXPECT_TRUE(wire.acquire("staged_fixture__handshake", std::chrono::seconds(1), &error))
        << error;
    error.clear();
    EXPECT_FALSE(wire.acquire("staged_fixture", std::chrono::milliseconds(100), &error));

    ASSERT_EQ(lp_provider_register(provider, dispatch, methods, token, &fixture), LP_OK);
    error.clear();
    EXPECT_TRUE(wire.acquire("staged_fixture", std::chrono::seconds(1), &error)) << error;

    wire.close();
    lp_provider_destroy(provider);
}

TEST(QtRemotePlainCabiTest, DeclaredReturnTypeDisambiguatesMapAndLogosResult)
{
    setInstanceId("qtro_cabi_result_contract_");
    Fixture fixture;
    lp_provider* provider = lp_provider_create("result_fixture", nullptr);
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_save_token(provider, "caller", "secret"), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, dispatch, methods, token, &fixture), LP_OK);

    logos::qt_remote_plain::Client wire;
    std::string error;
    ASSERT_TRUE(wire.connect("local:logos_result_fixture_" +
        std::string(std::getenv("LOGOS_INSTANCE_ID")), std::chrono::seconds(1), &error)) << error;
    auto call = [&](const char* method) {
        return wire.call("result_fixture", "callRemoteMethod(QString,QString,QVariantList)",
            {logos::qt_remote_plain::Variant::fromRpc(logos::plain::RpcValue{"secret"}),
             logos::qt_remote_plain::Variant::fromRpc(logos::plain::RpcValue{method}),
             logos::qt_remote_plain::Variant::fromRpc(logos::plain::RpcValue{logos::plain::RpcList{}})},
            std::chrono::seconds(1), &error);
    };
    const auto map = call("mapCollision");
    ASSERT_TRUE(map.has_value()) << error;
    EXPECT_EQ(map->type, logos::qt_remote_plain::MetaType::VariantMap);
    const auto result = call("resultCollision");
    ASSERT_TRUE(result.has_value()) << error;
    EXPECT_EQ(result->type, logos::qt_remote_plain::MetaType::User);
    EXPECT_EQ(result->customType, "LogosResult");
    // Detector: the C++ generator publishes the LIDL spelling, "result".
    const auto lidl = call("lidlResult");
    ASSERT_TRUE(lidl.has_value()) << error;
    EXPECT_EQ(lidl->type, logos::qt_remote_plain::MetaType::User);
    EXPECT_EQ(lidl->customType, "LogosResult");

    wire.close();
    lp_provider_destroy(provider);
}

TEST(QtRemotePlainCabiTest, DeferredSubscriptionReconnectsAndManualPolicyHolds)
{
    setInstanceId("qtro_cabi_restart_");
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
