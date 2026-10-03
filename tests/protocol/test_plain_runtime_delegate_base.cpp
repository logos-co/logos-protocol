// An image installs a host's table built before the scoped push was appended, and the
// scoped push is then unsupported. Installing is one-shot, so this is its own process.
#include "logos_protocol.h"
#include "logos_runtime_delegate.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

char* answer(const char*, const char*, void*)
{
    char* text = static_cast<char*>(std::malloc(3));
    std::memcpy(text, "42", 3);
    return text;
}
char* noMethods(void*)
{
    char* text = static_cast<char*>(std::malloc(3));
    std::memcpy(text, "[]", 3);
    return text;
}
int acceptToken(const char*, const char*, void*) { return LP_OK; }

TEST(PlainRuntimeDelegateBase, AnOlderHostsTableInstallsWithoutTheScopedPush)
{
#ifdef _WIN32
    ASSERT_EQ(::_putenv_s("LOGOS_INSTANCE_ID", ("delegate_base_" + std::to_string(::_getpid())).c_str()), 0);
#else
    ASSERT_EQ(::setenv("LOGOS_INSTANCE_ID", ("delegate_base_" + std::to_string(::getpid())).c_str(), 1), 0);
#endif
    lp_provider* provider = lp_provider_create("delegate_base_target", R"([{"protocol":"inproc"}])");
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_save_token(provider, "mod_b", "tok-b"), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, answer, noMethods, acceptToken, nullptr), LP_OK);
    ASSERT_EQ(lp_token_isolate_identity("mod_b"), LP_OK);
    ASSERT_EQ(lp_token_adopt_credential("mod_b", "cred-b"), LP_OK);
    ASSERT_EQ(lp_token_save_for("mod_b", "delegate_base_target", "tok-b"), LP_OK);
    const lp_runtime_delegate_v1* current = lp_runtime_delegate_create("mod_b", R"(["token_delivery"])");
    ASSERT_NE(current, nullptr);
    EXPECT_EQ(current->size, static_cast<unsigned>(sizeof(lp_runtime_delegate_v1)));
    EXPECT_NE(current->inform_scoped_module_token_to, nullptr);
    EXPECT_LT(LP_RUNTIME_DELEGATE_V1_BASE_SIZE, sizeof(lp_runtime_delegate_v1));

    lp_runtime_delegate_v1 older = *current;
    older.size = LP_RUNTIME_DELEGATE_V1_BASE_SIZE - 1;
    EXPECT_EQ(lp_runtime_install_delegate(&older), LP_ERR_INVALID_ARG);
    older.size = LP_RUNTIME_DELEGATE_V1_BASE_SIZE;
    older.inform_scoped_module_token_to = nullptr;
    ASSERT_EQ(lp_runtime_install_delegate(&older), LP_OK);

    EXPECT_EQ(lp_inform_scoped_module_token_to(nullptr, "cred-b", "delegate_base_target", "peer",
                                               "t", R"({"methods":["a"]})", 1000),
              LP_ERR_UNSUPPORTED);
    lp_client* client = lp_client_create("delegate_base_target", "ignored", nullptr, nullptr);
    ASSERT_NE(client, nullptr);
    char* result = nullptr;
    char* error = nullptr;
    EXPECT_EQ(lp_invoke(client, "answer", "[]", 2000, &result, &error), LP_OK) << (error ? error : "");
    EXPECT_STREQ(result, "42");
    lp_string_free(result);
    lp_string_free(error);
    lp_client_destroy(client);
    lp_runtime_delegate_release(current);
    lp_provider_destroy(provider);
}

} // namespace
