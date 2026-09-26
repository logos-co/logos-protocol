// tls_tcp: mutual TLS 1.3 against embedder anchors, a Hello the provider's
// authenticator turns into the caller, and no tokens over a session.
#include "logos_protocol.h"
#include "logos_transport_config_json.h"
#include "session_certs.h"

#include <gtest/gtest.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/ssl.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
using session_test::Identity;

namespace {

char* copyText(const std::string& value)
{
    auto* result = static_cast<char*>(std::malloc(value.size() + 1));
    std::memcpy(result, value.c_str(), value.size() + 1);
    return result;
}

struct Provider {
    std::atomic<int> running{0};
    std::atomic<int> peak{0};
    std::mutex mutex;
    std::vector<json> requests; // what the authenticator saw
    std::string reply = R"({"caller":{"kind":"remote","peer":"peer-1","name":"wallet"},)"
                        R"("lifetime_ms":60000,"session":{"peer":"peer-1","route":"r1","generation":1}})";
    std::string acceptTicket = "good";

    int authCount()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<int>(requests.size());
    }
};

char* dispatch(const char* method, const char* argsJson, void* userData)
{
    auto& p = *static_cast<Provider*>(userData);
    if (std::strcmp(method, "whoami") == 0) return copyText(lp_current_caller_json());
    if (std::strcmp(method, "echo") == 0) {
        const json args = json::parse(argsJson);
        return copyText(args.at(0).dump());
    }
    if (std::strcmp(method, "slow") == 0) {
        const int now = ++p.running;
        int seen = p.peak.load();
        while (now > seen && !p.peak.compare_exchange_weak(seen, now)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        --p.running;
        return copyText("true");
    }
    if (std::strcmp(method, "pending_shape") == 0)
        return copyText(R"({"__logos_pending_call__":"x1"})");
    return nullptr;
}

char* methods(void*) { return copyText("[]"); }

char* authenticate(const char* requestJson, void* userData)
{
    auto& p = *static_cast<Provider*>(userData);
    const json request = json::parse(requestJson);
    std::string reply;
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        p.requests.push_back(request);
        reply = request["hello"].value("ticket", "") == p.acceptTicket
            ? p.reply : std::string(R"({"error":"NOT_AUTHORISED"})");
    }
    return copyText(reply);
}

struct Client {
    std::string anchors;
    std::string pin;
    int port = 0;
    std::string ticket = "good";
    bool unanchored = false;
    std::atomic<int> dials{0};
    std::mutex mutex;
    std::vector<json> hellos; // what the hello hook saw
};

char* dialHook(const char*, void* userData)
{
    auto& c = *static_cast<Client*>(userData);
    ++c.dials;
    if (c.unanchored)
        return copyText(json{{"addresses", {"127.0.0.1"}}, {"port", c.port},
                             {"unanchored", true}}.dump());
    return copyText(json{{"addresses", {"127.0.0.1"}}, {"port", c.port},
                         {"server_pin", c.pin}, {"anchors", c.anchors}}.dump());
}

char* helloHook(const char* request, void* userData)
{
    auto& c = *static_cast<Client*>(userData);
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        c.hellos.push_back(json::parse(request));
    }
    return copyText(json{{"ticket", c.ticket}, {"module", "session_echo"}}.dump());
}

struct Fixture {
    Identity server{"serverAuth"};
    Identity client{"clientAuth"};
    Provider state;
    Client hooks;
    lp_provider* provider = nullptr;
    lp_client* consumer = nullptr;

    explicit Fixture(const char* options = nullptr, const char* transport =
        R"([{"protocol":"tls_tcp","host":"127.0.0.1","port":0}])")
    {
        provider = lp_provider_create("session_echo", transport);
        EXPECT_NE(provider, nullptr);
        EXPECT_EQ(lp_provider_set_tls_credential(provider, server.chainPem().c_str(),
                                                 server.keyPem().c_str()), LP_OK);
        EXPECT_EQ(lp_provider_set_trust_anchors(provider, client.rootPem().c_str()), LP_OK);
        EXPECT_EQ(lp_provider_set_session_authenticator(provider, &authenticate, &state), LP_OK);
        if (options) EXPECT_EQ(lp_provider_set_session_options(provider, options), LP_OK);
    }

