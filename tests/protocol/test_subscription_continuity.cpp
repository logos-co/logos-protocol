// Subscription continuity: telling "the provider restarted and I missed
// events" apart from "the module has been quiet".
//
// The defect these pin: a provider that unloads and reloads drives its replica
// out of Valid and back on the SAME node, and the event helper stays attached
// to that replica -- so the subscription survives and NOTHING said so. Events
// emitted while the provider was down reached nobody, the stream resumed, and
// no subscriber could see the hole. Worse, nothing was looking: reconnected()
// covers a torn-down connection and reviveArmed() covers a handle being
// replaced, but neither observes a provider dying underneath an ALREADY-ARMED
// subscription, and the retry timer stopped the moment everything armed -- so
// there was no tick in which to notice.
//
// Three properties are pinned here:
//   1. the generation counter advances across a provider restart, for a plain
//      lp_subscribe caller that adopts nothing,
//   2. the client's status callback reports the LOST -> ARMED pair that
//      brackets the gap,
//   3. a provider that never dies produces neither -- the control, without
//      which a green LOST cannot be distinguished from a mis-wired fixture.

#include <gtest/gtest.h>

#include "logos_api_client.h"
#include "logos_instance.h"
#include "logos_mode.h"
#include "logos_object.h"
#include "logos_protocol.h"
#include "logos_provider_interface.h"
#include "logos_subscription_state.h"
#include "module_proxy.h"
#include "remote_transport.h"
#include "token_manager.h"

#include <QCoreApplication>
#include <QString>
#include <QVariantList>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

QCoreApplication* ensureApp() {
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance())
        new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

class EchoProvider : public LogosProviderObject {
public:
    EventCallback emitFn;
    QVariant callMethod(const QString& method, const QVariantList& args) override {
        if (method == QLatin1String("echo") && !args.isEmpty()) return args.first();
        return QVariant();
    }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    QJsonArray getMethods() override { return QJsonArray{}; }
    void setEventListener(EventCallback cb) override { emitFn = std::move(cb); }
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("echo_module"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }
};

// The provider half, held in a unique_ptr by the tests so destroying it is how
// a test spells "the module unloaded".
struct Publisher {
    EchoProvider echo;
    ModuleProxy proxy;
    RemoteTransportHost host;
    explicit Publisher(const QString& moduleName)
        : proxy(&echo), host(LogosInstance::id(moduleName))
    {
        proxy.saveToken(QStringLiteral("caller"), QStringLiteral("tok"));
        host.publishObject(moduleName, &proxy);
    }
};

