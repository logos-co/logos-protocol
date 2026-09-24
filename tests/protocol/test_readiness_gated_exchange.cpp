// The token exchange must not ask "is the target there?" on a budget the caller
// never agreed to.
//
// MEASURED in the field before this: a module process is announced loaded by the host
// after fork+exec+token-pipe-write, but its QtRO socket binds LAZILY on its first
// publishObject — warm, 32.8 ms later; on an unlucky cold start, >3 s later. In that
// window capability_module's push found no listener, spent 250 ms probing for the
// handshake surface and 3000 ms acquiring the business object, and returned an empty
// token. The caller's own budget for the same question was 20000 ms.
//
// Two waits for one event, and the SHORT one gated the long one. So the fix is ordering,
// not speed: wait first on the caller's budget, then mint against a target that is
// provably up. These tests pin that order, and the cost of getting it wrong in either
// direction — minting too early (a wasted unauthorized round) or waiting twice (a
// fleet-wide latency regression).

#include <gtest/gtest.h>

#include "local_host.h"
#include "logos_api_client.h"
#include "logos_call_error.h"
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
#include <QTimer>
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

/// Records WHEN it was asked to mint, not only how often. The count alone cannot tell a
/// grant issued against a live target from one issued into a void, and the second is the
/// whole defect.
class CapabilityProvider : public LogosProviderObject {
public:
    void bindTarget(ModuleProxy* targetProxy) { m_targetProxy = targetProxy; }
    void startClock() { m_clock.start(); }

    QVariant callMethod(const QString& method, const QVariantList& args) override {
        if (method == QLatin1String("requestModule") && args.size() == 2) {
            if (m_mintCount.load() == 0) m_firstMintMs = m_clock.elapsed();
            m_mintCount.fetch_add(1, std::memory_order_relaxed);
            // A push to a target that is not there yet answers with an EMPTY token, which
            // is what capability_module does on a failed inform. Without this the stub
            // cannot reproduce the bug.
            if (m_targetPublished && !m_targetPublished())
                return QVariant(QString());
            const QString from = args.value(0).toString();
            const QString tok  = QUuid::createUuid().toString(QUuid::WithoutBraces);
            if (m_targetProxy) m_targetProxy->saveToken(from, tok);
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
    qint64 firstMintMs() const { return m_firstMintMs; }
    std::function<bool()> m_targetPublished;
private:
    ModuleProxy* m_targetProxy = nullptr;
    std::atomic<int> m_mintCount{0};
    QElapsedTimer m_clock;
    qint64 m_firstMintMs = -1;
};

} // namespace

class ReadinessGatedExchangeTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensureApp();
        TokenManager::instance().clearAllTokens();
    }
    void TearDown() override { TokenManager::instance().clearAllTokens(); }

    void pumpEventLoop(int ms) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};

// The assertion that is RED before the reorder: pre-fix the mint lands at ~0 ms, against
// a target that has not published, and comes back empty.
TEST_F(ReadinessGatedExchangeTest, TheExchangeIsNotAttemptedUntilTheTargetHasPublished)
{
    auto capHost = makeLocalHost(LogosInstance::id("capability_module"));
    auto targetHost = makeLocalHost(LogosInstance::id("target_module"));

    PingProvider targetProvider;
    ModuleProxy  targetProxy(&targetProvider);
    targetProvider.bindProxy(&targetProxy);

    CapabilityProvider capProvider;
    ModuleProxy        capProxy(&capProvider);
    capProvider.bindTarget(&targetProxy);

    std::atomic<bool> published{false};
    capProvider.m_targetPublished = [&published] { return published.load(); };

    const QString bootstrap = QStringLiteral("bootstrap-tok-gate");
    TokenManager::instance().saveToken(QStringLiteral("capability_module"), bootstrap);
    ASSERT_TRUE(capProxy.saveToken(QStringLiteral("test_origin"), bootstrap));
    ASSERT_TRUE(capHost->publishObject("capability_module", &capProxy));

    LogosAPIClient client(QStringLiteral("target_module"),
                          QStringLiteral("test_origin"),
                          &TokenManager::instance());
    capProvider.startClock();

    // The target appears DURING the call, which is the only arrangement that exercises the
    // defect: publishing before the call means the mint lands against a live target either
    // way, and the test passes with or without the gate. A sync invoke spins QtRO's nested
    // event loop, so this timer fires inside it.
    // Owned here, so a call that returns early cannot leave it to fire into the next test.
    QObject timerContext;
    QTimer::singleShot(500, &timerContext, [&] {
        published.store(true);
        targetHost->publishObject("target_module", &targetProxy);
    });

    QVariant r = client.invokeRemoteMethod(QStringLiteral("target_module"),
                                           QStringLiteral("ping"), QVariantList{},
                                           Timeout(6000), nullptr);

    EXPECT_EQ(r.toString(), QStringLiteral("ok"));
    EXPECT_EQ(capProvider.mintCount(), 1)
        << "a second mint means the first was spent against a target that was not there, "
           "and the target REPLACES a caller's token — so the second revokes the first";
    EXPECT_GE(capProvider.firstMintMs(), 400)
        << "the exchange ran at " << capProvider.firstMintMs()
        << " ms, i.e. before the target published. That is the defect: the grant is spent "
           "on a question the caller was about to ask again on its own, longer budget.";
}

