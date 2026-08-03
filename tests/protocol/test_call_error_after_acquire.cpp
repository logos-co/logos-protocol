// The call-error channel AFTER the target has been acquired.
//
// logos-protocol#40 made lp_invoke_async able to report a failure at all, but
// only for the two conditions the layers ABOVE the transport produce: acquire
// failure ("object_unavailable") and the unauthorized sentinel. Everything the
// transport itself learns while the call is in flight was still discarded:
//
//   * PlainLogosObject::callMethod / callMethodAsync answer a bare QVariant()
//     for BOTH `future timed out` and `ResultMessage.ok == false` — throwing
//     away res.err / res.errCode, which the wire already carries;
//   * LogosAPIConsumer::invokeRemoteMethodAsync then hard-coded an empty
//     logos::CallError next to that value.
//
// So once acquire succeeded, both entry points reported success no matter what
// happened. Two conditions in particular are ordinary, not exotic:
//
//   * a TIMEOUT — the caller's own deadline elapsed and nothing came back;
//   * MODULE NOT LOADED against a LIVE host. PlainTransportConnection::
//     requestObject never checks publication (it just constructs a handle over
//     the open connection), so "the module isn't there" is NOT an acquire
//     failure on this transport — it is a MODULE_NOT_LOADED ResultMessage at
//     call time, and #40's object_unavailable never fires for it.
//
// These tests are a matched set and only mean something together: the two
// failures must report ok=0 / LP_ERR_UNAVAILABLE with a canonical
// {code,message,origin} object, and the successful control must still report
// ok=1 with its value — a fix that reported failure everywhere would satisfy
// the first half and break the second.
//
// Sync and async are BOTH covered because both had the identical hole: an
// async-only fix would leave lp_invoke lying while lp_invoke_async told the
// truth, which is the opposite of the parity #40 set out to establish.
//
// Everything runs against a real transport (plain TCP) and a live in-process
// PlainTransportHost, never the mock.

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
#include <QThread>
#include <QVariant>
#include <QVariantList>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace logos::plain;

namespace {

// A provider faithful to the real ones: compute() answers 7, slow() blocks
// past any sane deadline, and an UNKNOWN method answers a bare QVariant() —
// which is exactly what logos-qt-sdk's QtProviderObject and every generated
// provider dispatch do for a name they don't recognise.
class SlowProvider : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList& args) override
    {
        if (method == QLatin1String("compute")) return QVariant(7);
        if (method == QLatin1String("echo")) return args.value(0);
        if (method == QLatin1String("slow")) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3000));
            return QVariant(1);
        }
        return QVariant();   // unknown method — indistinguishable from null
    }
    QJsonArray getMethods() override { return QJsonArray{}; }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("slow"); }
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

bool pumpUntilFired(Capture& c, int budgetMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!c.fired && timer.elapsed() < budgetMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return c.fired;
}

// A live host publishing `slow_module` through a ModuleProxy that lives on its
// OWN thread. The worker thread matters: PlainTransportHost::onCall dispatches
// to the proxy's thread, so a provider that sleeps would otherwise block the
// very event loop the consumer needs to deliver its own callback, and the test
// would measure the harness rather than the protocol.
class LiveHost {
public:
    LiveHost()
    {
        LogosTransportConfig cfg;
        cfg.protocol = LogosProtocol::Tcp;
        cfg.host = "127.0.0.1";
        cfg.port = 0;   // ephemeral
        m_host = std::make_unique<PlainTransportHost>(cfg);
        m_started = m_host->start();

        m_proxy = new ModuleProxy(&m_provider);
        m_proxy->saveToken(QStringLiteral("origin"), QStringLiteral("live-token"));
        m_thread = new QThread;
        m_proxy->moveToThread(m_thread);
        m_thread->start();

        m_published = m_host->publishObject("slow_module", m_proxy);

        const QString endpoint = m_host->endpoint();
        m_port = endpoint.mid(endpoint.lastIndexOf(':') + 1).toUShort();
    }

    ~LiveHost()
    {
        // Order matters: tear the host down FIRST so no inbound frame can be
        // dispatched to the proxy while we are dismantling it, then stop the
        // proxy's thread, then delete the proxy it was serving.
        m_host.reset();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        m_thread->quit();
        m_thread->wait();
        delete m_proxy;
        delete m_thread;
    }

    bool ok() const { return m_started && m_published && m_port != 0; }

