// A module image for plain_runtime_delegate_image_tests: it carries its own
// static plain runtime, exports only the functions below, and calls through lp_*.
#include "logos_protocol.h"
#include "logos_runtime_delegate.h"

#include <atomic>
#include <cstdio>

#if defined(_WIN32)
#define IMAGE_EXPORT __declspec(dllexport)
#else
#define IMAGE_EXPORT __attribute__((visibility("default")))
#endif

namespace {

std::atomic<int> gEvents{0};
lp_client* gSubscriber = nullptr;
lp_subscription* gSubscription = nullptr;

void onEvent(const char*, const char*, void*) { gEvents.fetch_add(1); }

} // namespace

extern "C" {

IMAGE_EXPORT int logos_module_set_runtime_delegate(const lp_runtime_delegate_v1* delegate)
{
    return lp_runtime_install_delegate(delegate);
}

// Calls `method` on `target` claiming another origin; the result or error goes to `out`.
IMAGE_EXPORT int delegate_image_call(const char* target, const char* method, char* out,
                                     int capacity)
{
    lp_client* client = lp_client_create(target, "impostor", nullptr, nullptr);
    if (!client) return LP_ERR_UNAVAILABLE;
    char* result = nullptr;
    char* error = nullptr;
    const int status = lp_invoke(client, method, "[]", 2000, &result, &error);
    std::snprintf(out, static_cast<size_t>(capacity), "%s",
                  result ? result : (error ? error : ""));
    lp_string_free(result);
    lp_string_free(error);
    lp_client_destroy(client);
    return status;
}

IMAGE_EXPORT int delegate_image_subscribe(const char* target, const char* event)
{
    gSubscriber = lp_client_create(target, "impostor", nullptr, nullptr);
    if (!gSubscriber) return LP_ERR_UNAVAILABLE;
    gSubscription = lp_subscribe(gSubscriber, event, onEvent, nullptr);
    return gSubscription ? LP_OK : LP_ERR_UNAVAILABLE;
}

IMAGE_EXPORT unsigned long long delegate_image_generation()
{
    return gSubscriber ? lp_client_subscription_generation(gSubscriber) : 0;
}

IMAGE_EXPORT int delegate_image_events() { return gEvents.load(); }

IMAGE_EXPORT void delegate_image_close()
{
    lp_unsubscribe(gSubscription);
    lp_client_destroy(gSubscriber);
    gSubscription = nullptr;
    gSubscriber = nullptr;
}

} // extern "C"
