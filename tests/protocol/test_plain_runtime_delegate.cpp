// A module image with a runtime delegate runs its client calls in the host's
// runtime as its admitted identity. This executable is that image AND its host:
// installing a delegate is process-wide and one-shot, so it has one test.
#include "logos_protocol.h"
#include "logos_runtime_delegate.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

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

char* whoami(const char*, const char*, void*)
{
    return copyText(json(json::parse(lp_current_caller_json())).dump());
}

char* noMethods(void*) { return copyText("[]"); }

int acceptToken(const char*, const char*, void*) { return LP_OK; }

struct Events {
    std::mutex mutex;
    int count = 0;
};

void onEvent(const char*, const char*, void* userData)
{
    auto& events = *static_cast<Events*>(userData);
    std::lock_guard<std::mutex> lock(events.mutex);
    ++events.count;
}

bool waitUntil(const std::function<bool()>& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

TEST(PlainRuntimeDelegate, AModuleImageCallsAsItsIdentityUntilReleased)
{
#ifdef _WIN32
    ASSERT_EQ(::_putenv_s("LOGOS_INSTANCE_ID", ("delegate_" + std::to_string(::_getpid())).c_str()), 0);
#else
    ASSERT_EQ(::setenv("LOGOS_INSTANCE_ID", ("delegate_" + std::to_string(::getpid())).c_str(), 1), 0);
#endif
    // Host side: a provider served in-process, and the identity it admits.
    lp_provider* provider = lp_provider_create("delegate_target", R"([{"protocol":"inproc"}])");
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_save_token(provider, "mod_a", "tok-a"), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, whoami, noMethods, acceptToken, nullptr), LP_OK);

    // Refused: not isolated, the engine's names, and a name outside the namespace.
    EXPECT_EQ(lp_runtime_delegate_create("mod_a", "[]"), nullptr);
    EXPECT_EQ(lp_runtime_delegate_create("core", "[]"), nullptr);
    EXPECT_EQ(lp_runtime_delegate_create("@runtime", "[]"), nullptr);
    ASSERT_EQ(lp_token_isolate_identity("mod_a"), LP_OK);
    EXPECT_EQ(lp_runtime_delegate_create("mod_a", "[]"), nullptr); // no credential yet
    ASSERT_EQ(lp_token_adopt_credential("mod_a", "cred-a"), LP_OK);
    ASSERT_EQ(lp_token_save_for("mod_a", "delegate_target", "tok-a"), LP_OK);
    EXPECT_EQ(lp_runtime_delegate_create("mod_a", R"(["no_such_service"])"), nullptr);
    const lp_runtime_delegate_v1* delegate = lp_runtime_delegate_create("mod_a", "[]");
    ASSERT_NE(delegate, nullptr);
    EXPECT_EQ(delegate->version, static_cast<unsigned>(LP_RUNTIME_DELEGATE_VERSION));

    // Image side: install once, before any client of the image's own.
    ASSERT_EQ(lp_runtime_install_delegate(delegate), LP_OK);
    EXPECT_EQ(lp_runtime_install_delegate(delegate), LP_ERR_UNSUPPORTED);

    // The origin passed is ignored: the call is mod_a's.
    lp_client* client = lp_client_create("delegate_target", "someone_else", nullptr, nullptr);
    ASSERT_NE(client, nullptr);
    char* result = nullptr;
    char* error = nullptr;
    ASSERT_EQ(lp_invoke(client, "whoami", "[]", 2000, &result, &error), LP_OK)
        << (error ? error : "");
    EXPECT_EQ(json::parse(result), json({{"kind", "module"}, {"name", "mod_a"}}));
    lp_string_free(result);
    lp_string_free(error);

    Events events;
    lp_subscription* subscription = lp_subscribe(client, "tick", onEvent, &events);
    ASSERT_NE(subscription, nullptr);
    ASSERT_TRUE(waitUntil([&] { return lp_client_subscription_generation(client) == 1; }));
    ASSERT_EQ(lp_provider_emit_event(provider, "tick", "[1]"), LP_OK);
    ASSERT_TRUE(waitUntil([&] {
        std::lock_guard<std::mutex> lock(events.mutex);
        return events.count == 1;
    }));

    // The image's grant is its own: an ungranted identity cannot push even
    // though this process holds token_delivery.
    ASSERT_EQ(lp_grant_host_services(R"(["token_delivery"])"), LP_OK);
    EXPECT_EQ(lp_inform_module_token_to(client, "", "delegate_target", "peer", "t", 1000),
              LP_ERR_UNSUPPORTED);

    // Released: every handle is dead and nothing new is created.
    lp_runtime_delegate_release(delegate);
    result = nullptr;
    error = nullptr;
    EXPECT_EQ(lp_invoke(client, "whoami", "[]", 1000, &result, &error), LP_ERR_UNAVAILABLE);
    lp_string_free(result);
    lp_string_free(error);
    EXPECT_EQ(lp_client_create("delegate_target", "x", nullptr, nullptr), nullptr);
    lp_unsubscribe(subscription);
    lp_client_destroy(client);
    lp_provider_destroy(provider);
}

} // namespace