    void start()
    {
        ASSERT_EQ(lp_provider_register(provider, &dispatch, &methods, nullptr, &state), LP_OK);
        hooks.port = sessionPort();
        hooks.anchors = server.rootPem();
        hooks.pin = server.leafPin();
        consumer = lp_client_create("session_echo", "anyone", R"({"protocol":"tls_tcp"})", nullptr);
        ASSERT_NE(consumer, nullptr);
        ASSERT_EQ(lp_client_set_tls_credential(consumer, client.chainPem().c_str(),
                                               client.keyPem().c_str()), LP_OK);
        ASSERT_EQ(lp_client_set_session_hook(consumer, &dialHook, &helloHook, &hooks), LP_OK);
    }

    int sessionPort()
    {
        char* text = lp_provider_endpoints_json(provider);
        const json endpoints = json::parse(text ? text : "[]");
        lp_string_free(text);
        for (const auto& e : endpoints)
            if (e.value("protocol", "") == "tls_tcp") return e.value("port", 0);
        return 0;
    }

    int invoke(const char* method, const std::string& args, json* result, json* error = nullptr,
               int timeout = 5000)
    {
        char* out = nullptr;
        char* err = nullptr;
        const int status = lp_invoke(consumer, method, args.c_str(), timeout, &out, &err);
        if (result && out) *result = json::parse(out);
        if (error && err) *error = json::parse(err);
        lp_string_free(out);
        lp_string_free(err);
        return status;
    }

    ~Fixture()
    {
        if (consumer) lp_client_destroy(consumer);
        if (provider) lp_provider_destroy(provider);
    }
};

} // namespace

TEST(RemoteSession, TheSessionsCallerIsWhatTheAuthenticatorBound)
{
    Fixture f;
    f.start();
    json result;
    ASSERT_EQ(f.invoke("whoami", "[]", &result), LP_OK);
    EXPECT_EQ(result, json({{"kind", "remote"}, {"peer", "peer-1"}, {"name", "wallet"}}));
    // One session answers every later call: no second Hello.
    ASSERT_EQ(f.invoke("echo", R"(["hello"])", &result), LP_OK);
    EXPECT_EQ(result, "hello");
    EXPECT_EQ(f.state.authCount(), 1);
}

TEST(RemoteSession, TheAuthenticatorSeesWhatTlsProved)
{
    Fixture f;
    f.start();
    json result;
    ASSERT_EQ(f.invoke("whoami", "[]", &result), LP_OK);
    std::lock_guard<std::mutex> lock(f.state.mutex);
    ASSERT_EQ(f.state.requests.size(), 1u);
    const json& request = f.state.requests.front();
    ASSERT_TRUE(request["peer_chain"].is_array());
    ASSERT_GE(request["peer_chain"].size(), 1u);
    EXPECT_EQ(request["peer_chain"][0], session_test::der64(f.client.leaf.get()));
    EXPECT_EQ(request["exporter"].get<std::string>().size(), 43u);
    EXPECT_EQ(request["hello"]["wire"], 1);
    EXPECT_EQ(request["hello"]["codec"], "json");
    EXPECT_EQ(request["hello"]["module"], "session_echo");
    EXPECT_EQ(request["protocol"], "tls_tcp");
    EXPECT_EQ(request["port"], f.hooks.port);
}

TEST(RemoteSession, ARefusedHelloFailsTheCall)
{
    Fixture f;
    f.start();
    f.hooks.ticket = "bad";
    json result;
    json error;
    EXPECT_NE(f.invoke("whoami", "[]", &result, &error, 3000), LP_OK);
    EXPECT_GE(f.state.authCount(), 1);
}

TEST(RemoteSession, AHostCallerIsNeverBound)
{
    Fixture f;
    f.state.reply = R"({"caller":{"kind":"host"},"lifetime_ms":60000})";
    f.start();
    json result;
    EXPECT_NE(f.invoke("whoami", "[]", &result, nullptr, 3000), LP_OK);
}

TEST(RemoteSession, AClientFromAnotherRootIsRefusedAtTls)
{
    Fixture f;
    f.start();
    const Identity stranger("clientAuth");
    ASSERT_EQ(lp_client_set_tls_credential(f.consumer, stranger.chainPem().c_str(),
                                           stranger.keyPem().c_str()), LP_OK);
    json result;
    EXPECT_NE(f.invoke("whoami", "[]", &result, nullptr, 3000), LP_OK);
    EXPECT_EQ(f.state.authCount(), 0);
}