void pump(int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// Pump until `done` or the budget runs out. Returns whether it became true, so
// a caller can assert on it rather than on a bare timeout.
template <typename Fn>
bool pumpUntil(Fn done, int budgetMs) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    while (std::chrono::steady_clock::now() < end) {
        if (done()) return true;
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

// The liveness poll runs at 1 s; give every wait several cycles of headroom so
// a loaded CI box does not turn a latency into a failure.
constexpr int kRestartBudgetMs = 15000;

} // anonymous namespace

class SubscriptionContinuityTest : public ::testing::Test {
protected:
    void SetUp() override { ensureApp(); LogosModeConfig::setMode(LogosMode::Remote); }
};

// ── consumer layer ───────────────────────────────────────────────────────────

// The core case. Subscribe, let it arm, kill the provider, bring it back: the
// subscription must re-arm (it always did) AND say so (it never did).
TEST_F(SubscriptionContinuityTest, ProviderRestart_ReportsLostThenArmedWithNewGeneration)
{
    const QString mod = QStringLiteral("continuity_restart_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));

    auto pub = std::make_unique<Publisher>(mod);
    auto client = std::make_unique<LogosAPIClient>(mod, QStringLiteral("caller"),
                                                  &TokenManager::instance());

    const quint64 id = client->onEventWhenAvailable(
        mod, QStringLiteral("ev"), [](const QString&, const QVariantList&) {});
    ASSERT_NE(id, 0u);

    struct Edge { LogosSubscriptionEvent ev; quint64 generation; QString reason; };
    std::vector<Edge> edges;
    client->setSubscriptionStatusCallback(
        mod, [&](LogosSubscriptionEvent ev, quint64 gen, const QString& reason) {
            edges.push_back({ev, gen, reason});
        });

    // Armed at generation 1 -- either already (replayed by the installer) or
    // shortly, depending on whether the arm beat the install.
    ASSERT_TRUE(pumpUntil([&] { return client->subscriptionGeneration(mod) == 1; },
                          kRestartBudgetMs))
        << "subscription never armed against a published module";
    ASSERT_FALSE(edges.empty());
    EXPECT_EQ(edges.front().ev, LogosSubscriptionEvent::Armed);
    EXPECT_EQ(edges.front().generation, 1u);

    // The module unloads.
    pub.reset();

    ASSERT_TRUE(pumpUntil([&] {
        for (const Edge& e : edges)
            if (e.ev == LogosSubscriptionEvent::Lost) return true;
        return false;
    }, kRestartBudgetMs))
        << "provider died and the subscription never reported LOST -- this is the "
           "silent gap the watchdog exists to remove";

    for (const Edge& e : edges) {
        if (e.ev != LogosSubscriptionEvent::Lost) continue;
        EXPECT_EQ(e.generation, 1u) << "LOST must name the arming that just ended";
        EXPECT_EQ(e.reason, QStringLiteral("provider_unavailable"));
    }

    // ...and comes back. A re-established subscription is a NEW one.
    auto pub2 = std::make_unique<Publisher>(mod);
    ASSERT_TRUE(pumpUntil([&] { return client->subscriptionGeneration(mod) >= 2; },
                          kRestartBudgetMs))
        << "subscription never re-armed after the provider returned";

    ASSERT_GE(edges.size(), 3u);
    EXPECT_EQ(edges.back().ev, LogosSubscriptionEvent::Armed);
    EXPECT_GE(edges.back().generation, 2u)
        << "a re-arm must advance the generation, or a subscriber cannot tell it "
           "apart from the original subscription";
}

// The control. Without it a green LOST above proves only that SOMETHING fires.
TEST_F(SubscriptionContinuityTest, ProviderStaysUp_NoLostAndGenerationStaysOne)
{
    const QString mod = QStringLiteral("continuity_stable_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));

    Publisher pub(mod);
    auto client = std::make_unique<LogosAPIClient>(mod, QStringLiteral("caller"),
                                                  &TokenManager::instance());

    const quint64 id = client->onEventWhenAvailable(
        mod, QStringLiteral("ev"), [](const QString&, const QVariantList&) {});
    ASSERT_NE(id, 0u);

    std::atomic<int> lost{0};
    client->setSubscriptionStatusCallback(
        mod, [&](LogosSubscriptionEvent ev, quint64, const QString&) {
            if (ev == LogosSubscriptionEvent::Lost) lost.fetch_add(1);
        });

    ASSERT_TRUE(pumpUntil([&] { return client->subscriptionGeneration(mod) == 1; },
                          kRestartBudgetMs));

    // Several liveness cycles with the provider healthy.
    pump(5000);

    EXPECT_EQ(lost.load(), 0) << "reported LOST for a provider that never went away";
    EXPECT_EQ(client->subscriptionGeneration(mod), 1u)
        << "re-armed a subscription that was never lost";
}

// ── C ABI ────────────────────────────────────────────────────────────────────

// The half that is NOT opt-in: a caller using plain lp_subscribe, adopting no
// callback and changing nothing about how it subscribes, can still detect the
// gap by reading the generation next to its events.
TEST_F(SubscriptionContinuityTest, LpSubscribe_GenerationAdvancesAcrossRestart)
{
    const QString mod = QStringLiteral("continuity_abi_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));

    auto pub = std::make_unique<Publisher>(mod);
    lp_client* client = lp_client_create(mod.toUtf8().constData(), "caller", nullptr, nullptr);
    ASSERT_NE(client, nullptr);

    lp_subscription* sub = lp_subscribe(
        client, "ev",
        [](const char*, const char*, void*) {},
        nullptr);
    ASSERT_NE(sub, nullptr);

    ASSERT_TRUE(pumpUntil([&] { return lp_client_subscription_generation(client) == 1; },
                          kRestartBudgetMs))
        << "lp_subscribe never armed";

    pub.reset();
    auto pub2 = std::make_unique<Publisher>(mod);

    EXPECT_TRUE(pumpUntil([&] { return lp_client_subscription_generation(client) >= 2; },
                          kRestartBudgetMs))
        << "generation did not advance across a provider restart -- a plain "
           "lp_subscribe caller has no way to see the gap";

    lp_unsubscribe(sub);
    lp_client_destroy(client);
}

// The opt-in half: the client's status callback delivers the bracketing pair.
TEST_F(SubscriptionContinuityTest, LpClientStatusCb_DeliversLostThenArmed)
{
    const QString mod = QStringLiteral("continuity_abi_ex_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));

    auto pub = std::make_unique<Publisher>(mod);
    lp_client* client = lp_client_create(mod.toUtf8().constData(), "caller", nullptr, nullptr);
    ASSERT_NE(client, nullptr);

    struct Seen {
        std::vector<int> states;
        std::vector<unsigned long long> gens;
        std::string lastReason;
    } seen;

    ASSERT_EQ(lp_client_set_subscription_status_cb(
        client,
        [](int state, unsigned long long gen, const char* reason, void* ud) {
            auto* s = static_cast<Seen*>(ud);
            s->states.push_back(state);
            s->gens.push_back(gen);
            if (reason) s->lastReason = reason;
        },
        &seen), 1);

    lp_subscription* sub = lp_subscribe(
        client, "ev",
        [](const char*, const char*, void*) {},
        nullptr);
    ASSERT_NE(sub, nullptr);

    ASSERT_TRUE(pumpUntil([&] { return !seen.states.empty(); }, kRestartBudgetMs));
    EXPECT_EQ(seen.states.front(), LP_SUB_ARMED);
    EXPECT_EQ(seen.gens.front(), 1ull);

    pub.reset();
    ASSERT_TRUE(pumpUntil([&] {
        for (int s : seen.states) if (s == LP_SUB_LOST) return true;
        return false;
    }, kRestartBudgetMs)) << "no LP_SUB_LOST after the provider went away";
    EXPECT_EQ(seen.lastReason, "provider_unavailable");

    auto pub2 = std::make_unique<Publisher>(mod);
    EXPECT_TRUE(pumpUntil([&] { return lp_client_subscription_generation(client) >= 2; },
                          kRestartBudgetMs));
    EXPECT_EQ(seen.states.back(), LP_SUB_ARMED);

    lp_unsubscribe(sub);
    lp_client_destroy(client);
}

// The generation mirror is installed at lp_client_create whether or not anyone
// asks for a status callback. A client that never installs one must keep
// working — and keep counting — rather than crashing in that always-present
// trampoline.
TEST_F(SubscriptionContinuityTest, NoStatusCb_IsStillPlainSubscribe)
{
    const QString mod = QStringLiteral("continuity_abi_null_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));

    Publisher pub(mod);
    lp_client* client = lp_client_create(mod.toUtf8().constData(), "caller", nullptr, nullptr);
    ASSERT_NE(client, nullptr);

    std::atomic<int> events{0};
    lp_subscription* sub = lp_subscribe(
        client, "ev",
        [](const char*, const char*, void* ud) {
            static_cast<std::atomic<int>*>(ud)->fetch_add(1);
        },
        &events);
    ASSERT_NE(sub, nullptr);

    EXPECT_TRUE(pumpUntil([&] { return lp_client_subscription_generation(client) == 1; },
                          kRestartBudgetMs));

    lp_unsubscribe(sub);
    lp_client_destroy(client);
}