    std::string target() const
    {
        return "{\"protocol\":\"tcp\",\"host\":\"127.0.0.1\",\"port\":"
             + std::to_string(m_port) + "}";
    }

private:
    SlowProvider m_provider;
    std::unique_ptr<PlainTransportHost> m_host;
    ModuleProxy* m_proxy = nullptr;
    QThread* m_thread = nullptr;
    bool m_started = false;
    bool m_published = false;
    uint16_t m_port = 0;
};

// Create an lp_client for `module` at `endpoint`, with its token pre-saved so
// the capability_module handshake is skipped and only the call path is under
// test.
lp_client* clientFor(const char* module, const std::string& endpoint)
{
    lp_token_save(module, "live-token");
    return lp_client_create(module, "origin", endpoint.c_str(), endpoint.c_str());
}

void destroyClient(lp_client* c)
{
    lp_client_destroy(c);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}

} // namespace

class CallErrorAfterAcquireTest : public ::testing::Test {
protected:
    void SetUp() override { ensureApp(); }
};

// ── control: a successful async call still reports its value ────────────────
TEST_F(CallErrorAfterAcquireTest, AsyncSuccessStillReportsTheValue)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());

    lp_client* client = clientFor("slow_module", host.target());
    ASSERT_NE(client, nullptr);

    Capture c;
    ASSERT_EQ(lp_invoke_async(client, "compute", "[]", 5000, &captureCb, &c), LP_OK);
    ASSERT_TRUE(pumpUntilFired(c, 15000)) << "async callback never fired";

    std::cout << "  ASYNC success        -> ok=" << c.ok << " json=" << c.json << std::endl;

    EXPECT_EQ(c.ok, 1) << "a successful async call reported failure";
    nlohmann::json v = nlohmann::json::parse(c.json, nullptr, false);
    ASSERT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.get<double>(), 7.0);

    destroyClient(client);
}

// ── the caller's deadline elapsed ───────────────────────────────────────────
//
// The provider sleeps 3s; the call is given 600ms. Pre-fix this delivered
// ok=1 with json "null" — a timeout dressed up as a provider that returned
// nothing.
TEST_F(CallErrorAfterAcquireTest, AsyncTimeoutReportsTheError)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());

    lp_client* client = clientFor("slow_module", host.target());
    ASSERT_NE(client, nullptr);

    Capture c;
    ASSERT_EQ(lp_invoke_async(client, "slow", "[]", 600, &captureCb, &c), LP_OK);
    ASSERT_TRUE(pumpUntilFired(c, 15000)) << "async callback never fired";

    std::cout << "  ASYNC timeout        -> ok=" << c.ok << " json=" << c.json << std::endl;

    EXPECT_EQ(c.ok, 0) << "a timed-out async call reported success";
    nlohmann::json e = nlohmann::json::parse(c.json, nullptr, false);
    ASSERT_TRUE(e.is_object()) << "ok==0 must carry the canonical error object";
    EXPECT_EQ(e.value("code", std::string{}), "timeout");
    EXPECT_EQ(e.value("origin", std::string{}), "slow_module");
    EXPECT_FALSE(e.value("message", std::string{}).empty());

    destroyClient(client);
    // The provider is still sleeping; let it drain before the host goes away.
    QElapsedTimer t; t.start();
    while (t.elapsed() < 3500) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}

// ── the module is not loaded, on a host that IS up ──────────────────────────
//
// The single most common real failure, and the one #40's release notes claim
// to have fixed. It does NOT go through the acquire path on this transport:
// PlainTransportConnection::requestObject hands back a handle for any name over
// an open connection, so the failure surfaces as a MODULE_NOT_LOADED
// ResultMessage at call time — which was discarded.
TEST_F(CallErrorAfterAcquireTest, AsyncModuleNotLoadedOnALiveHostReportsTheError)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());

    lp_client* client = clientFor("ghost_module", host.target());
    ASSERT_NE(client, nullptr);

    Capture c;
    ASSERT_EQ(lp_invoke_async(client, "compute", "[]", 5000, &captureCb, &c), LP_OK);
    ASSERT_TRUE(pumpUntilFired(c, 15000)) << "async callback never fired";

    std::cout << "  ASYNC not-published  -> ok=" << c.ok << " json=" << c.json << std::endl;

    EXPECT_EQ(c.ok, 0) << "a call to an unpublished module reported success";
    nlohmann::json e = nlohmann::json::parse(c.json, nullptr, false);
    ASSERT_TRUE(e.is_object());
    EXPECT_EQ(e.value("code", std::string{}), "object_unavailable");
    EXPECT_EQ(e.value("origin", std::string{}), "ghost_module");

    destroyClient(client);
}

