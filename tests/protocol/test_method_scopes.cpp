// A scoped token push (lp_inform_scoped_module_token_to) limits the token to listed
// methods; the target refuses the rest with not_authorised before any module code.
#include "logos_protocol.h"
#include "implementations/qt_remote_plain/qtro_transport.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
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

std::string instanceFor(const std::string& prefix)
{
#ifdef _WIN32
    const std::string instance = prefix + std::to_string(::_getpid());
    _putenv_s("LOGOS_INSTANCE_ID", instance.c_str());
#else
    const std::string instance = prefix + std::to_string(::getpid());
    ::setenv("LOGOS_INSTANCE_ID", instance.c_str(), 1);
#endif
    return instance;
}

std::string digestOf(const std::string& token)
{
    char* digest = lp_token_digest(token.c_str());
    std::string out = digest ? digest : "";
    lp_string_free(digest);
    return out;
}

std::uint16_t freePort()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor reservation(io, {boost::asio::ip::address_v4::loopback(), 0});
    return reservation.local_endpoint().port();
}

std::string tcpConfig(std::uint16_t port)
{
    return R"({"protocol":"tcp","host":"127.0.0.1","port":)" + std::to_string(port) + "}";
}

struct Target {
    std::mutex mutex;
    std::vector<std::string> dispatched;
    std::string lastCaller;
};

char* targetDispatch(const char* method, const char*, void* user)
{
    auto& target = *static_cast<Target*>(user);
    std::lock_guard<std::mutex> lock(target.mutex);
    target.dispatched.push_back(method);
    target.lastCaller = lp_current_caller_json();
    return copyText(R"("ok")");
}

char* noMethods(void*) { return copyText("[]"); }
int acceptToken(const char*, const char*, void*) { return LP_OK; }

std::atomic<int> gRequests{0};

char* capabilityDispatch(const char* method, const char*, void*)
{
    if (std::strcmp(method, "requestModule") == 0) {
        ++gRequests;
        return copyText(R"("reminted")");
    }
    return copyText("null");
}

constexpr const char* kScope = R"({"methods":["allowed","other"]})";

class MethodScopes : public ::testing::TestWithParam<const char*> {
protected:
    struct Outcome {
        int status = LP_ERR_INTERNAL;
        std::string code;
    };

    void SetUp() override
    {
        const std::string transport = GetParam();
        const std::string test = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        m_name = "scoped_" + std::to_string(std::hash<std::string>{}(test) % 100000);
        instanceFor("ms_" + transport.substr(0, 3) + "_");
        m_capabilityPort = freePort();
        const std::string capabilitySet = R"([{"protocol":"qt_remote_plain"},{"protocol":"inproc"},)"
            + tcpConfig(m_capabilityPort) + "]";
        m_capability = lp_provider_create("capability_module", capabilitySet.c_str());
        ASSERT_NE(m_capability, nullptr);
        ASSERT_EQ(lp_provider_save_token(m_capability, "peer", "cred"), LP_OK);
        ASSERT_EQ(lp_provider_register(m_capability, capabilityDispatch, noMethods, acceptToken,
                                       nullptr), LP_OK);

        std::string set = R"([{"protocol":"qt_remote_plain"}])";
        if (transport == "inproc") {
            set = R"([{"protocol":"inproc"}])";
            m_targetConfig = R"({"protocol":"inproc"})";
        } else if (transport == "tcp") {
            m_targetConfig = tcpConfig(freePort());
            set = R"([{"protocol":"qt_remote_plain"},)" + m_targetConfig + "]";
            m_capabilityConfig = tcpConfig(m_capabilityPort);
        }
        m_provider = lp_provider_create(m_name.c_str(), set.c_str());
        ASSERT_NE(m_provider, nullptr);
        ASSERT_EQ(lp_provider_save_token(m_provider, "core", "anchor"), LP_OK);
        ASSERT_EQ(lp_provider_register(m_provider, targetDispatch, noMethods, acceptToken,
                                       &m_target), LP_OK);
        ASSERT_EQ(lp_grant_host_services(R"(["token_delivery"])"), LP_OK);
        ASSERT_EQ(lp_token_save("capability_module", "cred"), LP_OK);
        gRequests = 0;
    }

