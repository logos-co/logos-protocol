// plain_local: a provider serving "inproc" reached from the same image without a
// socket, with the socket transports' call, event and token semantics.
#include "logos_protocol.h"

#include <gtest/gtest.h>
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

using json = nlohmann::json;

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

bool waitUntil(const std::function<bool()>& predicate,
               std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

struct Module {
    std::mutex mutex;
    std::string tokenModule;
    std::string tokenValue;
    std::atomic<int> running{0};
    std::atomic<int> peak{0};
    lp_provider* self = nullptr;
};

char* dispatch(const char* method, const char* argsJson, void* userData)
{
    auto& module = *static_cast<Module*>(userData);
    const json args = json::parse(argsJson);
    if (std::strcmp(method, "echo") == 0)
        return copyText(json("inproc:" + args.at(0).get<std::string>()).dump());
    if (std::strcmp(method, "whoami") == 0)
        return copyText(json(json::parse(lp_current_caller_json())).dump());
    if (std::strcmp(method, "deferred") == 0) {
        std::thread([&module] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            lp_provider_emit_event(module.self, "__logos_call_complete__",
                                   R"(["call-1",{"answer":42}])");
        }).detach();
        return copyText(R"({"__logos_pending_call__":"call-1"})");
    }
    if (std::strcmp(method, "work") == 0) {
        const int now = ++module.running;
        int seen = module.peak.load();
        while (now > seen && !module.peak.compare_exchange_weak(seen, now)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        --module.running;
        return copyText("true");
    }
    return copyText("null");
}

char* methods(void*)
{
    return copyText(R"json([{"type":"method","name":"echo","signature":"echo(string)","returnType":"string","isInvokable":true,"parameters":[]}])json");
}

int onToken(const char* moduleName, const char* token, void* userData)
{
    auto& module = *static_cast<Module*>(userData);
    std::lock_guard<std::mutex> lock(module.mutex);
    module.tokenModule = moduleName;
    module.tokenValue = token;
    return LP_OK;
}

lp_provider* startProvider(const char* name, const char* transports, Module& module)
{
    lp_provider* provider = lp_provider_create(name, transports);
    if (!provider) return nullptr;
    module.self = provider;
    lp_provider_save_token(provider, "caller", "secret");
    if (lp_provider_register(provider, dispatch, methods, onToken, &module) != LP_OK) {
        lp_provider_destroy(provider);
        return nullptr;
    }
    return provider;
}

struct Invocation {
    int status = LP_ERR_INTERNAL;
    json result;
    json error;
};

Invocation invoke(lp_client* client, const char* method, const char* args, int timeout = 2000)
{
    char* result = nullptr;
    char* error = nullptr;
    Invocation out;
    out.status = lp_invoke(client, method, args, timeout, &result, &error);
    if (result) out.result = json::parse(result);
    if (error) out.error = json::parse(error);
    lp_string_free(result);
    lp_string_free(error);
    return out;
}

struct Events {
    std::mutex mutex;
    std::vector<std::string> names;
    std::vector<std::string> data;
    std::vector<int> statuses;
    std::vector<unsigned long long> generations;
};

void onEvent(const char* name, const char* data, void* userData)
{
    auto& events = *static_cast<Events*>(userData);
    std::lock_guard<std::mutex> lock(events.mutex);
    events.names.push_back(name);
    events.data.push_back(data);
}

void onStatus(int state, unsigned long long generation, const char*, void* userData)
{
    auto& events = *static_cast<Events*>(userData);
    std::lock_guard<std::mutex> lock(events.mutex);
    events.statuses.push_back(state);
    events.generations.push_back(generation);
}

// No socket is served: every step below can only have gone through inproc.
TEST(PlainLocalInproc, CallsEventsAndIntrospectionNeedNoSocket)
{
    useInstance("inproc_basic_");
    Module module;
    lp_provider* provider = startProvider("inproc_basic", R"([{"protocol":"inproc"}])", module);
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_token_save("inproc_basic", "secret"), LP_OK);
    lp_client* client = lp_client_create("inproc_basic", "caller", nullptr, nullptr);
    ASSERT_NE(client, nullptr);

    const Invocation echo = invoke(client, "echo", R"(["hi"])");
    ASSERT_EQ(echo.status, LP_OK) << echo.error.dump();
    EXPECT_EQ(echo.result, "inproc:hi");

    char* interface = lp_get_methods(client);
    ASSERT_NE(interface, nullptr);
    EXPECT_EQ(json::parse(interface).at(0).at("name"), "echo");
    lp_string_free(interface);

    Events events;
    lp_subscription* subscription = lp_subscribe(client, "", onEvent, &events);
    ASSERT_NE(subscription, nullptr);
    ASSERT_TRUE(waitUntil([&] { return lp_client_subscription_generation(client) == 1; }));
    ASSERT_EQ(lp_provider_emit_event(provider, "tick", R"(["payload",7])"), LP_OK);
    ASSERT_TRUE(waitUntil([&] {
        std::lock_guard<std::mutex> lock(events.mutex);
        return !events.names.empty();
    }));
    {
        std::lock_guard<std::mutex> lock(events.mutex);
        EXPECT_EQ(events.names.front(), "tick");
        EXPECT_EQ(json::parse(events.data.front()), json::array({"payload", 7}));
    }

    lp_unsubscribe(subscription);
    lp_client_destroy(client);
    lp_provider_destroy(provider);
}

