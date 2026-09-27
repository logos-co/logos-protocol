// The host for PlainExitSessionTest, on the shared plain runtime. It loads a module
// image, serves tls_tcp, lets the image open a session and calls exit(1) with it open.
// argv: <image> [late-openssl: OpenSSL is first used after the image's static]
#include "logos_protocol.h"
#include "logos_runtime_delegate.h"
#include "session_certs.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <dlfcn.h>

namespace {

char* copyText(const std::string& text)
{
    auto* out = static_cast<char*>(std::malloc(text.size() + 1));
    std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
}

char* echo(const char* method, const char* argsJson, void*)
{
    if (std::strcmp(method, "echo") != 0) return nullptr;
    return copyText(nlohmann::json::parse(argsJson).at(0).dump());
}

char* noMethods(void*) { return copyText("[]"); }

char* authenticate(const char*, void*)
{
    return copyText(R"({"caller":{"kind":"remote","peer":"peer-1","name":"exit_image"},)"
                    R"("lifetime_ms":60000,"session":{"peer":"peer-1"}})");
}

int sessionPort(lp_provider* provider)
{
    char* text = lp_provider_endpoints_json(provider);
    const auto endpoints = nlohmann::json::parse(text ? text : "[]");
    lp_string_free(text);
    for (const auto& endpoint : endpoints)
        if (endpoint.value("protocol", "") == "tls_tcp") return endpoint.value("port", 0);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    const bool lateOpenssl = argc > 2 && std::strcmp(argv[2], "late-openssl") == 0;
    std::unique_ptr<session_test::Identity> server;
    std::unique_ptr<session_test::Identity> client;
    const auto makeCerts = [&] {
        server = std::make_unique<session_test::Identity>("serverAuth");
        client = std::make_unique<session_test::Identity>("clientAuth");
    };
    if (!lateOpenssl) makeCerts();

    if (lp_token_isolate_identity("exit_image") != LP_OK
        || lp_token_adopt_credential("exit_image", "exit-credential") != LP_OK)
        return 3;
    const lp_runtime_delegate_v1* delegate = lp_runtime_delegate_create("exit_image", "[]");
    void* image = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!delegate || !image) return 4;
    const auto install = reinterpret_cast<logos_module_set_runtime_delegate_fn>(
        dlsym(image, LOGOS_MODULE_SET_RUNTIME_DELEGATE_SYMBOL));
    const auto hold = reinterpret_cast<void (*)()>(dlsym(image, "exit_image_hold"));
    const auto open = reinterpret_cast<int (*)(int, const char*, const char*, const char*,
                                               const char*)>(dlsym(image, "exit_image_open"));
    if (!install || !hold || !open || install(delegate) != LP_OK) return 5;
    // Made before the runtime's I/O pool, as a module impl is at load.
    hold();
    if (lateOpenssl) makeCerts();

    lp_provider* provider = lp_provider_create(
        "exit_echo", R"([{"protocol":"tls_tcp","host":"127.0.0.1","port":0}])");
    if (!provider
        || lp_provider_set_tls_credential(provider, server->chainPem().c_str(),
                                          server->keyPem().c_str()) != LP_OK
        || lp_provider_set_trust_anchors(provider, client->rootPem().c_str()) != LP_OK
        || lp_provider_set_session_authenticator(provider, &authenticate, nullptr) != LP_OK
        || lp_provider_register(provider, &echo, &noMethods, nullptr, nullptr) != LP_OK)
        return 6;
    const int status = open(sessionPort(provider), server->rootPem().c_str(),
                            server->leafPin().c_str(), client->chainPem().c_str(),
                            client->keyPem().c_str());
    if (status != LP_OK) return 7;
    std::printf("OPEN\n");
    std::fflush(stdout);
    std::exit(1);
}