    void TearDown() override
    {
        lp_grant_host_services("[]");
        if (m_provider) lp_provider_destroy(m_provider);
        if (m_capability) lp_provider_destroy(m_capability);
    }

    int pushScoped(const std::string& token, const char* scope = kScope)
    {
        return lp_inform_scoped_module_token_to(nullptr, "anchor", m_name.c_str(), "peer",
                                                token.c_str(), scope, 2000);
    }

    int push(const std::string& token)
    {
        return lp_inform_module_token_to(nullptr, "anchor", m_name.c_str(), "peer", token.c_str(),
                                         2000);
    }

    Outcome call(const std::string& token, const char* method, const char* args = "[]")
    {
        lp_token_save(m_name.c_str(), token.c_str());
        lp_client* client = lp_client_create(m_name.c_str(), "peer",
            m_targetConfig.empty() ? nullptr : m_targetConfig.c_str(),
            m_capabilityConfig.empty() ? nullptr : m_capabilityConfig.c_str());
        Outcome outcome;
        char* result = nullptr;
        char* error = nullptr;
        outcome.status = lp_invoke(client, method, args, 2000, &result, &error);
        if (error) outcome.code = json::parse(error, nullptr, false).value("code", std::string{});
        lp_string_free(result);
        lp_string_free(error);
        lp_client_destroy(client);
        return outcome;
    }

    std::string lastCaller()
    {
        std::lock_guard<std::mutex> lock(m_target.mutex);
        return m_target.lastCaller;
    }

    std::size_t dispatches()
    {
        std::lock_guard<std::mutex> lock(m_target.mutex);
        return m_target.dispatched.size();
    }

    std::string m_name;
    std::string m_targetConfig;
    std::string m_capabilityConfig;
    std::uint16_t m_capabilityPort = 0;
    lp_provider* m_provider = nullptr;
    lp_provider* m_capability = nullptr;
    Target m_target;
};

TEST_P(MethodScopes, AnOutOfScopeCallIsRefusedBeforeDispatchAndNeverReexchanged)
{
    ASSERT_EQ(pushScoped("peer-token"), LP_OK);
    EXPECT_EQ(call("peer-token", "allowed").status, LP_OK);
    const json caller = json::parse(lastCaller(), nullptr, false);
    EXPECT_EQ(caller.value("name", std::string{}), "peer");
    EXPECT_EQ(caller.value("scoped", false), true) << lastCaller();

    const std::size_t before = dispatches();
    const Outcome refused = call("peer-token", "forbidden");
    EXPECT_NE(refused.status, LP_OK);
    EXPECT_EQ(refused.code, "not_authorised");
    EXPECT_EQ(dispatches(), before) << "module code ran for a refused method";
    EXPECT_EQ(gRequests.load(), 0) << "a refusal was answered with a token re-exchange";
}

TEST_P(MethodScopes, EveryUnscopedPushAndEveryRevocationClearsTheScope)
{
    ASSERT_EQ(pushScoped("peer-token"), LP_OK);
    ASSERT_EQ(call("peer-token", "forbidden").code, "not_authorised");
    ASSERT_EQ(push("peer-token"), LP_OK);
    EXPECT_EQ(call("peer-token", "forbidden").status, LP_OK);
    EXPECT_FALSE(json::parse(lastCaller(), nullptr, false).contains("scoped")) << lastCaller();

    ASSERT_EQ(pushScoped("peer-token"), LP_OK);
    ASSERT_EQ(call("peer-token", "forbidden").code, "not_authorised");
    ASSERT_EQ(lp_provider_save_token(m_provider, "peer", "peer-token"), LP_OK);
    EXPECT_EQ(call("peer-token", "forbidden").status, LP_OK);

    ASSERT_EQ(pushScoped("peer-token"), LP_OK);
    ASSERT_EQ(lp_revoke_module_token_to(nullptr, "anchor", m_name.c_str(), "peer",
                                        digestOf("peer-token").c_str(), 2000), LP_OK);
    EXPECT_EQ(call("peer-token", "allowed").code, "unauthorized");
    // The same value pushed unscoped again carries no leftover scope.
    ASSERT_EQ(push("peer-token"), LP_OK);
    EXPECT_EQ(call("peer-token", "forbidden").status, LP_OK);
}

