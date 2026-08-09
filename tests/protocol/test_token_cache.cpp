// Regression test for the IPC token-rotation race observed on Linux when
// storage_ui made back-to-back sync invokeRemoteMethod calls.
//
// Bug: LogosAPIClient::invokeRemoteMethod minted a fresh capability token on
// every cache miss but never wrote the result back to its TokenManager. On
// Linux, QtRO's sync waitForFinished spins a nested QEventLoop that dispatches
// other queued slots mid-wait — so each back-to-back sync call fired its own
// requestModule, minted a new token, and informed the target. The target
// stores ONE token per caller (ModuleProxy::saveToken replaces) so the latest
// inform invalidated the earlier in-flight call's token and the target rejected
// the call with "rejecting unauthorized call to <method> - auth token not
// recognized".
//
// Fix: LogosAPIClient now writes the minted token into its TokenManager
// immediately after requestModule returns (on both the sync and async paths),
// so every subsequent call short-circuits the handshake.
//
// What this test asserts (the fix's invariant):
//   * After N sync calls to the same target, capability_module.requestModule
//     is invoked exactly ONCE — not N times.
//   * After two back-to-back async bursts to the same target, the SECOND burst
//     reuses the cached token without re-minting (the existing
//     m_pendingHandshakes coalescer only collapses the FIRST burst).
//   * Every call returns a valid result (i.e. no call gets rejected as
//     unauthorized because the token was rotated under it).
//
// Without the fix the sync test fails with mintCount == N, and the async test
// fails with mintCount == 2 (one per burst).

#include <gtest/gtest.h>

#include "logos_api_client.h"
#include "logos_instance.h"
#include "logos_provider_interface.h"
#include "logos_transport_config.h"
#include "module_proxy.h"
#include "remote_transport.h"
#include "token_manager.h"

#include <QCoreApplication>
#include <QElapsedTimer>
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

class PingProvider : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList&) override {
        if (method == QLatin1String("ping")) return QStringLiteral("ok");
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
private:
    ModuleProxy* m_proxy = nullptr;
};

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

class TokenCacheTest : public ::testing::Test {
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
};

// N sync calls to the same un-tokened target ⇒ exactly ONE requestModule.
// Pre-fix this fails with mintCount == N (and may also reject one of the
// calls when the rotation race actually fires). Post-fix mintCount == 1
// and all N calls succeed.
TEST_F(TokenCacheTest, SyncCallsToSameTargetHandshakeOnce)
{
    RemoteTransportHost capHost(LogosInstance::id("capability_module"));
    RemoteTransportHost targetHost(LogosInstance::id("target_module"));

    PingProvider targetProvider;
    ModuleProxy  targetProxy(&targetProvider);
    targetProvider.bindProxy(&targetProxy);

    CapabilityProvider capProvider;
    ModuleProxy        capProxy(&capProvider);
    capProvider.bindTarget(&targetProxy);

    const QString bootstrapToken = QStringLiteral("bootstrap-tok-sync");
    TokenManager::instance().saveToken(QStringLiteral("capability_module"), bootstrapToken);
    ASSERT_TRUE(capProxy.saveToken(QStringLiteral("test_origin"), bootstrapToken));

    ASSERT_TRUE(capHost.publishObject("capability_module", &capProxy));
    ASSERT_TRUE(targetHost.publishObject("target_module", &targetProxy));

    LogosAPIClient client(QStringLiteral("target_module"),
                          QStringLiteral("test_origin"),
                          &TokenManager::instance());

    for (int i = 0; i < 100 && !client.isConnected(); ++i) pumpEventLoop(20);
    ASSERT_TRUE(client.isConnected());

    constexpr int N = 5;
    for (int i = 0; i < N; ++i) {
        QVariant r = client.invokeRemoteMethod(QStringLiteral("target_module"),
                                               QStringLiteral("ping"),
                                               QVariantList{});
        EXPECT_TRUE(r.isValid()) << "call #" << i << " returned invalid (auth rejected?)";
        EXPECT_EQ(r.toString(), QStringLiteral("ok"));
    }

    EXPECT_EQ(capProvider.mintCount(), 1)
        << "expected exactly one requestModule handshake per (client, target) "
           "pair; got " << capProvider.mintCount() << ". Without client-side "
           "token caching, every sync call re-mints — the token-rotation race "
           "we are fixing.";
}

