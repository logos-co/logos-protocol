// Tests for rejection-driven token re-exchange.
//
// Design intent (verified gap being fixed): the consumer<->provider token
// exchange (capability_module.requestModule) is fire-once and cached. When a
// provider REJECTS a call because the presented token is stale/unrecognized,
// the consumer must drop the cached token, re-run requestModule, and retry the
// call once — instead of silently reusing the dead token forever.
//
// Two-part fix, tested here:
//   * Provider side (ModuleProxy): an unauthorized call now returns the
//     structured sentinel (logos_rpc_status.h) instead of a bare QVariant().
//     Tested directly at the ModuleProxy layer (ProviderReturnsSentinel*).
//   * Consumer side (LogosAPIClient): on the sentinel it removeToken()s,
//     re-mints via requestModule, and retries once — bounded (no loop), never
//     firing on a legitimately-empty result, and never surfacing the sentinel
//     to the caller. Tested end-to-end over the qt_remote transport, with the
//     target provider EMITTING the sentinel to simulate a stale-token rejection
//     (a single-process test can't use real auth rejection because ModuleProxy
//     and the client share the one process-global TokenManager).
//
// Backward-compat is covered by construction: an OLD provider returns a bare
// QVariant() (never the sentinel) so a NEW consumer never re-exchanges against
// it; an OLD consumer converts the sentinel identically to QVariant() for
// scalar/string/LogosResult returns. The EmptyResult* test guards the
// consumer's "only on the explicit sentinel" invariant.

#include <gtest/gtest.h>

#include "local_host.h"
#include "logos_api_client.h"
#include "logos_instance.h"
#include "logos_provider_interface.h"
#include "logos_rpc_status.h"
#include "module_proxy.h"
#include "remote_transport.h"
#include "token_manager.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QString>
#include <QUuid>
#include <QVariantList>

#include <atomic>
#include <chrono>
#include <thread>

namespace {

QCoreApplication* ensureApp() {
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance())
        new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

// Always returns "ok" for ping — used for the ModuleProxy-level auth tests.
class PingProvider : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList&) override {
        if (method == QLatin1String("ping")) return QStringLiteral("ok");
        return QVariant();
    }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    QJsonArray getMethods() override { return QJsonArray{}; }
    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("target_module"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }
};

// Configurable target. Counts business calls and can emit the rejection sentinel
// to simulate the provider rejecting a stale token.
class SimTargetProvider : public LogosProviderObject {
public:
    enum Mode { RejectOnceThenOk, AlwaysReject, AlwaysEmpty };
    explicit SimTargetProvider(Mode m) : m_mode(m) {}

    QVariant callMethod(const QString& method, const QVariantList&) override {
        const int n = m_calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (method != QLatin1String("ping")) return QVariant();
        switch (m_mode) {
        case RejectOnceThenOk:
            return n == 1 ? logos::makeUnauthorizedSentinel()
                          : QVariant(QStringLiteral("ok"));
        case AlwaysReject:
            return logos::makeUnauthorizedSentinel();
        case AlwaysEmpty:
            return QVariant();
        }
        return QVariant();
    }
    bool informModuleToken(const QString& moduleName, const QString& token) override {
        if (m_proxy) m_proxy->saveToken(moduleName, token);
        return true;
    }
    QJsonArray getMethods() override { return QJsonArray{}; }
    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("target_module"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }

    void bindProxy(ModuleProxy* p) { m_proxy = p; }
    int calls() const { return m_calls.load(std::memory_order_relaxed); }
private:
    Mode m_mode;
    ModuleProxy* m_proxy = nullptr;
    std::atomic<int> m_calls{0};
};

// Mints a fresh token per requestModule and informs the target (the real
// capability behaviour), counting mints so a re-exchange is observable.
class CapabilityProvider : public LogosProviderObject {
public:
    void bindTarget(ModuleProxy* targetProxy) { m_targetProxy = targetProxy; }
    QVariant callMethod(const QString& method, const QVariantList& args) override {
        if (method == QLatin1String("requestModule") && args.size() == 2) {
            const QString from = args.value(0).toString();
            const QString tok  = QUuid::createUuid().toString(QUuid::WithoutBraces);
            if (m_targetProxy) m_targetProxy->saveToken(from, tok);
            m_mintCount.fetch_add(1, std::memory_order_relaxed);
            return tok;
        }
        return QVariant();
    }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    QJsonArray getMethods() override { return QJsonArray{}; }
    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("capability_module"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }
    int mintCount() const { return m_mintCount.load(std::memory_order_relaxed); }
private:
    ModuleProxy* m_targetProxy = nullptr;
    std::atomic<int> m_mintCount{0};
};

} // namespace

class TokenReexchangeTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensureApp();
        TokenManager::instance().clearAllTokens();
    }
    void TearDown() override {
        TokenManager::instance().clearAllTokens();
    }
    void pumpEventLoop(int ms) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    // Stand up capability_module + target_module over qt_remote and a connected
    // client, mirroring test_token_cache.cpp's harness.
    struct Rig {
        RemoteTransportHost capHost;
        RemoteTransportHost targetHost;
        ModuleProxy capProxy;
        ModuleProxy targetProxy;
        CapabilityProvider capProvider;
    };
};

// ── Provider side: unauthorized call returns the structured sentinel ──────────
TEST_F(TokenReexchangeTest, ProviderReturnsSentinelOnUnauthorizedCall)
{
    PingProvider provider;
    ModuleProxy proxy(&provider);

    // No token issued + global store cleared in SetUp -> any token is rejected.
    const QVariant rejected = proxy.callRemoteMethod(
        QStringLiteral("bogus-token"), QStringLiteral("ping"), QVariantList{});
    EXPECT_TRUE(logos::isUnauthorizedSentinel(rejected))
        << "an unauthorized call must return the structured rejection sentinel";

    // An authorized call returns the real result, not a sentinel.
    ASSERT_TRUE(proxy.saveToken(QStringLiteral("caller"), QStringLiteral("good-token")));
    const QVariant ok = proxy.callRemoteMethod(
        QStringLiteral("good-token"), QStringLiteral("ping"), QVariantList{});
    EXPECT_FALSE(logos::isUnauthorizedSentinel(ok));
    EXPECT_EQ(ok.toString(), QStringLiteral("ok"));

    // Ungated introspection is unaffected (no sentinel even without a token).
    const QVariant methods = proxy.callRemoteMethod(
        QString(), QStringLiteral("getPluginMethods"), QVariantList{});
    EXPECT_FALSE(logos::isUnauthorizedSentinel(methods));
}

// Backward-compat: an OLD consumer converts the sentinel exactly like a bare
// QVariant() for scalar/string returns, so it sees today's empty/failed result.
TEST_F(TokenReexchangeTest, SentinelDecodesLikeEmptyForOldConsumers)
{
    const QVariant s = logos::makeUnauthorizedSentinel();
    EXPECT_EQ(s.toString(), QString());      // QVariant().toString() == ""
    EXPECT_EQ(s.toInt(), 0);                 // QVariant().toInt()    == 0
    EXPECT_EQ(qvariant_cast<QString>(s), QString());
    EXPECT_FALSE(logos::isUnauthorizedSentinel(QVariant()));            // empty != sentinel
    EXPECT_FALSE(logos::isUnauthorizedSentinel(QVariant(QStringLiteral("ok"))));
    QVariantMap other; other.insert(QStringLiteral("x"), 1);
    EXPECT_FALSE(logos::isUnauthorizedSentinel(other));                 // unrelated map != sentinel
}

namespace {
// Common rig wiring for the client-level integration tests.
void wireRig(LogosTransportHost& capHost, LogosTransportHost& targetHost,
             ModuleProxy& capProxy, ModuleProxy& targetProxy,
             CapabilityProvider& capProvider, SimTargetProvider& targetProvider,
             const QString& bootstrapToken)
{
    targetProvider.bindProxy(&targetProxy);
    capProvider.bindTarget(&targetProxy);

    TokenManager::instance().saveToken(QStringLiteral("capability_module"), bootstrapToken);
    ASSERT_TRUE(capProxy.saveToken(QStringLiteral("test_origin"), bootstrapToken));

    ASSERT_TRUE(capHost.publishObject("capability_module", &capProxy));
    ASSERT_TRUE(targetHost.publishObject("target_module", &targetProxy));
}
} // namespace

// ── Consumer side: a rejection triggers exactly one re-exchange + retry ───────
TEST_F(TokenReexchangeTest, SyncRejectionReexchangesAndRetriesOnce)
{
    auto capHost = makeLocalHost(LogosInstance::id("capability_module"));
    auto targetHost = makeLocalHost(LogosInstance::id("target_module"));

    SimTargetProvider targetProvider(SimTargetProvider::RejectOnceThenOk);
    ModuleProxy       targetProxy(&targetProvider);
    CapabilityProvider capProvider;
    ModuleProxy        capProxy(&capProvider);

    wireRig(*capHost, *targetHost, capProxy, targetProxy, capProvider, targetProvider,
            QStringLiteral("bootstrap-tok-sync"));

    LogosAPIClient client(QStringLiteral("target_module"), QStringLiteral("test_origin"),
                          &TokenManager::instance());
    for (int i = 0; i < 100 && !client.isConnected(); ++i) pumpEventLoop(20);
    ASSERT_TRUE(client.isConnected());

    const QVariant r = client.invokeRemoteMethod(
        QStringLiteral("target_module"), QStringLiteral("ping"), QVariantList{});

    EXPECT_EQ(r.toString(), QStringLiteral("ok"))
        << "the retry after re-exchange should have succeeded";
    EXPECT_EQ(targetProvider.calls(), 2)
        << "provider should be hit twice: rejected call + retried call";
    EXPECT_EQ(capProvider.mintCount(), 2)
        << "exactly one initial exchange + one re-exchange (not more)";
}