TEST(RemoteSession, AServerLeafIsNoClientCredential)
{
    Fixture f;
    f.start();
    // Chains to the anchor, but its EKU says server: refused by the purpose check.
    const Identity serverLike("serverAuth");
    ASSERT_EQ(lp_provider_set_trust_anchors(f.provider, serverLike.rootPem().c_str()), LP_OK);
    ASSERT_EQ(lp_client_set_tls_credential(f.consumer, serverLike.chainPem().c_str(),
                                           serverLike.keyPem().c_str()), LP_OK);
    json result;
    EXPECT_NE(f.invoke("whoami", "[]", &result, nullptr, 3000), LP_OK);
    EXPECT_EQ(f.state.authCount(), 0);
}

TEST(RemoteSession, AnotherServerKeyIsRefusedByThePin)
{
    Fixture f;
    f.start();
    const Identity other("serverAuth");
    f.hooks.pin = other.leafPin();
    json result;
    json error;
    EXPECT_NE(f.invoke("whoami", "[]", &result, &error, 3000), LP_OK);
    EXPECT_EQ(f.state.authCount(), 0);
}

TEST(RemoteSession, NoTokenCrossesASession)
{
    Fixture f;
    f.start();
    json result;
    json error;
    EXPECT_NE(f.invoke("informModuleToken", R"(["capability_module","t"])", &result, &error), LP_OK);
    EXPECT_EQ(error.value("code", ""), "unauthorized");
    EXPECT_NE(f.invoke("revokeModuleToken", R"(["x","d"])", &result, &error), LP_OK);
    EXPECT_EQ(error.value("code", ""), "unauthorized");
}

TEST(RemoteSession, ThePendingShapeIsOnlyDataOverASession)
{
    Fixture f;
    f.start();
    json result;
    ASSERT_EQ(f.invoke("pending_shape", "[]", &result, nullptr, 2000), LP_OK);
    EXPECT_EQ(result, json({{"__logos_pending_call__", "x1"}}));
}

