// A module image for PlainExitSessionTest: its own static plain runtime, and a
// tls_tcp client kept in a function-local static, as the SDK glue keeps a module impl.
#include "logos_protocol.h"
#include "logos_runtime_delegate.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <string>

#define IMAGE_EXPORT __attribute__((visibility("default")))

namespace {

struct Dial {
    int port = 0;
    std::string anchors;
    std::string pin;
};

// Destroyed by exit, like the impl of a module its host never unloaded.
struct Held {
    Dial dial;
    lp_client* client = nullptr;
    ~Held()
    {
        if (client) lp_client_destroy(client);
    }
};

Held& held()
{
    static Held value;
    return value;
}

char* copyText(const std::string& text)
{
    auto* out = static_cast<char*>(std::malloc(text.size() + 1));
    std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
}

char* dialHook(const char*, void* userData)
{
    const auto& dial = *static_cast<const Dial*>(userData);
    return copyText(nlohmann::json{{"addresses", {"127.0.0.1"}}, {"port", dial.port},
                                   {"server_pin", dial.pin}, {"anchors", dial.anchors}}
                        .dump());
}

char* helloHook(const char*, void*) { return copyText(R"({"module":"exit_image"})"); }

} // namespace

extern "C" {

IMAGE_EXPORT int logos_module_set_runtime_delegate(const lp_runtime_delegate_v1* delegate)
{
    return lp_runtime_install_delegate(delegate);
}

IMAGE_EXPORT void exit_image_hold() { (void)held(); }

// A session through the host's runtime, left open.
IMAGE_EXPORT int exit_image_open(int port, const char* anchorsPem, const char* serverPin,
                                 const char* chainPem, const char* keyPem)
{
    Held& h = held();
    h.dial = Dial{port, anchorsPem, serverPin};
    h.client = lp_client_create("exit_echo", "exit_image", R"({"protocol":"tls_tcp"})", nullptr);
    if (!h.client) return LP_ERR_UNAVAILABLE;
    int status = lp_client_set_tls_credential(h.client, chainPem, keyPem);
    if (status == LP_OK) status = lp_client_set_session_hook(h.client, &dialHook, &helloHook, &h.dial);
    if (status != LP_OK) return status;
    char* result = nullptr;
    char* error = nullptr;
    status = lp_invoke(h.client, "echo", R"(["open"])", 5000, &result, &error);
    lp_string_free(result);
    lp_string_free(error);
    return status;
}

} // extern "C"