TEST(PlainLocalInproc, TheCallerIsTheConnectionsPrincipal)
{
    useInstance("inproc_caller_");
    Module module;
    lp_provider* provider = startProvider("inproc_caller", R"([{"protocol":"inproc"}])", module);
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_token_save("inproc_caller", "secret"), LP_OK);
    lp_client* client = lp_client_create("inproc_caller", "caller", nullptr, nullptr);
    const Invocation who = invoke(client, "whoami", "[]");
    ASSERT_EQ(who.status, LP_OK) << who.error.dump();
    EXPECT_EQ(who.result, json({{"kind", "module"}, {"name", "caller"}}));
    lp_client_destroy(client);

    // The engine's connection needs no token: the binding is its identity.
    lp_client* engine = lp_client_create("inproc_caller", "@runtime", nullptr, nullptr);
    const Invocation host = invoke(engine, "whoami", "[]");
    ASSERT_EQ(host.status, LP_OK) << host.error.dump();
    EXPECT_EQ(host.result, json({{"kind", "host"}}));
    lp_client_destroy(engine);
    lp_provider_destroy(provider);
}

// A token names its caller; on a bound connection it can name only that one.
TEST(PlainLocalInproc, ATokenNamingAnotherCallerIsRefusedWithoutReexchange)
{
    useInstance("inproc_binding_");
    Module module;
    lp_provider* provider = startProvider("inproc_binding", R"([{"protocol":"inproc"}])", module);
    ASSERT_NE(provider, nullptr);
    // "secret" is caller's; impostor presents it.
    ASSERT_EQ(lp_token_save("inproc_binding", "secret"), LP_OK);
    lp_client* impostor = lp_client_create("inproc_binding", "impostor", nullptr, nullptr);
    const auto started = std::chrono::steady_clock::now();
    const Invocation refused = invoke(impostor, "whoami", "[]", 3000);
    const auto took = std::chrono::steady_clock::now() - started;
    EXPECT_NE(refused.status, LP_OK);
    EXPECT_EQ(refused.error.value("code", ""), "unauthorized");
    // Refused at once: no capability exchange was attempted for a new token.
    EXPECT_LT(took, std::chrono::milliseconds(500));
    lp_client_destroy(impostor);
    lp_provider_destroy(provider);
}

TEST(PlainLocalInproc, AnExplicitInprocTargetNeverFallsBackToTheSocket)
{
    useInstance("inproc_explicit_");
    Module module;
    lp_provider* provider = startProvider("inproc_explicit",
                                          R"([{"protocol":"qt_remote_plain"}])", module);
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_token_save("inproc_explicit", "secret"), LP_OK);
    lp_client* client = lp_client_create("inproc_explicit", "caller",
                                         R"({"protocol":"inproc"})", nullptr);
    ASSERT_NE(client, nullptr);
    const Invocation missing = invoke(client, "echo", R"(["x"])", 1000);
    EXPECT_NE(missing.status, LP_OK);
    EXPECT_EQ(missing.error.value("code", ""), "object_unavailable");
    lp_client_destroy(client);

    // The socket still serves a client that asked for it.
    lp_client* socket = lp_client_create("inproc_explicit", "caller", nullptr, nullptr);
    EXPECT_EQ(invoke(socket, "echo", R"(["x"])").status, LP_OK);
    lp_client_destroy(socket);
    lp_provider_destroy(provider);
}

