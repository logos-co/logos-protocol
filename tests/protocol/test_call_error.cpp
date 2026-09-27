#include <gtest/gtest.h>

#include "logos_protocol.h"

#include <nlohmann/json.hpp>

// The call-error channel: invoking a method on a target whose object cannot
// be acquired must fail loudly — LP_ERR_UNAVAILABLE plus the canonical
// {code, message, origin} error JSON — never LP_OK with a null result (the
// silent-default trap generated typed wrappers fell into; see
// logos_call_error.h).
TEST(CallErrorChannel, UnreachableTargetYieldsCanonicalError)
{
    // The local socket of a module nothing serves.
    const char* deadTarget = "{\"protocol\":\"local\"}";

    // Pre-save a token so the capability requestModule flow is skipped —
    // this test exercises the transport-acquisition failure only.
    ASSERT_EQ(lp_token_save("missing_module", "test-token"), LP_OK);

    lp_client* client =
        lp_client_create("missing_module", "origin", deadTarget, deadTarget);
    ASSERT_NE(client, nullptr);

    char* result = nullptr;
    char* error = nullptr;
    const int rc = lp_invoke(client, "anyMethod", "[1,2]", 1500, &result, &error);
    EXPECT_EQ(rc, LP_ERR_UNAVAILABLE);
    EXPECT_EQ(result, nullptr);
    ASSERT_NE(error, nullptr);

    nlohmann::json e = nlohmann::json::parse(error, nullptr, false);
    ASSERT_TRUE(e.is_object());
    EXPECT_EQ(e.value("code", std::string{}), "object_unavailable");
    EXPECT_EQ(e.value("origin", std::string{}), "missing_module");
    EXPECT_FALSE(e.value("message", std::string{}).empty());

    lp_string_free(error);
    lp_client_destroy(client);
}
