// lp_revoke_module_token_to withdraws a pushed token at its target, only while
// the target still holds the token the digest names.
#include "logos_protocol.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

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

std::string digestOf(const std::string& token)
{
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    EVP_Digest(token.data(), token.size(), hash, &length, EVP_sha256(), nullptr);
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < length; ++i) {
        out.push_back(hex[hash[i] >> 4]);
        out.push_back(hex[hash[i] & 15]);
    }
    return out;
}

char* echo(const char*, const char*, void*) { return copyText(R"("ok")"); }
char* noMethods(void*) { return copyText("[]"); }
int acceptToken(const char*, const char*, void*) { return LP_OK; }

int callAsPeer(const char* target)
{
    lp_client* client = lp_client_create(target, "peer", nullptr, nullptr);
    char* result = nullptr;
    char* error = nullptr;
    const int status = lp_invoke(client, "echo", "[]", 1500, &result, &error);
    lp_string_free(result);
    lp_string_free(error);
    lp_client_destroy(client);
    return status;
}

class PlainTokenRevocation : public ::testing::TestWithParam<const char*> {};

TEST_P(PlainTokenRevocation, ARevokedTokenStopsAuthorizingAndAStaleOneIsSpared)
{
    const std::string target = std::string("revoked_") + (GetParam()[0] == 'i' ? "inproc" : "socket");
    useInstance(target + "_");
    const std::string transports = std::string(R"([{"protocol":")") + GetParam() + R"("}])";
    lp_provider* provider = lp_provider_create(target.c_str(), transports.c_str());
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_save_token(provider, "core", "anchor"), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, echo, noMethods, acceptToken, nullptr), LP_OK);

    ASSERT_EQ(lp_grant_host_services(R"(["token_delivery"])"), LP_OK);
    ASSERT_EQ(lp_inform_module_token_to(nullptr, "anchor", target.c_str(), "peer", "peer-token",
                                        1000), LP_OK);
    ASSERT_EQ(lp_token_save(target.c_str(), "peer-token"), LP_OK);
    ASSERT_EQ(callAsPeer(target.c_str()), LP_OK);

    // A revocation naming an older token spares the current one.
    EXPECT_NE(lp_revoke_module_token_to(nullptr, "anchor", target.c_str(), "peer",
                                        digestOf("older-token").c_str(), 1000), LP_OK);
    // Neither may anyone without the channel's credential revoke it.
    lp_client* stranger = lp_client_create(target.c_str(), "stranger", nullptr, nullptr);
    EXPECT_NE(lp_revoke_module_token_to(stranger, "not-the-anchor", target.c_str(), "peer",
                                        digestOf("peer-token").c_str(), 1000), LP_OK);
    lp_client_destroy(stranger);
    EXPECT_EQ(callAsPeer(target.c_str()), LP_OK);

    EXPECT_EQ(lp_revoke_module_token_to(nullptr, "anchor", target.c_str(), "peer",
                                        digestOf("peer-token").c_str(), 1000), LP_OK);
    EXPECT_NE(callAsPeer(target.c_str()), LP_OK);

    ASSERT_EQ(lp_grant_host_services("[]"), LP_OK);
    EXPECT_EQ(lp_revoke_module_token_to(nullptr, "anchor", target.c_str(), "peer",
                                        digestOf("peer-token").c_str(), 1000),
              LP_ERR_UNSUPPORTED);
    lp_provider_destroy(provider);
}

TEST(PlainTokenDigest, IsTheHexSha256RevocationNames)
{
    char* digest = lp_token_digest("abc");
    ASSERT_NE(digest, nullptr);
    EXPECT_STREQ(digest, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(std::string(digest), digestOf("abc"));
    lp_string_free(digest);
    EXPECT_EQ(lp_token_digest(nullptr), nullptr);
}

INSTANTIATE_TEST_SUITE_P(OverEachLocalTransport, PlainTokenRevocation,
                         ::testing::Values("qt_remote_plain", "inproc"));

} // namespace