// A provider serving both is reached in-process by a default client: only the
// inproc binding makes an untokened engine connection the host.
TEST(PlainLocalInproc, ADefaultClientPrefersAnInProcessProvider)
{
    useInstance("inproc_prefer_");
    Module module;
    lp_provider* provider = startProvider(
        "inproc_prefer", R"([{"protocol":"qt_remote_plain"},{"protocol":"inproc"}])", module);
    ASSERT_NE(provider, nullptr);
    lp_client* engine = lp_client_create("inproc_prefer", "@runtime",
                                         R"({"protocol":"qt_remote_plain"})", nullptr);
    const Invocation host = invoke(engine, "whoami", "[]");
    ASSERT_EQ(host.status, LP_OK) << host.error.dump();
    EXPECT_EQ(host.result, json({{"kind", "host"}}));
    lp_client_destroy(engine);
    lp_provider_destroy(provider);
}

TEST(PlainLocalInproc, AWithdrawnProviderIsALossAndItsReturnReArms)
{
    useInstance("inproc_restart_");
    Module first;
    lp_provider* provider = startProvider("inproc_restart", R"([{"protocol":"inproc"}])", first);
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_token_save("inproc_restart", "secret"), LP_OK);
    lp_client* client = lp_client_create("inproc_restart", "caller", nullptr, nullptr);
    Events events;
    ASSERT_EQ(lp_client_set_subscription_status_cb(client, onStatus, &events), 1);
    lp_subscription* subscription = lp_subscribe(client, "tick", onEvent, &events);
    ASSERT_TRUE(waitUntil([&] { return lp_client_subscription_generation(client) == 1; }));

    lp_provider_destroy(provider);
    ASSERT_TRUE(waitUntil([&] {
        std::lock_guard<std::mutex> lock(events.mutex);
        return std::find(events.statuses.begin(), events.statuses.end(), LP_SUB_LOST)
            != events.statuses.end();
    }));

    Module second;
    provider = startProvider("inproc_restart", R"([{"protocol":"inproc"}])", second);
    ASSERT_NE(provider, nullptr);
    ASSERT_TRUE(waitUntil([&] { return lp_client_subscription_generation(client) == 2; }));
    ASSERT_EQ(lp_provider_emit_event(provider, "tick", R"([1])"), LP_OK);
    ASSERT_TRUE(waitUntil([&] {
        std::lock_guard<std::mutex> lock(events.mutex);
        return !events.names.empty();
    }));
    EXPECT_EQ(invoke(client, "echo", R"(["back"])").result, "inproc:back");

    lp_unsubscribe(subscription);
    lp_client_destroy(client);
    lp_provider_destroy(provider);
}

TEST(PlainLocalInproc, TokenPushesNeedCapabilityOrTheEngine)
{
    useInstance("inproc_push_");
    Module module;
    lp_provider* provider = startProvider("inproc_push", R"([{"protocol":"inproc"}])", module);
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_grant_host_services(R"(["token_delivery"])"), LP_OK);

    lp_client* stranger = lp_client_create("inproc_push", "stranger", nullptr, nullptr);
    EXPECT_NE(lp_inform_module_token_to(stranger, "", "inproc_push", "peer", "forged", 1000),
              LP_OK);
    lp_client_destroy(stranger);

    lp_client* capability = lp_client_create("inproc_push", "capability_module", nullptr, nullptr);
    EXPECT_EQ(lp_inform_module_token_to(capability, "", "inproc_push", "peer", "peer-token",
                                        1000),
              LP_OK);
    lp_client_destroy(capability);
    ASSERT_EQ(lp_grant_host_services("[]"), LP_OK);
    {
        std::lock_guard<std::mutex> lock(module.mutex);
        EXPECT_EQ(module.tokenModule, "peer");
        EXPECT_EQ(module.tokenValue, "peer-token");
    }

    // The pushed token now names its holder on that holder's connection.
    ASSERT_EQ(lp_token_save_for("peer", "inproc_push", "peer-token"), LP_OK);
    lp_client* peer = lp_client_create("inproc_push", "peer", nullptr, nullptr);
    const Invocation who = invoke(peer, "whoami", "[]");
    EXPECT_EQ(who.result, json({{"kind", "module"}, {"name", "peer"}}));
    lp_client_destroy(peer);
    lp_provider_destroy(provider);
}