// ── Bounded: a provider that always rejects retries once, then gives up ───────
TEST_F(TokenReexchangeTest, SyncPersistentRejectionRetriesOnceThenGivesUp)
{
    auto capHost = makeLocalHost(LogosInstance::id("capability_module"));
    auto targetHost = makeLocalHost(LogosInstance::id("target_module"));

    SimTargetProvider targetProvider(SimTargetProvider::AlwaysReject);
    ModuleProxy       targetProxy(&targetProvider);
    CapabilityProvider capProvider;
    ModuleProxy        capProxy(&capProvider);

    wireRig(*capHost, *targetHost, capProxy, targetProxy, capProvider, targetProvider,
            QStringLiteral("bootstrap-tok-bounded"));

    LogosAPIClient client(QStringLiteral("target_module"), QStringLiteral("test_origin"),
                          &TokenManager::instance());
    for (int i = 0; i < 100 && !client.isConnected(); ++i) pumpEventLoop(20);
    ASSERT_TRUE(client.isConnected());

    const QVariant r = client.invokeRemoteMethod(
        QStringLiteral("target_module"), QStringLiteral("ping"), QVariantList{});

    EXPECT_FALSE(logos::isUnauthorizedSentinel(r))
        << "the sentinel must never be surfaced to the caller";
    EXPECT_FALSE(r.isValid()) << "give-up collapses to today's empty result";
    EXPECT_EQ(targetProvider.calls(), 2)
        << "exactly one retry (2 calls total), not an unbounded loop";
    EXPECT_EQ(capProvider.mintCount(), 2)
        << "initial exchange + exactly one re-exchange attempt";
}

// ── False-positive guard: a legitimately empty result must NOT re-exchange ────
TEST_F(TokenReexchangeTest, SyncEmptyResultDoesNotReexchange)
{
    auto capHost = makeLocalHost(LogosInstance::id("capability_module"));
    auto targetHost = makeLocalHost(LogosInstance::id("target_module"));

    SimTargetProvider targetProvider(SimTargetProvider::AlwaysEmpty);
    ModuleProxy       targetProxy(&targetProvider);
    CapabilityProvider capProvider;
    ModuleProxy        capProxy(&capProvider);

    wireRig(*capHost, *targetHost, capProxy, targetProxy, capProvider, targetProvider,
            QStringLiteral("bootstrap-tok-empty"));

    LogosAPIClient client(QStringLiteral("target_module"), QStringLiteral("test_origin"),
                          &TokenManager::instance());
    for (int i = 0; i < 100 && !client.isConnected(); ++i) pumpEventLoop(20);
    ASSERT_TRUE(client.isConnected());

    const QVariant r = client.invokeRemoteMethod(
        QStringLiteral("target_module"), QStringLiteral("ping"), QVariantList{});

    EXPECT_FALSE(r.isValid());
    EXPECT_EQ(targetProvider.calls(), 1) << "an empty return must not trigger a retry";
    EXPECT_EQ(capProvider.mintCount(), 1)
        << "only the initial exchange — a plain empty result is NOT a rejection";
}

// ── Async path re-exchanges + retries on a rejection ─────────────────────────
TEST_F(TokenReexchangeTest, AsyncRejectionReexchangesAndRetriesOnce)
{
    auto capHost = makeLocalHost(LogosInstance::id("capability_module"));
    auto targetHost = makeLocalHost(LogosInstance::id("target_module"));

    SimTargetProvider targetProvider(SimTargetProvider::RejectOnceThenOk);
    ModuleProxy       targetProxy(&targetProvider);
    CapabilityProvider capProvider;
    ModuleProxy        capProxy(&capProvider);

    wireRig(*capHost, *targetHost, capProxy, targetProxy, capProvider, targetProvider,
            QStringLiteral("bootstrap-tok-async"));

    LogosAPIClient client(QStringLiteral("target_module"), QStringLiteral("test_origin"),
                          &TokenManager::instance());
    for (int i = 0; i < 100 && !client.isConnected(); ++i) pumpEventLoop(20);
    ASSERT_TRUE(client.isConnected());

    std::atomic<int> done{0};
    QString got;
    client.invokeRemoteMethodAsync(
        QStringLiteral("target_module"), QStringLiteral("ping"), QVariantList{},
        [&done, &got](QVariant r) { got = r.toString(); done.fetch_add(1); });

    for (int i = 0; i < 400 && done.load() < 1; ++i) pumpEventLoop(20);
    ASSERT_EQ(done.load(), 1) << "async call never completed";
    EXPECT_EQ(got, QStringLiteral("ok")) << "retry after re-exchange should succeed";
    EXPECT_EQ(targetProvider.calls(), 2);
    EXPECT_EQ(capProvider.mintCount(), 2);
}