// The fatal flaw every refuter found in the naive version: gate, fail, then fall through
// and pay the same budget again. The gate must RETURN.
TEST_F(ReadinessGatedExchangeTest, AFirstCallToAnAbsentTargetPaysTheAcquireBudgetOnlyOnce)
{
    auto capHost = makeLocalHost(LogosInstance::id("capability_module"));

    CapabilityProvider capProvider;
    ModuleProxy        capProxy(&capProvider);

    const QString bootstrap = QStringLiteral("bootstrap-tok-absent");
    TokenManager::instance().saveToken(QStringLiteral("capability_module"), bootstrap);
    ASSERT_TRUE(capProxy.saveToken(QStringLiteral("test_origin"), bootstrap));
    ASSERT_TRUE(capHost->publishObject("capability_module", &capProxy));

    LogosAPIClient client(QStringLiteral("target_module"),
                          QStringLiteral("test_origin"),
                          &TokenManager::instance());

    logos::CallError err;
    QElapsedTimer t; t.start();
    QVariant r = client.invokeRemoteMethod(QStringLiteral("target_module"),
                                           QStringLiteral("ping"), QVariantList{},
                                           Timeout(600), &err);
    const qint64 spent = t.elapsed();

    EXPECT_FALSE(r.isValid());
    EXPECT_EQ(err.code, "object_unavailable");
    EXPECT_LT(spent, 1400)
        << "spent " << spent << " ms on a 600 ms budget: the gate failed and the call then "
           "paid the same budget over again. Falling through is what turns this fix into a "
           "fleet-wide latency regression.";
}

// The control. Without it a green suite cannot tell the fix from a mis-wired fixture.
TEST_F(ReadinessGatedExchangeTest, ACallToATargetThatIsAlreadyUpIsNotDelayedByTheGate)
{
    auto capHost = makeLocalHost(LogosInstance::id("capability_module"));
    auto targetHost = makeLocalHost(LogosInstance::id("target_module"));

    PingProvider targetProvider;
    ModuleProxy  targetProxy(&targetProvider);
    targetProvider.bindProxy(&targetProxy);

    CapabilityProvider capProvider;
    ModuleProxy        capProxy(&capProvider);
    capProvider.bindTarget(&targetProxy);

    const QString bootstrap = QStringLiteral("bootstrap-tok-warm");
    TokenManager::instance().saveToken(QStringLiteral("capability_module"), bootstrap);
    ASSERT_TRUE(capProxy.saveToken(QStringLiteral("test_origin"), bootstrap));
    ASSERT_TRUE(capHost->publishObject("capability_module", &capProxy));
    ASSERT_TRUE(targetHost->publishObject("target_module", &targetProxy));

    LogosAPIClient client(QStringLiteral("target_module"),
                          QStringLiteral("test_origin"),
                          &TokenManager::instance());
    for (int i = 0; i < 100 && !client.isConnected(); ++i) pumpEventLoop(20);
    ASSERT_TRUE(client.isConnected());

    QElapsedTimer t; t.start();
    QVariant r = client.invokeRemoteMethod(QStringLiteral("target_module"),
                                           QStringLiteral("ping"), QVariantList{});
    EXPECT_EQ(r.toString(), QStringLiteral("ok"));
    EXPECT_EQ(capProvider.mintCount(), 1);
    EXPECT_LT(t.elapsed(), 2000) << "a warm target must not wait on the gate at all";
}

// ── the async arm ────────────────────────────────────────────────────────────────
//
// whenObjectAvailable NEVER fires for an object that never appears, so without a deadline
// the queued continuations owe their callers an answer they never get. That is the one
// property a hang looks exactly like success from the outside.

TEST_F(ReadinessGatedExchangeTest, AnAsyncCallToATargetThatNeverAppearsStillAnswersItsCaller)
{
    auto capHost = makeLocalHost(LogosInstance::id("capability_module"));

    CapabilityProvider capProvider;
    ModuleProxy        capProxy(&capProvider);

    const QString bootstrap = QStringLiteral("bootstrap-tok-async-absent");
    TokenManager::instance().saveToken(QStringLiteral("capability_module"), bootstrap);
    ASSERT_TRUE(capProxy.saveToken(QStringLiteral("test_origin"), bootstrap));
    ASSERT_TRUE(capHost->publishObject("capability_module", &capProxy));

    LogosAPIClient client(QStringLiteral("target_module"),
                          QStringLiteral("test_origin"),
                          &TokenManager::instance());

    std::atomic<int> answered{0};
    std::atomic<int> unavailable{0};
    for (int i = 0; i < 3; ++i) {
        client.invokeRemoteMethodAsync(QStringLiteral("target_module"),
                                       QStringLiteral("ping"), QVariantList{},
                                       [&](QVariant, const logos::CallError& e) {
                                           if (e.code == "object_unavailable") unavailable++;
                                           answered++;
                                       },
                                       Timeout(600));
    }
    pumpEventLoop(2500);

    EXPECT_EQ(answered.load(), 3)
        << "a queued continuation owes its caller an answer; without the deadline timer "
           "whenObjectAvailable never fires and these hang forever";
    EXPECT_EQ(unavailable.load(), 3);
    EXPECT_EQ(capProvider.mintCount(), 0)
        << "nothing was minted against a target that never appeared";
}