struct AsyncResult {
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    int ok = 0;
    std::string json;
};

void onResult(int ok, const char* text, void* userData)
{
    auto& result = *static_cast<AsyncResult*>(userData);
    std::lock_guard<std::mutex> lock(result.mutex);
    result.done = true;
    result.ok = ok;
    result.json = text;
    result.changed.notify_all();
}

TEST(PlainLocalInproc, AsyncCallsAndDeferredCompletionsArrive)
{
    useInstance("inproc_async_");
    Module module;
    lp_provider* provider = startProvider("inproc_async", R"([{"protocol":"inproc"}])", module);
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_token_save("inproc_async", "secret"), LP_OK);
    lp_client* client = lp_client_create("inproc_async", "caller", nullptr, nullptr);

    AsyncResult result;
    ASSERT_EQ(lp_invoke_async(client, "echo", R"(["later"])", 2000, onResult, &result), LP_OK);
    {
        std::unique_lock<std::mutex> lock(result.mutex);
        ASSERT_TRUE(result.changed.wait_for(lock, std::chrono::seconds(3),
                                            [&] { return result.done; }));
        EXPECT_EQ(result.ok, 1);
        EXPECT_EQ(json::parse(result.json), "inproc:later");
    }

    const Invocation deferred = invoke(client, "deferred", "[]", 3000);
    ASSERT_EQ(deferred.status, LP_OK) << deferred.error.dump();
    EXPECT_EQ(deferred.result, json({{"answer", 42}}));

    lp_client_destroy(client);
    lp_provider_destroy(provider);
}

TEST(PlainLocalInproc, OneGateServesTheSocketAndInproc)
{
    useInstance("inproc_gate_");
    Module module;
    lp_provider* provider = startProvider(
        "inproc_gate", R"([{"protocol":"qt_remote_plain"},{"protocol":"inproc"}])", module);
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_set_max_concurrent_calls(provider, 1), LP_OK);
    ASSERT_EQ(lp_token_save("inproc_gate", "secret"), LP_OK);

    std::atomic<int> succeeded{0};
    std::vector<std::thread> callers;
    for (int i = 0; i < 4; ++i) {
        for (const char* transport : {R"({"protocol":"inproc"})", R"({"protocol":"qt_remote_plain"})"}) {
            callers.emplace_back([&, transport] {
                lp_client* client = lp_client_create("inproc_gate", "caller", transport, nullptr);
                for (int call = 0; call < 5; ++call)
                    if (invoke(client, "work", "[]", 5000).status == LP_OK) ++succeeded;
                lp_client_destroy(client);
            });
        }
    }
    for (auto& caller : callers) caller.join();
    lp_provider_destroy(provider);
    EXPECT_EQ(succeeded.load(), 40);
    EXPECT_EQ(module.peak.load(), 1);
}

TEST(PlainLocalInproc, ASecondProviderCannotTakeAPublishedName)
{
    useInstance("inproc_taken_");
    Module first;
    Module second;
    lp_provider* provider = startProvider("inproc_taken", R"([{"protocol":"inproc"}])", first);
    ASSERT_NE(provider, nullptr);
    EXPECT_EQ(startProvider("inproc_taken", R"([{"protocol":"inproc"}])", second), nullptr);
    lp_provider_destroy(provider);
    lp_provider* again = startProvider("inproc_taken", R"([{"protocol":"inproc"}])", second);
    EXPECT_NE(again, nullptr);
    lp_provider_destroy(again);
}

} // namespace