// Same invariant on the async path. Pre-PR#5 (the coalescer) this would also
// fan out N handshakes; even with PR#5, only the FIRST burst is coalesced, so
// a second burst (after the queue drains) would re-mint without the cache.
// Post-fix: the cache is populated in the requestModule callback before the
// drain, so the SECOND burst also short-circuits — exactly one mint total.
TEST_F(TokenCacheTest, AsyncCallsToSameTargetHandshakeOnceAcrossBursts)
{
    RemoteTransportHost capHost(LogosInstance::id("capability_module"));
    RemoteTransportHost targetHost(LogosInstance::id("target_module"));

    PingProvider targetProvider;
    ModuleProxy  targetProxy(&targetProvider);
    targetProvider.bindProxy(&targetProxy);

    CapabilityProvider capProvider;
    ModuleProxy        capProxy(&capProvider);
    capProvider.bindTarget(&targetProxy);

    const QString bootstrapToken = QStringLiteral("bootstrap-tok-async");
    TokenManager::instance().saveToken(QStringLiteral("capability_module"), bootstrapToken);
    ASSERT_TRUE(capProxy.saveToken(QStringLiteral("test_origin"), bootstrapToken));

    ASSERT_TRUE(capHost.publishObject("capability_module", &capProxy));
    ASSERT_TRUE(targetHost.publishObject("target_module", &targetProxy));

    LogosAPIClient client(QStringLiteral("target_module"),
                          QStringLiteral("test_origin"),
                          &TokenManager::instance());

    for (int i = 0; i < 100 && !client.isConnected(); ++i) pumpEventLoop(20);
    ASSERT_TRUE(client.isConnected());

    auto fireBurst = [&](int n) {
        std::atomic<int> done{0};
        std::atomic<int> ok{0};
        for (int i = 0; i < n; ++i) {
            client.invokeRemoteMethodAsync(
                QStringLiteral("target_module"),
                QStringLiteral("ping"),
                QVariantList{},
                [&done, &ok](QVariant r) {
                    if (r.isValid() && r.toString() == QStringLiteral("ok"))
                        ok.fetch_add(1);
                    done.fetch_add(1);
                });
        }
        for (int i = 0; i < 400 && done.load() < n; ++i) pumpEventLoop(20);
        EXPECT_EQ(done.load(), n) << "not all async calls completed";
        EXPECT_EQ(ok.load(),   n) << "some async calls were rejected unauthorized";
    };

    fireBurst(4); // first burst — coalesced by m_pendingHandshakes
    fireBurst(4); // SECOND burst — relies on the cache, not the coalescer

    EXPECT_EQ(capProvider.mintCount(), 1)
        << "expected one handshake total across both bursts; got "
        << capProvider.mintCount() << ". The first burst is coalesced by "
           "m_pendingHandshakes; the second burst should reuse the cached "
           "token written in the requestModule callback.";
}

// The caller's budget must bound the TOKEN EXCHANGE, not only the call.
//
// LogosAPIClient::invokeRemoteMethod takes a Timeout, but on an un-tokened
// target the capability handshake runs FIRST — and it used to hardcode 20 s
// twice (the capability_module acquire and the requestModule call on it). So a
// caller asking for 1500 ms could block on the order of 40 s before the part it
// had actually bounded even began. logos-view-module-runtime's callModule
// documented a 1500 ms bound on exactly this path; that bound was not real.
//
// capability_module is deliberately NEVER PUBLISHED here, so the acquire runs
// out its budget instead of succeeding. That is the whole point: this measures
// the timeout path, which is the one that was wrong. Every other test in this
// file publishes it and therefore never exercises the wait at all — which is
// how a hardcoded 20 s survived alongside them.
//
// TWO-SIDED on purpose. The upper bound alone would pass if something else made
// the acquire return instantly, leaving the hardcoded 20 s in place and the test
// green for the wrong reason:
//
//   * >= budget  proves the acquire really did wait, i.e. the timeout path ran;
//   * <  4×budget proves it waited for the CALLER's budget rather than the 20 s
//     default. Pre-fix this side fails at ~20 s.
TEST_F(TokenCacheTest, UnTokenedCallBoundsTheHandshakeByTheCallersBudget)
{
    RemoteTransportHost targetHost(LogosInstance::id("target_module"));

    PingProvider targetProvider;
    ModuleProxy  targetProxy(&targetProvider);
    targetProvider.bindProxy(&targetProxy);
    ASSERT_TRUE(targetHost.publishObject("target_module", &targetProxy));

    // A capability token exists, so the client tries the handshake — but there
    // is no capability_module host and no published object to reach.
    TokenManager::instance().saveToken(QStringLiteral("capability_module"),
                                       QStringLiteral("bootstrap-tok-budget"));

    LogosAPIClient client(QStringLiteral("target_module"),
                          QStringLiteral("test_origin"),
                          &TokenManager::instance());

    for (int i = 0; i < 100 && !client.isConnected(); ++i) pumpEventLoop(20);
    ASSERT_TRUE(client.isConnected());

    constexpr int kBudgetMs = 1500;

    QElapsedTimer t;
    t.start();
    // The result is not asserted: with capability_module absent the handshake
    // cannot mint a token, so whether the target accepts the call is a separate
    // question from the one under test. What is asserted is how long being told
    // so takes.
    client.invokeRemoteMethod(QStringLiteral("target_module"),
                              QStringLiteral("ping"),
                              QVariantList{},
                              Timeout(kBudgetMs));
    const qint64 elapsed = t.elapsed();

    // A 200 ms slack below the budget rather than the exact figure: a deadline
    // that expires a few ms early is a scheduling detail, and a knife-edge
    // assertion here would turn this into a flake. Anything that short-circuits
    // the acquire returns in single-digit ms, so the discrimination is intact.
    EXPECT_GE(elapsed, kBudgetMs - 200)
        << "the handshake returned in " << elapsed << "ms without consuming its "
        << kBudgetMs << "ms budget, so the capability acquire never actually "
           "waited. This test cannot say anything about the timeout path unless "
           "that path runs — fix the fixture rather than the bound.";

    EXPECT_LT(elapsed, 4 * kBudgetMs)
        << "an un-tokened call with a " << kBudgetMs << "ms budget took "
        << elapsed << "ms. The capability handshake is ignoring the caller's "
           "budget — it used to hardcode 20000 twice in "
           "LogosAPIConsumer::requestModule, which is ~40s of blocking in front "
           "of a bound the caller believed was 1500ms.";
}
