// The deployment a runtime delegate exists for: a host on the shared runtime
// loads a module image that carries its own, hidden, static runtime.
#include "logos_protocol.h"
#include "logos_runtime_delegate.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace {

using json = nlohmann::json;
using CallFn = int (*)(const char*, const char*, char*, int);
using SubscribeFn = int (*)(const char*, const char*);
using GenerationFn = unsigned long long (*)();
using CountFn = int (*)();
using CloseFn = void (*)();

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

void* loadImage()
{
#ifdef _WIN32
    // Next to this executable: the Windows test bundle ships both in one directory.
    wchar_t exe[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe, length);
    path.resize(path.find_last_of(L"\\/") + 1);
    for (const char* c = DELEGATE_IMAGE_NAME; *c; ++c) path.push_back(static_cast<wchar_t>(*c));
    return reinterpret_cast<void*>(LoadLibraryW(path.c_str()));
#else
    return dlopen(DELEGATE_IMAGE_PATH, RTLD_NOW | RTLD_LOCAL);
#endif
}

void* symbol(void* image, const char* name)
{
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(image), name));
#else
    return dlsym(image, name);
#endif
}

template <typename Fn>
Fn function(void* image, const char* name)
{
    return reinterpret_cast<Fn>(symbol(image, name));
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

TEST(PlainRuntimeDelegateImage, AnImageWithItsOwnRuntimeCallsAsItsIdentity)
{
#ifdef _WIN32
    ASSERT_EQ(::_putenv_s("LOGOS_INSTANCE_ID", ("image_" + std::to_string(::_getpid())).c_str()), 0);
#else
    ASSERT_EQ(::setenv("LOGOS_INSTANCE_ID", ("image_" + std::to_string(::getpid())).c_str(), 1), 0);
#endif
    lp_provider* provider = lp_provider_create("image_target", R"([{"protocol":"inproc"}])");
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_save_token(provider, "mod_img", "tok-img"), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, whoami, noMethods, acceptToken, nullptr), LP_OK);
    ASSERT_EQ(lp_token_isolate_identity("mod_img"), LP_OK);
    ASSERT_EQ(lp_token_adopt_credential("mod_img", "cred-img"), LP_OK);
    ASSERT_EQ(lp_token_save_for("mod_img", "image_target", "tok-img"), LP_OK);
    const lp_runtime_delegate_v1* delegate = lp_runtime_delegate_create("mod_img", "[]");
    ASSERT_NE(delegate, nullptr);
    // The host's runtime never takes a delegate itself.
    EXPECT_EQ(lp_runtime_install_delegate(delegate), LP_ERR_UNSUPPORTED);

    void* image = loadImage();
    ASSERT_NE(image, nullptr);
    // The image's runtime is hidden, so its lp_* calls cannot bind to the host's.
    EXPECT_EQ(symbol(image, "lp_client_create"), nullptr);
    auto install = function<logos_module_set_runtime_delegate_fn>(
        image, LOGOS_MODULE_SET_RUNTIME_DELEGATE_SYMBOL);
    auto call = function<CallFn>(image, "delegate_image_call");
    auto subscribe = function<SubscribeFn>(image, "delegate_image_subscribe");
    auto generation = function<GenerationFn>(image, "delegate_image_generation");
    auto events = function<CountFn>(image, "delegate_image_events");
    auto close = function<CloseFn>(image, "delegate_image_close");
    ASSERT_TRUE(install && call && subscribe && generation && events && close);
    ASSERT_EQ(install(delegate), LP_OK);

    char out[512] = {};
    ASSERT_EQ(call("image_target", "whoami", out, sizeof out), LP_OK) << out;
    EXPECT_EQ(json::parse(out), json({{"kind", "module"}, {"name", "mod_img"}}));

    // The image's callback runs from the host's delivery thread.
    ASSERT_EQ(subscribe("image_target", "tick"), LP_OK);
    ASSERT_TRUE(waitUntil([&] { return generation() == 1; }));
    ASSERT_EQ(lp_provider_emit_event(provider, "tick", "[1]"), LP_OK);
    EXPECT_TRUE(waitUntil([&] { return events() == 1; }));

    lp_runtime_delegate_release(delegate);
    EXPECT_NE(call("image_target", "whoami", out, sizeof out), LP_OK);
    close();
    lp_provider_destroy(provider);
    // The image stays mapped, as a runtime host keeps it.
}

} // namespace