TEST_F(ReadinessGatedExchangeTest, AnAsyncBurstToALateTargetMintsOnceAndCompletesWhenItAppears)
{
    auto capHost = makeLocalHost(LogosInstance::id("capability_module"));
    auto targetHost = makeLocalHost(LogosInstance::id("target_module"));

    PingProvider targetProvider;
    ModuleProxy  targetProxy(&targetProvider);
    targetProvider.bindProxy(&targetProxy);

    CapabilityProvider capProvider;
    ModuleProxy        capProxy(&capProvider);
    capProvider.bindTarget(&targetProxy);

    std::atomic<bool> published{false};
    capProvider.m_targetPublished = [&published] { return published.load(); };

    const QString bootstrap = QStringLiteral("bootstrap-tok-async-late");
    TokenManager::instance().saveToken(QStringLiteral("capability_module"), bootstrap);
    ASSERT_TRUE(capProxy.saveToken(QStringLiteral("test_origin"), bootstrap));
    ASSERT_TRUE(capHost->publishObject("capability_module", &capProxy));

    LogosAPIClient client(QStringLiteral("target_module"),
                          QStringLiteral("test_origin"),
                          &TokenManager::instance());
    capProvider.startClock();

    std::atomic<int> ok{0};
    for (int i = 0; i < 4; ++i) {
        client.invokeRemoteMethodAsync(QStringLiteral("target_module"),
                                       QStringLiteral("ping"), QVariantList{},
                                       [&](QVariant r, const logos::CallError&) {
                                           if (r.toString() == QStringLiteral("ok")) ok++;
                                       },
                                       Timeout(6000));
    }

    // The target appears late, as a real module process does.
    pumpEventLoop(500);
    ASSERT_TRUE(targetHost->publishObject("target_module", &targetProxy));
    published.store(true);
    pumpEventLoop(2500);

    EXPECT_EQ(ok.load(), 4);
    EXPECT_EQ(capProvider.mintCount(), 1)
        << "the burst must still coalesce behind ONE handshake — a second mint overwrites "
           "the first at the target and rejects its in-flight call";
    EXPECT_GE(capProvider.firstMintMs(), 400)
        << "minted at " << capProvider.firstMintMs()
        << " ms, i.e. before the target published";
}

TEST_F(ReadinessGatedExchangeTest, AnAsyncCallToAWarmTargetIsNotDelayedByTheGate)
{
    auto capHost = makeLocalHost(LogosInstance::id("capability_module"));
    auto targetHost = makeLocalHost(LogosInstance::id("target_module"));

    PingProvider targetProvider;
    ModuleProxy  targetProxy(&targetProvider);
    targetProvider.bindProxy(&targetProxy);

    CapabilityProvider capProvider;
    ModuleProxy        capProxy(&capProvider);
    capProvider.bindTarget(&targetProxy);

    const QString bootstrap = QStringLiteral("bootstrap-tok-async-warm");
    TokenManager::instance().saveToken(QStringLiteral("capability_module"), bootstrap);
    ASSERT_TRUE(capProxy.saveToken(QStringLiteral("test_origin"), bootstrap));
    ASSERT_TRUE(capHost->publishObject("capability_module", &capProxy));
    ASSERT_TRUE(targetHost->publishObject("target_module", &targetProxy));

    LogosAPIClient client(QStringLiteral("target_module"),
                          QStringLiteral("test_origin"),
                          &TokenManager::instance());
    for (int i = 0; i < 100 && !client.isConnected(); ++i) pumpEventLoop(20);
    ASSERT_TRUE(client.isConnected());

    std::atomic<int> ok{0};
    QElapsedTimer t; t.start();
    client.invokeRemoteMethodAsync(QStringLiteral("target_module"),
                                   QStringLiteral("ping"), QVariantList{},
                                   [&](QVariant r, const logos::CallError&) {
                                       if (r.toString() == QStringLiteral("ok")) ok++;
                                   },
                                   Timeout(5000));
    for (int i = 0; i < 100 && ok.load() == 0; ++i) pumpEventLoop(20);

    EXPECT_EQ(ok.load(), 1);
    EXPECT_EQ(capProvider.mintCount(), 1);
    EXPECT_LT(t.elapsed(), 2500)
        << "a warm target must not wait on the gate; the readiness arm fires synchronously";
}
