// The ASYNC half of the call-error channel.
//
// lp_result_cb has always been documented as carrying an outcome —
//   "ok != 0 → `json` is the result JSON value; ok == 0 → `json` is the
//    canonical error object"
// — and lp_invoke, its synchronous twin, has always honoured that shape via
// LP_ERR_UNAVAILABLE + out_error_json. lp_invoke_async did not: it subscribed
// with the VALUE-ONLY invokeRemoteMethodAsync overload and called back
// `cb(1, json, user_data)` with ok hard-coded, so a call to a module that
// cannot be acquired reached the callback as a SUCCESS carrying a
// default-constructed value. An error channel that always says "fine" is worse
// than none — it is why logos-cpp-sdk#132 shipped `fooAsyncResult` on the Qt
// surface but deliberately withheld it on the Qt-free (Lp) one.
//
// These two tests are a matched pair and only mean something together:
//
//   1. a genuinely failing call must reach the callback with ok == 0 and the
//      canonical {code, message, origin} object — the case that used to lie;
//   2. a genuinely succeeding call must still reach it with ok == 1 and its
//      value — the control that an over-eager "report failure everywhere" fix
//      would break.
//
// Both run against a REAL transport (plain TCP), not the mock: (1) dials a
// port nothing listens on, (2) dials a live in-process PlainTransportHost
// publishing a real provider through ModuleProxy.

#include <gtest/gtest.h>

#include "logos_protocol.h"

#include "logos_provider_interface.h"
#include "logos_transport_config.h"
#include "module_proxy.h"

#include "plain_transport_host.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QString>
#include <QVariant>
#include <QVariantList>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

using namespace logos::plain;

namespace {

// Minimal live provider: compute() -> 7.
class EchoProvider : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList& args) override
    {
        if (method == QLatin1String("compute")) return QVariant(7);
        if (method == QLatin1String("echo")) return args.value(0);
        return QVariant();
    }
    QJsonArray getMethods() override { return QJsonArray{}; }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("echo"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }
};

QCoreApplication* ensureApp()
{
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance())
        new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

// What lp_result_cb handed us.
struct Capture {
    std::atomic<bool> fired{false};
    int ok = -1;
    std::string json;
};

void captureCb(int ok, const char* json, void* user_data)
{
    auto* c = static_cast<Capture*>(user_data);
    c->ok = ok;
    c->json = json ? json : "";
    c->fired = true;
}

// Pump the owner thread's loop until the callback lands (or we give up).
bool pumpUntilFired(Capture& c, int budgetMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!c.fired && timer.elapsed() < budgetMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return c.fired;
}

} // namespace

class LpInvokeAsyncErrorTest : public ::testing::Test {
protected:
    void SetUp() override { ensureApp(); }
};

// ── 1. The case that used to lie ────────────────────────────────────────────
//
// Plain TCP to a port nothing listens on: the object cannot be acquired, which
// the protocol reports as "object_unavailable" — exactly what lp_invoke puts in
// out_error_json for the same call (test_call_error.cpp pins that half).
// Pre-fix this delivered ok=1 with json "null".
TEST_F(LpInvokeAsyncErrorTest, UnreachableTargetDeliversTheErrorToTheCallback)
{
    const char* deadTarget =
        "{\"protocol\":\"tcp\",\"host\":\"127.0.0.1\",\"port\":9}";

    // Pre-save a token so the capability requestModule handshake is skipped —
    // this exercises transport acquisition only.
    ASSERT_EQ(lp_token_save("async_missing_module", "test-token"), LP_OK);

    lp_client* client = lp_client_create("async_missing_module", "origin",
                                         deadTarget, deadTarget);
    ASSERT_NE(client, nullptr);

    Capture c;
    ASSERT_EQ(lp_invoke_async(client, "anyMethod", "[1,2]", 1500,
                              &captureCb, &c),
              LP_OK);
    ASSERT_TRUE(pumpUntilFired(c, 10000)) << "async callback never fired";

    std::cout << "  FAILING async call -> ok=" << c.ok
              << " json=" << c.json << std::endl;

    // The point of the whole fix.
    EXPECT_EQ(c.ok, 0) << "a failed async call reported success";

    nlohmann::json e = nlohmann::json::parse(c.json, nullptr, false);
    ASSERT_TRUE(e.is_object()) << "ok==0 must carry the canonical error object";
    EXPECT_EQ(e.value("code", std::string{}), "object_unavailable");
    EXPECT_EQ(e.value("origin", std::string{}), "async_missing_module");
    EXPECT_FALSE(e.value("message", std::string{}).empty());

    lp_client_destroy(client);
}

// ── 2. The control ─────────────────────────────────────────────────────────
//
// A live provider over the same real transport must still report ok=1 with its
// value. Without this, "always report failure" would pass test 1.
TEST_F(LpInvokeAsyncErrorTest, LiveTargetStillReportsOkWithItsValue)
{
    LogosTransportConfig cfg;
    cfg.protocol = LogosProtocol::Tcp;
    cfg.host = "127.0.0.1";
    cfg.port = 0;   // ephemeral

    auto host = std::make_unique<PlainTransportHost>(cfg);
    ASSERT_TRUE(host->start());

    EchoProvider provider;
    ModuleProxy proxy(&provider);
    // The token the consumer will present (pre-saved below), so ModuleProxy
    // authorizes the call without a capability_module handshake.
    ASSERT_TRUE(proxy.saveToken(QStringLiteral("origin"),
                                QStringLiteral("live-token")));
    ASSERT_TRUE(host->publishObject("async_live_module", &proxy));

    const QString endpoint = host->endpoint();
    ASSERT_TRUE(endpoint.startsWith("tcp://"));
    const uint16_t port =
        endpoint.mid(endpoint.lastIndexOf(':') + 1).toUShort();
    ASSERT_NE(port, 0);

    const std::string liveTarget =
        "{\"protocol\":\"tcp\",\"host\":\"127.0.0.1\",\"port\":"
        + std::to_string(port) + "}";

    ASSERT_EQ(lp_token_save("async_live_module", "live-token"), LP_OK);

    lp_client* client = lp_client_create("async_live_module", "origin",
                                         liveTarget.c_str(),
                                         liveTarget.c_str());
    ASSERT_NE(client, nullptr);

    Capture c;
    ASSERT_EQ(lp_invoke_async(client, "compute", "[]", 5000, &captureCb, &c),
              LP_OK);
    ASSERT_TRUE(pumpUntilFired(c, 10000)) << "async callback never fired";

    std::cout << "  SUCCEEDING async call -> ok=" << c.ok
              << " json=" << c.json << std::endl;

    EXPECT_EQ(c.ok, 1) << "a successful async call reported failure";
    nlohmann::json v = nlohmann::json::parse(c.json, nullptr, false);
    ASSERT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.get<double>(), 7.0);

    lp_client_destroy(client);
    // Let the deferred client teardown run before the host goes away.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    host.reset();
}
