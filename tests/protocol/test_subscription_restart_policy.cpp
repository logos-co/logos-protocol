// Per-subscription restart policy: manual holds instead of re-arming.
//
// The property that needs the most protection is not the hold — it is that the
// hold does NOT reach the FIRST arm. A subscription taken before its provider
// has called listen() (a module's init(), a UI backend's onContextReady()) is
// deferred and armed under either policy; manual only decides what happens to a
// subscription that has ALREADY armed and then lost its provider. Get that
// wrong and every deferred subscription in the fleet stops working, which is
// why ManualStillArmsDeferred is here and why it is the first test.

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
    QVariant callMethod(const QString&, const QVariantList&) override { return QVariant(); }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    QJsonArray getMethods() override { return QJsonArray{}; }
    void setEventListener(EventCallback cb) override { emitFn = std::move(cb); }
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("echo_module"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }
};

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

void pump(int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

constexpr int kBudgetMs = 15000;

} // anonymous namespace

class SubscriptionRestartPolicyTest : public ::testing::Test {
protected:
    void SetUp() override { ensureApp(); LogosModeConfig::setMode(LogosMode::Remote); }
};

// ── the invariant ───────────────────────────────────────────────────────────

// Manual must NOT touch the deferred first arm. Subscribe against a module that
// does not exist yet, THEN publish it: it has to arm, exactly as an automatic
// one would. If this ever fails, every subscription taken during init() in the
// fleet is broken.
TEST_F(SubscriptionRestartPolicyTest, ManualStillArmsDeferred)
{
    const QString mod = QStringLiteral("policy_deferred_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));
    auto client = std::make_unique<LogosAPIClient>(mod, QStringLiteral("caller"),
                                                  &TokenManager::instance());

    const quint64 id = client->onEventWhenAvailable(
        mod, QStringLiteral("ev"), [](const QString&, const QVariantList&) {});
    ASSERT_NE(id, 0u);
    client->setSubscriptionRestartPolicy(mod, LogosRestartPolicy::Manual);

    pump(300);
    ASSERT_EQ(client->subscriptionGeneration(mod), 0u) << "armed against a module that is not up";
    EXPECT_EQ(client->eventSubscriptionState(id), LogosSubscriptionState::Pending);

    Publisher pub(mod);   // the module appears
    EXPECT_TRUE(pumpUntil([&] { return client->subscriptionGeneration(mod) == 1; }, kBudgetMs))
        << "a MANUAL subscription failed to make its FIRST arm -- manual must mean "
           "'do not RE-arm', never 'do not arm'";
}

// ── manual holds ────────────────────────────────────────────────────────────

TEST_F(SubscriptionRestartPolicyTest, ManualHoldsInsteadOfReArming)
{
    const QString mod = QStringLiteral("policy_hold_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));

    auto pub = std::make_unique<Publisher>(mod);
    auto client = std::make_unique<LogosAPIClient>(mod, QStringLiteral("caller"),
                                                  &TokenManager::instance());
    const quint64 id = client->onEventWhenAvailable(
        mod, QStringLiteral("ev"), [](const QString&, const QVariantList&) {});
    ASSERT_NE(id, 0u);
    client->setSubscriptionRestartPolicy(mod, LogosRestartPolicy::Manual);

    struct Edge { LogosSubscriptionEvent ev; quint64 gen; };
    std::vector<Edge> edges;
    client->setSubscriptionStatusCallback(
        mod, [&](LogosSubscriptionEvent ev, quint64 g, const QString&) { edges.push_back({ev, g}); });

    ASSERT_TRUE(pumpUntil([&] { return client->subscriptionGeneration(mod) == 1; }, kBudgetMs));

    pub.reset();   // provider goes away

    ASSERT_TRUE(pumpUntil([&] {
        for (const Edge& e : edges) if (e.ev == LogosSubscriptionEvent::Held) return true;
        return false;
    }, kBudgetMs)) << "manual subscription did not report Held";

    // Held is delivered INSTEAD OF Lost, never as well as it.
    for (const Edge& e : edges)
        EXPECT_NE(e.ev, LogosSubscriptionEvent::Lost)
            << "a manual subscription reported Lost as well as Held";

    EXPECT_EQ(client->eventSubscriptionState(id), LogosSubscriptionState::Held);

    // And it stays held even once the provider is back.
    auto pub2 = std::make_unique<Publisher>(mod);
    pump(4000);
    EXPECT_EQ(client->subscriptionGeneration(mod), 1u)
        << "a held subscription re-armed on its own -- the whole point of manual";
    EXPECT_EQ(client->eventSubscriptionState(id), LogosSubscriptionState::Held);
}

TEST_F(SubscriptionRestartPolicyTest, RearmRevivesAHeldSubscription)
{
    const QString mod = QStringLiteral("policy_rearm_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));

    auto pub = std::make_unique<Publisher>(mod);
    auto client = std::make_unique<LogosAPIClient>(mod, QStringLiteral("caller"),
                                                  &TokenManager::instance());
    const quint64 id = client->onEventWhenAvailable(
        mod, QStringLiteral("ev"), [](const QString&, const QVariantList&) {});
    ASSERT_NE(id, 0u);
    client->setSubscriptionRestartPolicy(mod, LogosRestartPolicy::Manual);
    ASSERT_TRUE(pumpUntil([&] { return client->subscriptionGeneration(mod) == 1; }, kBudgetMs));

    pub.reset();
    ASSERT_TRUE(pumpUntil([&] {
        return client->eventSubscriptionState(id) == LogosSubscriptionState::Held;
    }, kBudgetMs));

    // Rearm with the provider still gone: accepted, but stays pending.
    EXPECT_TRUE(client->rearmSubscriptions(mod));
    pump(1500);
    EXPECT_EQ(client->subscriptionGeneration(mod), 1u);

    // Provider returns; now it arms, and the generation advances so the
    // subscriber can still tell a gap happened.
    auto pub2 = std::make_unique<Publisher>(mod);
    EXPECT_TRUE(pumpUntil([&] { return client->subscriptionGeneration(mod) >= 2; }, kBudgetMs))
        << "rearm did not revive the subscription once the provider returned";
}

TEST_F(SubscriptionRestartPolicyTest, RearmRefusesWhatIsNotHeld)
{
    const QString mod = QStringLiteral("policy_rearm_refuse_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));
    Publisher pub(mod);
    auto client = std::make_unique<LogosAPIClient>(mod, QStringLiteral("caller"),
                                                  &TokenManager::instance());
    const quint64 id = client->onEventWhenAvailable(
        mod, QStringLiteral("ev"), [](const QString&, const QVariantList&) {});
    ASSERT_TRUE(pumpUntil([&] { return client->subscriptionGeneration(mod) == 1; }, kBudgetMs));

    EXPECT_FALSE(client->rearmSubscriptions(mod)) << "revived a subscription that is armed";
    EXPECT_FALSE(client->rearmSubscriptions(QStringLiteral("no_such_module")))
        << "revived a module nothing is subscribed to";
}

// A held subscription must still be cancellable, or a consumer that decides not
// to revive it has no way to be rid of it.
TEST_F(SubscriptionRestartPolicyTest, AHeldSubscriptionCanBeCancelled)
{
    const QString mod = QStringLiteral("policy_cancel_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));

    auto pub = std::make_unique<Publisher>(mod);
    auto client = std::make_unique<LogosAPIClient>(mod, QStringLiteral("caller"),
                                                  &TokenManager::instance());
    const quint64 id = client->onEventWhenAvailable(
        mod, QStringLiteral("ev"), [](const QString&, const QVariantList&) {});
    ASSERT_TRUE(pumpUntil([&] { return client->subscriptionGeneration(mod) == 1; }, kBudgetMs));
    client->setSubscriptionRestartPolicy(mod, LogosRestartPolicy::Manual);

    pub.reset();
    ASSERT_TRUE(pumpUntil([&] {
        return client->eventSubscriptionState(id) == LogosSubscriptionState::Held;
    }, kBudgetMs));

    EXPECT_TRUE(client->cancelEventSubscription(id));
    EXPECT_EQ(client->eventSubscriptionState(id), LogosSubscriptionState::Unknown);
}

// ── the control ─────────────────────────────────────────────────────────────

// Automatic is untouched: still Lost, still re-arms, still advances. Without
// this a green "manual holds" proves only that something changed.
TEST_F(SubscriptionRestartPolicyTest, AutomaticIsUnchanged)
{
    const QString mod = QStringLiteral("policy_automatic_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));

    auto pub = std::make_unique<Publisher>(mod);
    auto client = std::make_unique<LogosAPIClient>(mod, QStringLiteral("caller"),
                                                  &TokenManager::instance());
    const quint64 id = client->onEventWhenAvailable(
        mod, QStringLiteral("ev"), [](const QString&, const QVariantList&) {});
    ASSERT_NE(id, 0u);   // no setSubscriptionRestartPolicy call at all

    std::atomic<int> lost{0}, held{0};
    client->setSubscriptionStatusCallback(
        mod, [&](LogosSubscriptionEvent ev, quint64, const QString&) {
            if (ev == LogosSubscriptionEvent::Lost) lost.fetch_add(1);
            if (ev == LogosSubscriptionEvent::Held) held.fetch_add(1);
        });
    ASSERT_TRUE(pumpUntil([&] { return client->subscriptionGeneration(mod) == 1; }, kBudgetMs));

    pub.reset();
    ASSERT_TRUE(pumpUntil([&] { return lost.load() > 0; }, kBudgetMs)) << "automatic did not report Lost";
    EXPECT_EQ(held.load(), 0) << "automatic reported Held";

    auto pub2 = std::make_unique<Publisher>(mod);
    EXPECT_TRUE(pumpUntil([&] { return client->subscriptionGeneration(mod) >= 2; }, kBudgetMs))
        << "automatic did not re-arm on its own";
}

// ── the coupling ────────────────────────────────────────────────────────────

// Liveness is per-MODULE: m_handles holds ONE handle per object name and every
// subscription to that object hangs off it, so two subscriptions to one module
// rise and fall TOGETHER. This is the fact the whole per-target design rests
// on; if it ever stops holding, keying the policy and the watcher by module
// becomes wrong rather than merely simpler.
TEST_F(SubscriptionRestartPolicyTest, SubscriptionsToOneModuleTransitionTogether)
{
    const QString mod = QStringLiteral("policy_coupled_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));

    auto pub = std::make_unique<Publisher>(mod);
    auto client = std::make_unique<LogosAPIClient>(mod, QStringLiteral("caller"),
                                                  &TokenManager::instance());

    const quint64 a = client->onEventWhenAvailable(
        mod, QStringLiteral("ev_a"), [](const QString&, const QVariantList&) {});
    const quint64 b = client->onEventWhenAvailable(
        mod, QStringLiteral("ev_b"), [](const QString&, const QVariantList&) {});
    ASSERT_NE(a, 0u);
    ASSERT_NE(b, 0u);

    std::atomic<int> lost{0};
    client->setSubscriptionStatusCallback(
        mod, [&](LogosSubscriptionEvent e, quint64, const QString&) {
            if (e == LogosSubscriptionEvent::Lost) lost.fetch_add(1);
        });

    ASSERT_TRUE(pumpUntil([&] { return client->subscriptionGeneration(mod) == 1; }, kBudgetMs));
    ASSERT_EQ(client->eventSubscriptionState(a), LogosSubscriptionState::Armed);
    ASSERT_EQ(client->eventSubscriptionState(b), LogosSubscriptionState::Armed);

    pub.reset();   // ONE provider goes away

    ASSERT_TRUE(pumpUntil([&] { return lost.load() > 0; }, kBudgetMs))
        << "losing the provider was not reported";

    // ONCE, not once per subscription. This is the redundancy the per-target
    // key removes: the same loss used to be delivered N times, leaving a
    // consumer to de-duplicate an event that was never plural.
    pump(2500);
    EXPECT_EQ(lost.load(), 1)
        << "one provider loss produced " << lost.load()
        << " reports -- the status callback is per MODULE, not per subscription";

    auto pub2 = std::make_unique<Publisher>(mod);
    EXPECT_TRUE(pumpUntil([&] { return client->subscriptionGeneration(mod) >= 2; }, kBudgetMs))
        << "subscriptions to the same module did not rise together";
    EXPECT_EQ(client->eventSubscriptionState(a), LogosSubscriptionState::Armed);
    EXPECT_EQ(client->eventSubscriptionState(b), LogosSubscriptionState::Armed);
}

// The policy is per MODULE, so every subscription to it holds together and the
// divergence a per-subscription policy allowed cannot be expressed any more.
//
// This test replaces one that pinned the OPPOSITE — it asserted that an
// automatic and a manual subscription to the same module ended up permanently
// in different states, one revived and one held. That behaviour was real and
// reachable; it was also useless, describing a split in an event that is
// indivisibly per-module. Keeping the test as documentation of the old shape
// would have been keeping a footgun with a certificate.
TEST_F(SubscriptionRestartPolicyTest, ThePolicyAppliesToEverySubscriptionOnTheModule)
{
    const QString mod = QStringLiteral("policy_uniform_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));

    auto pub = std::make_unique<Publisher>(mod);
    auto client = std::make_unique<LogosAPIClient>(mod, QStringLiteral("caller"),
                                                  &TokenManager::instance());

    const quint64 first = client->onEventWhenAvailable(
        mod, QStringLiteral("ev_one"), [](const QString&, const QVariantList&) {});
    // Set BETWEEN the two subscribes: the policy belongs to the module, so it
    // governs the one taken before it just as much as the one taken after.
    client->setSubscriptionRestartPolicy(mod, LogosRestartPolicy::Manual);
    const quint64 second = client->onEventWhenAvailable(
        mod, QStringLiteral("ev_two"), [](const QString&, const QVariantList&) {});

    ASSERT_TRUE(pumpUntil([&] { return client->subscriptionGeneration(mod) == 1; }, kBudgetMs));

    pub.reset();
    ASSERT_TRUE(pumpUntil([&] {
        return client->eventSubscriptionState(first) == LogosSubscriptionState::Held;
    }, kBudgetMs));
    EXPECT_EQ(client->eventSubscriptionState(second), LogosSubscriptionState::Held)
        << "one subscription held and the other did not -- the policy is per module";

    // Neither revives on its own, and rearming revives BOTH.
    auto pub2 = std::make_unique<Publisher>(mod);
    pump(3000);
    EXPECT_EQ(client->eventSubscriptionState(first), LogosSubscriptionState::Held);
    EXPECT_EQ(client->eventSubscriptionState(second), LogosSubscriptionState::Held);

    EXPECT_TRUE(client->rearmSubscriptions(mod));
    EXPECT_TRUE(pumpUntil([&] { return client->subscriptionGeneration(mod) >= 2; }, kBudgetMs));
    EXPECT_EQ(client->eventSubscriptionState(first), LogosSubscriptionState::Armed);
    EXPECT_EQ(client->eventSubscriptionState(second), LogosSubscriptionState::Armed);
}

// A watcher installed AFTER the fact still learns where the module is. The
// per-target record outlives any single subscription, so this is now the normal
// order rather than a race: configure the client, subscribe, and the replay
// covers anything that happened in between.
TEST_F(SubscriptionRestartPolicyTest, InstallingAWatcherReplaysTheCurrentState)
{
    const QString mod = QStringLiteral("policy_replay_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));

    auto pub = std::make_unique<Publisher>(mod);
    auto client = std::make_unique<LogosAPIClient>(mod, QStringLiteral("caller"),
                                                  &TokenManager::instance());
    const quint64 id = client->onEventWhenAvailable(
        mod, QStringLiteral("ev"), [](const QString&, const QVariantList&) {});
    ASSERT_NE(id, 0u);
    ASSERT_TRUE(pumpUntil([&] { return client->subscriptionGeneration(mod) == 1; }, kBudgetMs));

    // Installed long after the arm: it must be told about it anyway.
    std::vector<LogosSubscriptionEvent> seen;
    client->setSubscriptionStatusCallback(
        mod, [&](LogosSubscriptionEvent e, quint64, const QString&) { seen.push_back(e); });
    ASSERT_FALSE(seen.empty()) << "installing a watcher on an armed module replayed nothing";
    EXPECT_EQ(seen.front(), LogosSubscriptionEvent::Armed);
}

// Configure BEFORE subscribing to anything. This is the order the per-target
// key is supposed to make safe, and it is the order that broke: the registry is
// built lazily by the first onEventWhenAvailable(), so a setter that merely
// bailed when it was absent turned configure-then-subscribe into a silent
// no-op. The manual policy was accepted, dropped, and the subscription re-armed
// itself — the exact failure the policy exists to prevent, with no diagnostic.
TEST_F(SubscriptionRestartPolicyTest, PolicySetBeforeAnySubscriptionStillApplies)
{
    const QString mod = QStringLiteral("policy_before_subscribe_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));

    auto pub = std::make_unique<Publisher>(mod);
    auto client = std::make_unique<LogosAPIClient>(mod, QStringLiteral("caller"),
                                                  &TokenManager::instance());

    // Nothing has subscribed yet — there is no registry at this point.
    client->setSubscriptionRestartPolicy(mod, LogosRestartPolicy::Manual);
    std::atomic<int> held{0};
    client->setSubscriptionStatusCallback(
        mod, [&](LogosSubscriptionEvent e, quint64, const QString&) {
            if (e == LogosSubscriptionEvent::Held) held.fetch_add(1);
        });

    const quint64 id = client->onEventWhenAvailable(
        mod, QStringLiteral("ev"), [](const QString&, const QVariantList&) {});
    ASSERT_NE(id, 0u);
    ASSERT_TRUE(pumpUntil([&] { return client->subscriptionGeneration(mod) == 1; }, kBudgetMs))
        << "a subscription taken after the policy was set never armed";

    pub.reset();
    EXPECT_TRUE(pumpUntil([&] { return held.load() > 0; }, kBudgetMs))
        << "the policy set before the first subscribe was dropped";
    EXPECT_EQ(client->eventSubscriptionState(id), LogosSubscriptionState::Held);
}

// A module whose handle goes stale with NOTHING armed against it produces no
// edge. The handle outlives the last cancel by up to a watchdog tick, so this
// state is reachable in normal use — and a Lost reported there would tell a
// watcher its subscriptions died when it had already ended them itself.
TEST_F(SubscriptionRestartPolicyTest, ALossWithNothingArmedReportsNothing)
{
    const QString mod = QStringLiteral("policy_no_armed_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));

    auto pub = std::make_unique<Publisher>(mod);
    auto client = std::make_unique<LogosAPIClient>(mod, QStringLiteral("caller"),
                                                  &TokenManager::instance());

    std::atomic<int> edges{0};
    client->setSubscriptionStatusCallback(
        mod, [&](LogosSubscriptionEvent, quint64, const QString&) { edges.fetch_add(1); });

    const quint64 id = client->onEventWhenAvailable(
        mod, QStringLiteral("ev"), [](const QString&, const QVariantList&) {});
    ASSERT_TRUE(pumpUntil([&] { return client->subscriptionGeneration(mod) == 1; }, kBudgetMs));
    ASSERT_GT(edges.load(), 0);   // the arm was reported

    ASSERT_TRUE(client->cancelEventSubscription(id));
    const int afterCancel = edges.load();

    // Now lose the provider, with nothing armed.
    pub.reset();
    pump(4000);
    EXPECT_EQ(edges.load(), afterCancel)
        << "a provider loss was reported although nothing was subscribed";
}

// ── the C ABI ───────────────────────────────────────────────────────────────

TEST_F(SubscriptionRestartPolicyTest, LpClientOptionsCarryTheManualPolicy)
{
    const QString mod = QStringLiteral("policy_abi_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));

    auto pub = std::make_unique<Publisher>(mod);
    lp_client* client = lp_client_create(mod.toUtf8().constData(), "caller", nullptr, nullptr);
    ASSERT_NE(client, nullptr);

    struct Seen { std::vector<int> states; } seen;
    ASSERT_EQ(lp_client_set_subscription_status_cb(
        client,
        [](int state, unsigned long long, const char*, void* ud) {
            static_cast<Seen*>(ud)->states.push_back(state);
        }, &seen), 1);
    ASSERT_EQ(lp_client_set_subscription_options(client, R"({"restart":"manual"})"), 1);

    lp_subscription* sub = lp_subscribe(
        client, "ev", [](const char*, const char*, void*) {}, nullptr);
    ASSERT_NE(sub, nullptr);

    ASSERT_TRUE(pumpUntil([&] { return lp_client_subscription_generation(client) == 1; }, kBudgetMs));
    pub.reset();
    ASSERT_TRUE(pumpUntil([&] {
        for (int s : seen.states) if (s == LP_SUB_HELD) return true;
        return false;
    }, kBudgetMs)) << "no LP_SUB_HELD from a manual client";

    for (int s : seen.states)
        EXPECT_NE(s, LP_SUB_LOST) << "manual delivered LP_SUB_LOST as well as LP_SUB_HELD";

    EXPECT_EQ(lp_client_rearm_subscriptions(client), 1);
    auto pub2 = std::make_unique<Publisher>(mod);
    EXPECT_TRUE(pumpUntil([&] { return lp_client_subscription_generation(client) >= 2; }, kBudgetMs));

    lp_unsubscribe(sub);
    lp_client_destroy(client);
}

// A NULL or absent options document must mean "defaults", and an unknown key
// must be ignored — a newer caller against an older runtime should lose the
// OPTION, not its subscriptions. Unparseable JSON is the one case that reports
// failure, because unlike an unknown key it cannot be a forward-compatible
// document: nothing will ever make it parse.
TEST_F(SubscriptionRestartPolicyTest, OptionsDegradeToDefaultsRatherThanFailing)
{
    const QString mod = QStringLiteral("policy_abi_defaults_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));
    Publisher pub(mod);
    lp_client* client = lp_client_create(mod.toUtf8().constData(), "caller", nullptr, nullptr);
    ASSERT_NE(client, nullptr);

    for (const char* opts : {static_cast<const char*>(nullptr), "", "{}",
                             R"({"restart":"nonsense"})", R"({"unknown_key":1})"}) {
        EXPECT_EQ(lp_client_set_subscription_options(client, opts), 1)
            << "options '" << (opts ? opts : "(null)") << "' were refused";
    }
    EXPECT_EQ(lp_client_set_subscription_options(client, "not json"), 0)
        << "unparseable options must be reported, not silently ignored";

    // Whatever the above did, subscribing still works and stays automatic.
    lp_subscription* sub = lp_subscribe(
        client, "ev", [](const char*, const char*, void*) {}, nullptr);
    EXPECT_NE(sub, nullptr);
    if (sub) lp_unsubscribe(sub);
    lp_client_destroy(client);
}

// A client that sets no options at all stays automatic: still LP_SUB_LOST,
// still re-arms on its own. Without this the manual test above proves only that
// something changed.
TEST_F(SubscriptionRestartPolicyTest, AClientWithNoOptionsStaysAutomatic)
{
    const QString mod = QStringLiteral("policy_abi_default_automatic_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));

    auto pub = std::make_unique<Publisher>(mod);
    lp_client* client = lp_client_create(mod.toUtf8().constData(), "caller", nullptr, nullptr);
    ASSERT_NE(client, nullptr);

    struct Seen { std::vector<int> states; } seen;
    ASSERT_EQ(lp_client_set_subscription_status_cb(
        client,
        [](int state, unsigned long long, const char*, void* ud) {
            static_cast<Seen*>(ud)->states.push_back(state);
        }, &seen), 1);

    lp_subscription* sub = lp_subscribe(
        client, "ev", [](const char*, const char*, void*) {}, nullptr);
    ASSERT_NE(sub, nullptr);
    ASSERT_TRUE(pumpUntil([&] { return lp_client_subscription_generation(client) == 1; }, kBudgetMs));

    pub.reset();
    ASSERT_TRUE(pumpUntil([&] {
        for (int s : seen.states) if (s == LP_SUB_LOST) return true;
        return false;
    }, kBudgetMs)) << "a client with no options stopped defaulting to automatic";
    for (int s : seen.states)
        EXPECT_NE(s, LP_SUB_HELD);

    lp_unsubscribe(sub);
    lp_client_destroy(client);
}