// ── the synchronous twin had the identical hole ─────────────────────────────
TEST_F(CallErrorAfterAcquireTest, SyncSuccessStillReportsTheValue)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());

    lp_client* client = clientFor("slow_module", host.target());
    ASSERT_NE(client, nullptr);

    char* result = nullptr;
    char* error = nullptr;
    const int rc = lp_invoke(client, "compute", "[]", 5000, &result, &error);

    std::cout << "  SYNC  success        -> rc=" << rc
              << " result=" << (result ? result : "(null)")
              << " error=" << (error ? error : "(null)") << std::endl;

    EXPECT_EQ(rc, LP_OK);
    ASSERT_NE(result, nullptr);
    EXPECT_STREQ(result, "7");
    EXPECT_EQ(error, nullptr);

    lp_string_free(result);
    lp_string_free(error);
    destroyClient(client);
}

TEST_F(CallErrorAfterAcquireTest, SyncTimeoutReportsTheError)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());

    lp_client* client = clientFor("slow_module", host.target());
    ASSERT_NE(client, nullptr);

    char* result = nullptr;
    char* error = nullptr;
    const int rc = lp_invoke(client, "slow", "[]", 600, &result, &error);

    std::cout << "  SYNC  timeout        -> rc=" << rc
              << " result=" << (result ? result : "(null)")
              << " error=" << (error ? error : "(null)") << std::endl;

    EXPECT_EQ(rc, LP_ERR_UNAVAILABLE) << "a timed-out sync call reported success";
    ASSERT_NE(error, nullptr) << "LP_ERR_UNAVAILABLE must carry the error object";
    nlohmann::json e = nlohmann::json::parse(error, nullptr, false);
    ASSERT_TRUE(e.is_object());
    EXPECT_EQ(e.value("code", std::string{}), "timeout");

    lp_string_free(result);
    lp_string_free(error);
    destroyClient(client);
    QElapsedTimer t; t.start();
    while (t.elapsed() < 3500) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}

TEST_F(CallErrorAfterAcquireTest, SyncModuleNotLoadedOnALiveHostReportsTheError)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());

    lp_client* client = clientFor("ghost_module", host.target());
    ASSERT_NE(client, nullptr);

    char* result = nullptr;
    char* error = nullptr;
    const int rc = lp_invoke(client, "compute", "[]", 5000, &result, &error);

    std::cout << "  SYNC  not-published  -> rc=" << rc
              << " result=" << (result ? result : "(null)")
              << " error=" << (error ? error : "(null)") << std::endl;

    EXPECT_EQ(rc, LP_ERR_UNAVAILABLE);
    ASSERT_NE(error, nullptr);
    nlohmann::json e = nlohmann::json::parse(error, nullptr, false);
    ASSERT_TRUE(e.is_object());
    EXPECT_EQ(e.value("code", std::string{}), "object_unavailable");

    lp_string_free(result);
    lp_string_free(error);
    destroyClient(client);
}

// ── the residual gap, pinned deliberately ───────────────────────────────────
//
// An UNKNOWN METHOD is NOT fixed here and cannot be at this layer: every
// provider flavour answers a bare null for a name it doesn't recognise
// (logos-qt-sdk QtProviderObject's `return QVariant()`, the generated Qt and
// cdylib dispatches' `unknown method` fall-through, the Rust provider's), which
// is byte-identical to a method that legitimately returns null. The transport
// sees ok=true with a null value and MUST report success — reporting failure
// would break every method whose return really is null. Closing it needs the
// PROVIDER contract to answer a rejection object for an unknown name, in every
// SDK, and mirrored into lp_invoke so the twins stay identical.
//
// This test asserts today's behaviour so the boundary is explicit rather than
// assumed, and so it fails loudly if a provider ever starts distinguishing.
TEST_F(CallErrorAfterAcquireTest, UnknownMethodStaysIndistinguishableFromANullReturn)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());

    lp_client* client = clientFor("slow_module", host.target());
    ASSERT_NE(client, nullptr);

    Capture c;
    ASSERT_EQ(lp_invoke_async(client, "noSuchMethod", "[]", 5000, &captureCb, &c), LP_OK);
    ASSERT_TRUE(pumpUntilFired(c, 15000)) << "async callback never fired";

    std::cout << "  ASYNC unknown method -> ok=" << c.ok << " json=" << c.json
              << "   (residual gap: the provider itself answers a bare null)"
              << std::endl;

    EXPECT_EQ(c.ok, 1);
    EXPECT_EQ(c.json, "null");

    destroyClient(client);
}
