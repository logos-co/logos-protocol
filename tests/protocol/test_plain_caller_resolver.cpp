// lp_provider_set_caller_resolver names credentials the provider's table does
// not, and "@op:<name>" pair tokens name operators.
#include "logos_protocol.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <string>

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

char* whoami(const char*, const char*, void*)
{
    return copyText(json(json::parse(lp_current_caller_json())).dump());
}

char* noMethods(void*) { return copyText("[]"); }

int acceptToken(const char*, const char*, void*) { return LP_OK; }

struct Resolution {
    std::string token;
    std::string document;
    std::string transport;
};

char* resolve(const char* token, const char* transport, void* userData)
{
    auto& resolution = *static_cast<Resolution*>(userData);
    resolution.transport = transport ? transport : "";
    if (!token || resolution.token != token) return nullptr;
    return copyText(resolution.document);
}

json callAs(const char* target, const char* origin, const char* token, int* status)
{
    lp_token_save(target, token);
    lp_client* client = lp_client_create(target, origin, nullptr, nullptr);
    char* result = nullptr;
    char* error = nullptr;
    *status = lp_invoke(client, "whoami", "[]", 2000, &result, &error);
    json value = result ? json::parse(result) : json();
    lp_string_free(result);
    lp_string_free(error);
    lp_client_destroy(client);
    return value;
}

TEST(PlainCallerResolver, AResolverNamesAnOperator)
{
    useInstance("resolver_operator_");
    Resolution resolution{"op-token", R"({"kind":"operator","name":"alice"})", {}};
    lp_provider* provider = lp_provider_create("resolver_operator",
                                               R"([{"protocol":"qt_remote_plain"}])");
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_set_caller_resolver(provider, resolve, &resolution), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, whoami, noMethods, acceptToken, nullptr), LP_OK);

    int status = LP_ERR_INTERNAL;
    const json caller = callAs("resolver_operator", "cli_client", "op-token", &status);
    EXPECT_EQ(status, LP_OK);
    EXPECT_EQ(caller, json({{"kind", "operator"}, {"name", "alice"}}));
    EXPECT_EQ(resolution.transport, "local");
    lp_provider_destroy(provider);
}

// A resolver can refuse, and can never make a caller the host.
TEST(PlainCallerResolver, ARefusalOrAHostClaimIsUnauthorized)
{
    useInstance("resolver_refused_");
    Resolution resolution{"claims-host", R"({"kind":"host"})", {}};
    lp_provider* provider = lp_provider_create("resolver_refused",
                                               R"([{"protocol":"qt_remote_plain"}])");
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_set_caller_resolver(provider, resolve, &resolution), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, whoami, noMethods, acceptToken, nullptr), LP_OK);

    int status = LP_OK;
    callAs("resolver_refused", "cli_client", "claims-host", &status);
    EXPECT_NE(status, LP_OK);
    callAs("resolver_refused", "cli_client", "unknown-token", &status);
    EXPECT_NE(status, LP_OK);
    lp_provider_destroy(provider);
}

TEST(PlainCallerResolver, AnOperatorPairTokenNamesTheOperator)
{
    useInstance("resolver_pair_");
    lp_provider* provider = lp_provider_create("resolver_pair",
                                               R"([{"protocol":"qt_remote_plain"}])");
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_save_token(provider, "@op:bob", "bob-pair"), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, whoami, noMethods, acceptToken, nullptr), LP_OK);

    int status = LP_ERR_INTERNAL;
    const json caller = callAs("resolver_pair", "core_service", "bob-pair", &status);
    EXPECT_EQ(status, LP_OK);
    EXPECT_EQ(caller, json({{"kind", "operator"}, {"name", "bob"}}));
    lp_provider_destroy(provider);
}

} // namespace