TEST(RemoteSession, ClosedSessionsAreRedialledWithANewHello)
{
    Fixture f;
    f.start();
    json result;
    ASSERT_EQ(f.invoke("whoami", "[]", &result), LP_OK);
    EXPECT_EQ(lp_provider_close_sessions(f.provider, R"({"peer":"other"})"), 0);
    EXPECT_EQ(lp_provider_close_sessions(f.provider, R"({"peer":"peer-1"})"), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_EQ(f.invoke("whoami", "[]", &result), LP_OK);
    EXPECT_EQ(f.state.authCount(), 2);
    EXPECT_EQ(lp_provider_close_sessions(f.provider, R"({"generation_below":2})"), 1);
}

TEST(RemoteSession, ASessionEndsWhenItsLifetimeDoes)
{
    Fixture f;
    f.state.reply = R"({"caller":{"kind":"remote","peer":"peer-1","name":"wallet"},"lifetime_ms":300})";
    f.start();
    json result;
    ASSERT_EQ(f.invoke("whoami", "[]", &result), LP_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    EXPECT_EQ(lp_provider_close_sessions(f.provider, "{}"), 0);
}

TEST(RemoteSession, AnUnknownRootPassesOnlyWhileUnanchoredAdmissionIsOn)
{
    Fixture f;
    f.start();
    const Identity stranger("clientAuth");
    ASSERT_EQ(lp_client_set_tls_credential(f.consumer, stranger.chainPem().c_str(),
                                           stranger.keyPem().c_str()), LP_OK);
    json result;
    EXPECT_NE(f.invoke("whoami", "[]", &result, nullptr, 3000), LP_OK);
    EXPECT_EQ(f.state.authCount(), 0);

    ASSERT_EQ(lp_provider_set_unanchored_admission(f.provider, 1), LP_OK);
    ASSERT_EQ(f.invoke("whoami", "[]", &result), LP_OK);
    std::lock_guard<std::mutex> lock(f.state.mutex);
    ASSERT_EQ(f.state.requests.size(), 1u);
    const json& request = f.state.requests.front();
    EXPECT_EQ(request["anchored"], false);
    ASSERT_EQ(request["peer_chain"].size(), 2u);
    EXPECT_EQ(request["peer_chain"][0], session_test::der64(stranger.leaf.get()));
    EXPECT_EQ(request["peer_chain"][1], session_test::der64(stranger.root.get()));
}

TEST(RemoteSession, AnAnchoredClientIsMarkedSoWhileAdmissionIsOn)
{
    Fixture f;
    ASSERT_EQ(lp_provider_set_unanchored_admission(f.provider, 1), LP_OK);
    f.start();
    json result;
    ASSERT_EQ(f.invoke("whoami", "[]", &result), LP_OK);
    std::lock_guard<std::mutex> lock(f.state.mutex);
    ASSERT_EQ(f.state.requests.size(), 1u);
    EXPECT_EQ(f.state.requests.front()["anchored"], true);
}

TEST(RemoteSession, ALeafWithoutItsRootIsRefusedEvenWhenAdmitted)
{
    Fixture f;
    ASSERT_EQ(lp_provider_set_unanchored_admission(f.provider, 1), LP_OK);
    f.start();
    const Identity stranger("clientAuth");
    ASSERT_EQ(lp_client_set_tls_credential(f.consumer,
                                           session_test::pem(stranger.leaf.get()).c_str(),
                                           stranger.keyPem().c_str()), LP_OK);
    json result;
    EXPECT_NE(f.invoke("whoami", "[]", &result, nullptr, 3000), LP_OK);
    EXPECT_EQ(f.state.authCount(), 0);
}

TEST(RemoteSession, AnUnanchoredClientStillNeedsTheClientPurpose)
{
    Fixture f;
    ASSERT_EQ(lp_provider_set_unanchored_admission(f.provider, 1), LP_OK);
    f.start();
    const Identity serverLike("serverAuth");
    ASSERT_EQ(lp_client_set_tls_credential(f.consumer, serverLike.chainPem().c_str(),
                                           serverLike.keyPem().c_str()), LP_OK);
    json result;
    EXPECT_NE(f.invoke("whoami", "[]", &result, nullptr, 3000), LP_OK);
    EXPECT_EQ(f.state.authCount(), 0);
}

TEST(RemoteSession, AnUnanchoredDialLetsTheHelloHookDecide)
{
    Fixture f;
    f.start();
    f.hooks.unanchored = true;
    json result;
    ASSERT_EQ(f.invoke("whoami", "[]", &result), LP_OK);
    std::lock_guard<std::mutex> lock(f.hooks.mutex);
    ASSERT_EQ(f.hooks.hellos.size(), 1u);
    const json& hello = f.hooks.hellos.front();
    EXPECT_EQ(hello["anchored"], false);
    ASSERT_EQ(hello["peer_chain"].size(), 2u);
    EXPECT_EQ(hello["peer_chain"][0], session_test::der64(f.server.leaf.get()));
    EXPECT_EQ(hello["peer_chain"][1], session_test::der64(f.server.root.get()));
}

TEST(RemoteSession, APinnedDialIsMarkedAnchored)
{
    Fixture f;
    f.start();
    json result;
    ASSERT_EQ(f.invoke("whoami", "[]", &result), LP_OK);
    std::lock_guard<std::mutex> lock(f.hooks.mutex);
    ASSERT_EQ(f.hooks.hellos.size(), 1u);
    EXPECT_EQ(f.hooks.hellos.front()["anchored"], true);
}

TEST(RemoteSession, AnUnanchoredDialStillNeedsTheServerPurpose)
{
    Fixture f;
    // A server presenting a client leaf: self-consistent, wrong purpose.
    const Identity clientLike("clientAuth");
    ASSERT_EQ(lp_provider_set_tls_credential(f.provider, clientLike.chainPem().c_str(),
                                             clientLike.keyPem().c_str()), LP_OK);
    f.start();
    f.hooks.unanchored = true;
    json result;
    EXPECT_NE(f.invoke("whoami", "[]", &result, nullptr, 3000), LP_OK);
    EXPECT_EQ(f.state.authCount(), 0);
}

TEST(RemoteSession, ExtendingASessionKeepsItOpen)
{
    Fixture f;
    f.state.reply = R"({"caller":{"kind":"remote","peer":"peer-1","name":"wallet"},"lifetime_ms":400,)"
                    R"("session":{"route":"r1"}})";
    f.start();
    json result;
    ASSERT_EQ(f.invoke("whoami", "[]", &result), LP_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(lp_provider_extend_sessions(f.provider, R"({"route":"r1"})", 5000), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    EXPECT_EQ(lp_provider_close_sessions(f.provider, "{}"), 1);
}

namespace {
struct Events {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> seen;
    bool armed = false;
};
void onEvent(const char* name, const char*, void* userData)
{
    auto& e = *static_cast<Events*>(userData);
    std::lock_guard<std::mutex> lock(e.mutex);
    e.seen.push_back(name);
    e.changed.notify_all();
}
void onStatus(int status, unsigned long long, const char*, void* userData)
{
    auto& e = *static_cast<Events*>(userData);
    std::lock_guard<std::mutex> lock(e.mutex);
    if (status == LP_SUB_ARMED) e.armed = true;
    e.changed.notify_all();
}
} // namespace

TEST(RemoteSession, EventsArriveButNeverTheCompletionChannel)
{
    Fixture f;
    f.start();
    Events events;
    lp_client_set_subscription_status_cb(f.consumer, &onStatus, &events);
    lp_subscription* subscription = lp_subscribe(f.consumer, "", &onEvent, &events);
    ASSERT_NE(subscription, nullptr);
    {
        std::unique_lock<std::mutex> lock(events.mutex);
        ASSERT_TRUE(events.changed.wait_for(lock, std::chrono::seconds(5), [&] { return events.armed; }));
    }
    // The wildcard Subscribe was written before ARMED; a call after it is ordered behind it.
    json result;
    ASSERT_EQ(f.invoke("whoami", "[]", &result), LP_OK);
    ASSERT_EQ(lp_provider_emit_event(f.provider, "__logos_call_complete__", R"(["x1",1])"), LP_OK);
    ASSERT_EQ(lp_provider_emit_event(f.provider, "tick", "[1]"), LP_OK);
    {
        std::unique_lock<std::mutex> lock(events.mutex);
        ASSERT_TRUE(events.changed.wait_for(lock, std::chrono::seconds(5),
                                            [&] { return !events.seen.empty(); }));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        EXPECT_EQ(events.seen, std::vector<std::string>{"tick"});
    }
    lp_unsubscribe(subscription);
}

TEST(RemoteSession, ASingleProviderRunsOneSessionCallAtATime)
{
    Fixture f;
    ASSERT_EQ(lp_provider_set_max_concurrent_calls(f.provider, 1), LP_OK);
    f.start();
    lp_client* second = lp_client_create("session_echo", "anyone", R"({"protocol":"tls_tcp"})", nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_EQ(lp_client_set_tls_credential(second, f.client.chainPem().c_str(),
                                           f.client.keyPem().c_str()), LP_OK);
    ASSERT_EQ(lp_client_set_session_hook(second, &dialHook, &helloHook, &f.hooks), LP_OK);
    std::thread other([&] {
        char* out = nullptr;
        char* err = nullptr;
        lp_invoke(second, "slow", "[]", 5000, &out, &err);
        lp_string_free(out);
        lp_string_free(err);
    });
    json result;
    ASSERT_EQ(f.invoke("slow", "[]", &result), LP_OK);
    other.join();
    EXPECT_EQ(f.state.peak.load(), 1);
    lp_client_destroy(second);
}

TEST(RemoteSession, TheSenderRefusesACallOverTheNegotiatedLimit)
{
    Fixture f(R"({"max_frame":65536})");
    f.start();
    json result;
    json error;
    const std::string big = json::array({std::string(100 * 1024, 'x')}).dump();
    const auto started = std::chrono::steady_clock::now();
    EXPECT_NE(f.invoke("echo", big, &result, &error, 5000), LP_OK);
    EXPECT_EQ(error.value("code", ""), "invalid_arg");
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(3));
    // The session survives it.
    ASSERT_EQ(f.invoke("echo", R"(["small"])", &result), LP_OK);
    EXPECT_EQ(result, "small");
}

TEST(RemoteSession, AnEndpointCanBeAddedAfterRegistration)
{
    Fixture f(nullptr, R"([{"protocol":"tcp","host":"127.0.0.1","port":0}])");
    ASSERT_EQ(lp_provider_register(f.provider, &dispatch, &methods, nullptr, &f.state), LP_OK);
    EXPECT_EQ(f.sessionPort(), 0);
    ASSERT_EQ(lp_provider_add_endpoint(f.provider, R"({"protocol":"tls_tcp","host":"127.0.0.1","port":0})"),
              LP_OK);
    char* text = lp_provider_endpoints_json(f.provider);
    const json endpoints = json::parse(text);
    lp_string_free(text);
    ASSERT_EQ(endpoints.size(), 2u);
    EXPECT_FALSE(endpoints[0].contains("key_file"));
    f.hooks.port = f.sessionPort();
    f.hooks.anchors = f.server.rootPem();
    f.hooks.pin = f.server.leafPin();
    f.consumer = lp_client_create("session_echo", "anyone", R"({"protocol":"tls_tcp"})", nullptr);
    ASSERT_EQ(lp_client_set_tls_credential(f.consumer, f.client.chainPem().c_str(),
                                           f.client.keyPem().c_str()), LP_OK);
    ASSERT_EQ(lp_client_set_session_hook(f.consumer, &dialHook, &helloHook, &f.hooks), LP_OK);
    json result;
    ASSERT_EQ(f.invoke("whoami", "[]", &result), LP_OK);
    EXPECT_EQ(result["kind"], "remote");
}

TEST(RemoteSession, APortRangeIsHonoured)
{
    std::uint16_t port = 0;
    {
        boost::asio::io_context io;
        boost::asio::ip::tcp::acceptor probe(io, {boost::asio::ip::make_address("127.0.0.1"), 0});
        port = probe.local_endpoint().port();
    }
    const std::string options = json{{"port_min", port}, {"port_max", port}}.dump();
    Fixture f(options.c_str());
    f.start();
    EXPECT_EQ(f.sessionPort(), port);
}

TEST(RemoteSession, OptionsAndCredentialsAreValidated)
{
    Fixture f;
    EXPECT_EQ(lp_provider_set_session_options(f.provider, R"({"bogus":1})"), LP_ERR_INVALID_ARG);
    EXPECT_EQ(lp_provider_set_session_options(f.provider, R"({"port_min":10})"), LP_ERR_INVALID_ARG);
    EXPECT_EQ(lp_provider_set_session_options(f.provider, R"({"max_frame":-1})"), LP_ERR_INVALID_ARG);
    const Identity other("serverAuth");
    // A chain with another leaf's key is refused.
    EXPECT_EQ(lp_provider_set_tls_credential(f.provider, f.server.chainPem().c_str(),
                                             other.keyPem().c_str()), LP_ERR_INVALID_ARG);
    lp_provider* bare = lp_provider_create("bare", R"([{"protocol":"tls_tcp","host":"127.0.0.1"}])");
    ASSERT_NE(bare, nullptr);
    EXPECT_EQ(lp_provider_register(bare, &dispatch, &methods, nullptr, &f.state), LP_ERR_INVALID_ARG);
    lp_provider_destroy(bare);
}

TEST(RemoteSession, TheTransportNameRoundTrips)
{
    LogosTransportSet set;
    std::string error;
    ASSERT_TRUE(logos::parseTransportSet(R"([{"protocol":"tls_tcp","host":"0.0.0.0","port":7450}])",
                                         &set, &error)) << error;
    ASSERT_EQ(set.size(), 1u);
    EXPECT_EQ(set[0].protocol, LogosProtocol::TlsTcp);
    const json round = json::parse(logos::transportSetToJsonString(set));
    EXPECT_EQ(round[0]["protocol"], "tls_tcp");
    EXPECT_EQ(round[0]["port"], 7450);
}

TEST(RemoteSession, AClientThatSaysNothingIsDropped)
{
    Fixture f(R"({"hello_timeout_ms":300})");
    f.start();
    boost::asio::io_context io;
    boost::asio::ssl::context context(boost::asio::ssl::context::tls_client);
    context.use_certificate_chain(boost::asio::buffer(f.client.chainPem()));
    context.use_private_key(boost::asio::buffer(f.client.keyPem()), boost::asio::ssl::context::pem);
    context.set_verify_mode(boost::asio::ssl::verify_none);
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream(io, context);
    boost::asio::ip::tcp::resolver resolver(io);
    boost::asio::connect(stream.lowest_layer(),
                         resolver.resolve("127.0.0.1", std::to_string(f.hooks.port)));
    stream.handshake(boost::asio::ssl::stream_base::client);
    const auto started = std::chrono::steady_clock::now();
    std::array<char, 16> buffer{};
    boost::system::error_code ec;
    boost::asio::read(stream, boost::asio::buffer(buffer), ec);
    EXPECT_TRUE(ec);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(3));
    EXPECT_EQ(f.state.authCount(), 0);
}