TEST_P(MethodScopes, AStaleRevocationSparesANewerScopedToken)
{
    ASSERT_EQ(pushScoped("older"), LP_OK);
    ASSERT_EQ(pushScoped("newer", R"({"methods":["other"]})"), LP_OK);
    EXPECT_NE(lp_revoke_module_token_to(nullptr, "anchor", m_name.c_str(), "peer",
                                        digestOf("older").c_str(), 2000), LP_OK);
    EXPECT_EQ(call("newer", "other").status, LP_OK);
    EXPECT_EQ(call("newer", "allowed").code, "not_authorised");
}

TEST_P(MethodScopes, IdentityAndIntrospectionStayOpen)
{
    ASSERT_EQ(pushScoped("peer-token"), LP_OK);
    for (const char* method : {"name", "version", "lidl"})
        EXPECT_EQ(call("peer-token", method).status, LP_OK) << method;
    EXPECT_EQ(call("peer-token", "name", R"(["x"])").code, "not_authorised");
    lp_token_save(m_name.c_str(), "peer-token");
    lp_client* client = lp_client_create(m_name.c_str(), "peer",
        m_targetConfig.empty() ? nullptr : m_targetConfig.c_str(), nullptr);
    char* methods = lp_get_methods(client);
    EXPECT_NE(methods, nullptr);
    lp_string_free(methods);
    lp_client_destroy(client);
}

TEST_P(MethodScopes, AScopedTokenThatAlsoMatchesAnotherKeyIsRefused)
{
    ASSERT_EQ(lp_provider_save_token(m_provider, "someone_else", "shared"), LP_OK);
    ASSERT_EQ(push("shared"), LP_OK);
    EXPECT_EQ(call("shared", "forbidden").status, LP_OK) << "two unscoped keys stay callable";
    ASSERT_EQ(pushScoped("shared"), LP_OK);
    EXPECT_EQ(call("shared", "allowed").code, "not_authorised");
}

TEST_P(MethodScopes, AMalformedScopeIsRefusedBeforeAnythingIsSent)
{
    for (const char* scope : {"", "[]", R"({"methods":[]})", R"({"methods":["a","a"]})",
                              R"({"methods":["*"]})", R"({"methods":["a"],"events":["e"]})",
                              R"({"methods":[1]})"})
        EXPECT_EQ(pushScoped("peer-token", scope), LP_ERR_INVALID_ARG) << scope;
    ASSERT_EQ(lp_grant_host_services("[]"), LP_OK);
    EXPECT_EQ(pushScoped("peer-token"), LP_ERR_UNSUPPORTED);
}

INSTANTIATE_TEST_SUITE_P(OverEachTransport, MethodScopes,
                         ::testing::Values("qt_remote_plain", "inproc", "tcp"));

// A target whose handshake object lacks the method is sent nothing at all.
TEST(MethodScopesOldTarget, ATargetWithoutTheMethodReceivesNothing)
{
    using namespace logos::qt_remote_plain;
    const std::string instance = instanceFor("ms_old_");
    std::atomic<int> invoked{0};
    Server server;
    ASSERT_TRUE(server.start("local:logos_old_target_" + instance));
    ClassDefinition old{"ModuleHandshakeProxy", {},
        {{"informModuleToken(QString,QString,QString)", "bool", {"authToken", "moduleName", "token"}},
         {"revokeModuleToken(QString,QString,QString)", "bool",
          {"authToken", "moduleName", "tokenDigest"}}},
        {}};
    ASSERT_TRUE(server.publish({"old_target__handshake", old,
        [&](std::int32_t, const std::vector<Variant>&) {
            ++invoked;
            return Variant::fromRpc(logos::plain::RpcValue{true});
        },
        [](std::int32_t) { return true; }}));
    ASSERT_EQ(lp_grant_host_services(R"(["token_delivery"])"), LP_OK);
    EXPECT_EQ(lp_inform_scoped_module_token_to(nullptr, "anchor", "old_target", "peer", "t",
                                               kScope, 2000), LP_ERR_TARGET_UNSUPPORTED);
    EXPECT_EQ(invoked.load(), 0);
    EXPECT_EQ(lp_inform_module_token_to(nullptr, "anchor", "old_target", "peer", "t", 2000),
              LP_OK) << "control: the same target takes an unscoped push";
    EXPECT_EQ(invoked.load(), 1);
    lp_grant_host_services("[]");
    server.stop();
}

} // namespace
