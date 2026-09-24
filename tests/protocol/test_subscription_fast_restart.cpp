// A provider that is replaced faster than the 1 s liveness poll must still read as a restart:
// a LOST edge, a new generation, and no event from the replacement under the old generation.

#include <gtest/gtest.h>

#include "local_host.h"
#include "logos_api_client.h"
#include "logos_instance.h"
#include "logos_mode.h"
#include "logos_object.h"
#include "logos_object_source_watch.h"
#include "logos_protocol.h"
#include "logos_provider_interface.h"
#include "logos_subscription_state.h"
#include "module_proxy.h"
#include "remote_transport.h"
#include "token_manager.h"

#include <QCoreApplication>
#include <QRemoteObjectRegistryHost>
#include <QString>
#include <QUrl>
#include <QVariantList>

#include <algorithm>
#include <chrono>
#include <cstring>
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

// One provider instance; every event it emits carries its tag.
struct Provider {
    EchoProvider echo;
    ModuleProxy proxy{&echo};
    QString tag;
    explicit Provider(QString t) : tag(std::move(t)) {
        proxy.saveToken(QStringLiteral("caller"), QStringLiteral("tok"));
    }
    void emitEvent() { echo.emitFn(QStringLiteral("ev"), QVariantList{tag}); }
};

// A live QtRO host whose published object is swapped without dropping the connection
// (RemoveObject path).
struct QtSwappableHost {
    QRemoteObjectRegistryHost host;
    QString name;
    explicit QtSwappableHost(const QString& mod) : name(mod) {
        EXPECT_TRUE(host.setRegistryUrl(QUrl(LogosInstance::id(mod))));
    }
    void publish(Provider& p) { EXPECT_TRUE(host.enableRemoting(&p.proxy, name)); }
    void replace(Provider& from, Provider& to) {
        EXPECT_TRUE(host.disableRemoting(&from.proxy));
        EXPECT_TRUE(host.enableRemoting(&to.proxy, name));
    }
};

// A fast restart on the local transport. qt_remote swaps the source on the live
// connection; a qt_remote_plain host is a process, so it restarts as a new host.
struct SwappableHost {
    std::unique_ptr<QtSwappableHost> qt;
    std::unique_ptr<LogosTransportHost> plain;
    QString name;
    explicit SwappableHost(const QString& mod) : name(mod) {
        if (localIsPlain()) plain = makeLocalHost(LogosInstance::id(mod));
        else qt = std::make_unique<QtSwappableHost>(mod);
    }
    void publish(Provider& p) {
        if (qt) qt->publish(p);
        else EXPECT_TRUE(plain->publishObject(name, &p.proxy));
    }
    void replace(Provider& from, Provider& to) {
        if (qt) return qt->replace(from, to);
        plain.reset();
        plain = makeLocalHost(LogosInstance::id(name));
        EXPECT_TRUE(plain->publishObject(name, &to.proxy));
    }
};

// A whole host that is torn down and rebound on the same socket, like a module process swap.
struct Publisher {
    Provider provider;
    std::unique_ptr<LogosTransportHost> host;
    Publisher(const QString& mod, const QString& tag)
        : provider(tag), host(makeLocalHost(LogosInstance::id(mod))) {
        EXPECT_TRUE(host->publishObject(mod, &provider.proxy));
    }
};

struct Seen {
    bool isEvent = false;
    LogosSubscriptionEvent edge = LogosSubscriptionEvent::Armed;
    quint64 gen = 0;   // the edge's generation, or the one current when the event arrived
    QString tag;
};

struct Log {
    std::vector<Seen> items;

    std::function<void(LogosSubscriptionEvent, quint64, const QString&)> status() {
        return [this](LogosSubscriptionEvent e, quint64 g, const QString&) {
            items.push_back({false, e, g, QString()});
        };
    }
    std::function<void(const QString&, const QVariantList&)> events(LogosAPIClient* c,
                                                                   const QString& mod) {
        return [this, c, mod](const QString&, const QVariantList& data) {
            items.push_back({true, LogosSubscriptionEvent::Armed, c->subscriptionGeneration(mod),
                             data.value(0).toString()});
        };
    }
    int firstEvent(const QString& tag) const {
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].isEvent && items[i].tag == tag) return int(i);
        return -1;
    }
    int firstEdge(LogosSubscriptionEvent e, size_t from = 0) const {
        for (size_t i = from; i < items.size(); ++i)
            if (!items[i].isEvent && items[i].edge == e) return int(i);
        return -1;
    }
    int countEdges(LogosSubscriptionEvent e) const {
        int n = 0;
        for (const Seen& s : items) n += (!s.isEvent && s.edge == e);
        return n;
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

void pump(int ms) { pumpUntil([] { return false; }, ms); }

// Keeps emitting until one of `p`'s events is seen, so a re-arm gap only costs retries.
template <typename Delivered>
bool emitUntil(Provider& p, Delivered delivered, int budgetMs) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    while (std::chrono::steady_clock::now() < end) {
        p.emitEvent();
        if (pumpUntil(delivered, 25)) return true;
    }
    return delivered();
}

constexpr int kBudgetMs = 15000;

// Every event from `tag` must have arrived under `gen`, and a LOST must precede the first one.
void expectRestartBracketsEvents(const Log& log, const QString& tag, quint64 gen) {
    const int first = log.firstEvent(tag);
    ASSERT_GE(first, 0) << "no event from " << tag.toStdString();
    int lost = -1;
    for (int i = first - 1; i >= 0 && lost < 0; --i)
        if (!log.items[i].isEvent && log.items[i].edge == LogosSubscriptionEvent::Lost) lost = i;
    EXPECT_GE(lost, 0) << tag.toStdString() << "'s events arrived with no LOST before them";
    for (const Seen& s : log.items)
        if (s.isEvent && s.tag == tag)
            EXPECT_EQ(s.gen, gen) << "an event from " << tag.toStdString()
                                  << " arrived under a stale generation";
}

} // anonymous namespace

class FastProviderRestartTest : public ::testing::Test {
protected:
    void SetUp() override { ensureApp(); LogosModeConfig::setMode(LogosMode::Remote); }
};

// DETECTOR: the source is swapped inside one host, so the gap lasts one round trip.
TEST_F(FastProviderRestartTest, ASourceReplacedOnALiveConnectionIsARestart)
{
    const QString mod = QStringLiteral("fast_restart_republish_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));

    Provider p1(QStringLiteral("p1")), p2(QStringLiteral("p2"));
    SwappableHost host(mod);
    host.publish(p1);

    LogosAPIClient client(mod, QStringLiteral("caller"), &TokenManager::instance());
    Log log;
    client.setSubscriptionStatusCallback(mod, log.status());
    ASSERT_NE(client.onEventWhenAvailable(mod, QStringLiteral("ev"), log.events(&client, mod)), 0u);
    ASSERT_TRUE(pumpUntil([&] { return client.subscriptionGeneration(mod) == 1; }, kBudgetMs));
    ASSERT_TRUE(emitUntil(p1, [&] { return log.firstEvent(p1.tag) >= 0; }, kBudgetMs));

    host.replace(p1, p2);
    ASSERT_TRUE(emitUntil(p2, [&] { return log.firstEvent(p2.tag) >= 0; }, kBudgetMs))
        << "the replacement's events never arrived";

    expectRestartBracketsEvents(log, p2.tag, 2);
    EXPECT_EQ(log.countEdges(LogosSubscriptionEvent::Lost), 1);
    EXPECT_EQ(client.subscriptionGeneration(mod), 2u);
}

// DETECTOR: whole-host swaps back to back, each gap well under the poll interval.
TEST_F(FastProviderRestartTest, BackToBackHostSwapsAreEachARestart)
{
    const QString mod = QStringLiteral("fast_restart_swap_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));

    auto pub = std::make_unique<Publisher>(mod, QStringLiteral("p0"));
    LogosAPIClient client(mod, QStringLiteral("caller"), &TokenManager::instance());
    Log log;
    client.setSubscriptionStatusCallback(mod, log.status());
    ASSERT_NE(client.onEventWhenAvailable(mod, QStringLiteral("ev"), log.events(&client, mod)), 0u);
    ASSERT_TRUE(pumpUntil([&] { return client.subscriptionGeneration(mod) == 1; }, kBudgetMs));
    ASSERT_TRUE(emitUntil(pub->provider, [&] { return log.firstEvent(QStringLiteral("p0")) >= 0; },
                          kBudgetMs));
    pump(400);   // clear of the watchdog's first tick, so only detection can see the swap

    constexpr int kSwaps = 3;
    for (int i = 1; i <= kSwaps; ++i) {
        const QString tag = QStringLiteral("p%1").arg(i);
        pub.reset();
        pub = std::make_unique<Publisher>(mod, tag);
        ASSERT_TRUE(emitUntil(pub->provider, [&] { return log.firstEvent(tag) >= 0; }, kBudgetMs))
            << "swap " << i << ": the new host's events never arrived";
        expectRestartBracketsEvents(log, tag, quint64(i) + 1);
    }
    EXPECT_EQ(log.countEdges(LogosSubscriptionEvent::Lost), kSwaps);
    EXPECT_EQ(client.subscriptionGeneration(mod), quint64(kSwaps) + 1);
}

// DETECTOR: a Manual subscription must be HELD, not silently fed by the replacement.
TEST_F(FastProviderRestartTest, ManualHoldsAcrossAFastRestart)
{
    const QString mod = QStringLiteral("fast_restart_manual_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));

    Provider p1(QStringLiteral("p1")), p2(QStringLiteral("p2"));
    SwappableHost host(mod);
    host.publish(p1);

    LogosAPIClient client(mod, QStringLiteral("caller"), &TokenManager::instance());
    client.setSubscriptionRestartPolicy(mod, LogosRestartPolicy::Manual);
    Log log;
    client.setSubscriptionStatusCallback(mod, log.status());
    const quint64 id = client.onEventWhenAvailable(mod, QStringLiteral("ev"),
                                                   log.events(&client, mod));
    ASSERT_NE(id, 0u);
    ASSERT_TRUE(pumpUntil([&] { return client.subscriptionGeneration(mod) == 1; }, kBudgetMs));
    ASSERT_TRUE(emitUntil(p1, [&] { return log.firstEvent(p1.tag) >= 0; }, kBudgetMs));

    host.replace(p1, p2);
    auto heldOrFed = [&] {
        return log.firstEdge(LogosSubscriptionEvent::Held) >= 0 || log.firstEvent(p2.tag) >= 0;
    };
    ASSERT_TRUE(emitUntil(p2, heldOrFed, kBudgetMs));
    EXPECT_GE(log.firstEdge(LogosSubscriptionEvent::Held), 0) << "the restart was not reported";

    // Held means no delivery, however long the replacement keeps emitting.
    const auto quietUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    emitUntil(p2, [&] { return std::chrono::steady_clock::now() >= quietUntil; }, 2000);
    EXPECT_LT(log.firstEvent(p2.tag), 0) << "a HELD subscription received the replacement's events";
    EXPECT_EQ(client.eventSubscriptionState(id), LogosSubscriptionState::Held);
    EXPECT_EQ(client.subscriptionGeneration(mod), 1u);

    ASSERT_TRUE(client.rearmSubscriptions(mod));
    ASSERT_TRUE(emitUntil(p2, [&] { return log.firstEvent(p2.tag) >= 0; }, kBudgetMs));
    for (const Seen& s : log.items)
        if (s.isEvent && s.tag == p2.tag) EXPECT_EQ(s.gen, 2u);
}

// DETECTOR: the C ABI sees LP_SUB_LOST, and events after it carry the new generation.
TEST_F(FastProviderRestartTest, LpClientSeesTheFastRestart)
{
    const QString mod = QStringLiteral("fast_restart_abi_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));

    Provider p1(QStringLiteral("p1")), p2(QStringLiteral("p2"));
    SwappableHost host(mod);
    host.publish(p1);

    lp_client* client = lp_client_create(mod.toUtf8().constData(), "caller", nullptr, nullptr);
    ASSERT_NE(client, nullptr);
    struct Abi {
        lp_client* client = nullptr;
        std::vector<int> states;
        std::vector<std::pair<std::string, unsigned long long>> events;   // (payload, generation)
        int lostBeforeP2 = 0;
        bool sawP1 = false, sawP2 = false;
    } abi;
    abi.client = client;

    ASSERT_EQ(lp_client_set_subscription_status_cb(
        client,
        [](int state, unsigned long long, const char*, void* ud) {
            auto* a = static_cast<Abi*>(ud);
            a->states.push_back(state);
            if (state == LP_SUB_LOST && !a->sawP2) ++a->lostBeforeP2;
        }, &abi), 1);
    lp_subscription* sub = lp_subscribe(
        client, "ev",
        [](const char*, const char* data, void* ud) {
            auto* a = static_cast<Abi*>(ud);
            a->events.emplace_back(data, lp_client_subscription_generation(a->client));
            a->sawP1 = a->sawP1 || std::strstr(data, "\"p1\"");
            a->sawP2 = a->sawP2 || std::strstr(data, "\"p2\"");
        }, &abi);
    ASSERT_NE(sub, nullptr);

    ASSERT_TRUE(pumpUntil([&] { return lp_client_subscription_generation(client) == 1; }, kBudgetMs));
    ASSERT_TRUE(emitUntil(p1, [&] { return abi.sawP1; }, kBudgetMs));

    host.replace(p1, p2);
    ASSERT_TRUE(emitUntil(p2, [&] { return abi.sawP2; }, kBudgetMs));

    EXPECT_EQ(abi.lostBeforeP2, 1) << "LP_SUB_LOST did not precede the replacement's events";
    for (const auto& [payload, gen] : abi.events)
        if (payload.find("\"p2\"") != std::string::npos)
            EXPECT_EQ(gen, 2ull) << "the replacement's event arrived under generation " << gen;
    EXPECT_EQ(abi.states.back(), LP_SUB_ARMED);

    lp_unsubscribe(sub);
    lp_client_destroy(client);
}

// DETECTOR for the transport gate, with no registry in play: a bound handle stops at its first loss,
// even though QtRO re-validates its facade; an unbound handle keeps following the replacement, as before.
TEST_F(FastProviderRestartTest, ABoundHandleStopsAtTheLossAnUnboundOneDoesNot)
{
    const QString mod = QStringLiteral("fast_restart_bound_module");
    Provider p1(QStringLiteral("p1")), p2(QStringLiteral("p2"));
    QtSwappableHost host(mod);
    host.publish(p1);

    RemoteTransportConnection conn(LogosInstance::id(mod));
    ASSERT_TRUE(conn.connectToHost());
    LogosObject* bound = conn.requestObject(mod, 5000);
    LogosObject* unbound = conn.requestObject(mod, 5000);
    ASSERT_NE(bound, nullptr);
    ASSERT_NE(unbound, nullptr);
    auto* watch = dynamic_cast<LogosObjectSourceWatch*>(bound);
    ASSERT_NE(watch, nullptr);

    int losses = 0;
    watch->bindToSource([&] { ++losses; });
    ASSERT_FALSE(watch->sourceLost());
    QStringList boundSeen, unboundSeen;
    bound->onEvent(QStringLiteral("ev"), [&](const QString&, const QVariantList& d) {
        boundSeen << d.value(0).toString();
    });
    unbound->onEvent(QStringLiteral("ev"), [&](const QString&, const QVariantList& d) {
        unboundSeen << d.value(0).toString();
    });
    ASSERT_TRUE(emitUntil(p1, [&] {
        return boundSeen.contains(p1.tag) && unboundSeen.contains(p1.tag);
    }, kBudgetMs));

    host.replace(p1, p2);
    ASSERT_TRUE(emitUntil(p2, [&] { return unboundSeen.contains(p2.tag); }, kBudgetMs))
        << "an unbound handle no longer follows the replacement";

    EXPECT_FALSE(boundSeen.contains(p2.tag)) << "a bound handle delivered the replacement's events";
    EXPECT_EQ(losses, 1);
    EXPECT_TRUE(watch->sourceLost());
    EXPECT_TRUE(bound->isValid()) << "QtRO re-validated the facade; isValid() alone cannot see the swap";

    bound->release();
    unbound->release();
    pump(50);
}

// DETECTOR: a subscriber joining an established module gets its own onArmed, but no target edge or generation.
TEST_F(FastProviderRestartTest, ALateSubscriberJoinsTheCurrentGeneration)
{
    const QString mod = QStringLiteral("fast_restart_late_joiner_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));

    Provider p1(QStringLiteral("p1")), p2(QStringLiteral("p2"));
    SwappableHost host(mod);
    host.publish(p1);

    LogosAPIClient client(mod, QStringLiteral("caller"), &TokenManager::instance());
    Log log;
    client.setSubscriptionStatusCallback(mod, log.status());
    ASSERT_NE(client.onEventWhenAvailable(mod, QStringLiteral("ev"), log.events(&client, mod)), 0u);
    ASSERT_TRUE(pumpUntil([&] { return client.subscriptionGeneration(mod) == 1; }, kBudgetMs));

    int lateArmed = 0;
    QStringList lateSeen;
    const quint64 late = client.onEventWhenAvailable(
        mod, QStringLiteral("ev"),
        [&](const QString&, const QVariantList& d) { lateSeen << d.value(0).toString(); },
        [&](bool ok) { lateArmed += ok ? 1 : 0; });
    for (int i = 0; i < 3; ++i) {
        const quint64 extra = client.onEventWhenAvailable(
            mod, QStringLiteral("other"), [](const QString&, const QVariantList&) {});
        pump(20);
        EXPECT_TRUE(client.cancelEventSubscription(extra));
    }
    ASSERT_TRUE(emitUntil(p1, [&] { return lateSeen.contains(p1.tag); }, kBudgetMs));
    EXPECT_EQ(lateArmed, 1);
    EXPECT_EQ(client.eventSubscriptionState(late), LogosSubscriptionState::Armed);
    EXPECT_EQ(client.subscriptionGeneration(mod), 1u) << "a late subscriber looked like a restart";
    EXPECT_EQ(log.countEdges(LogosSubscriptionEvent::Armed), 1);

    // A real restart still advances once, for everything armed on the module.
    host.replace(p1, p2);
    ASSERT_TRUE(emitUntil(p2, [&] {
        return lateSeen.contains(p2.tag) && log.firstEvent(p2.tag) >= 0;
    }, kBudgetMs));
    expectRestartBracketsEvents(log, p2.tag, 2);
    EXPECT_EQ(log.countEdges(LogosSubscriptionEvent::Armed), 2);
}

// DETECTOR: the same through the C ABI, whose generation the header tells plain lp_subscribe callers to watch.
TEST_F(FastProviderRestartTest, LpSubscribeOnAnArmedClientKeepsTheGeneration)
{
    const QString mod = QStringLiteral("fast_restart_abi_late_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));

    Provider p1(QStringLiteral("p1"));
    SwappableHost host(mod);
    host.publish(p1);

    lp_client* client = lp_client_create(mod.toUtf8().constData(), "caller", nullptr, nullptr);
    ASSERT_NE(client, nullptr);
    std::vector<int> states;
    ASSERT_EQ(lp_client_set_subscription_status_cb(
        client,
        [](int state, unsigned long long, const char*, void* ud) {
            static_cast<std::vector<int>*>(ud)->push_back(state);
        }, &states), 1);
    auto noop = [](const char*, const char*, void*) {};
    lp_subscription* first = lp_subscribe(client, "ev", noop, nullptr);
    ASSERT_NE(first, nullptr);
    ASSERT_TRUE(pumpUntil([&] { return lp_client_subscription_generation(client) == 1; }, kBudgetMs));

    lp_subscription* second = lp_subscribe(client, "ev", noop, nullptr);
    lp_subscription* third = lp_subscribe(client, "other", noop, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(third, nullptr);
    pump(300);
    EXPECT_EQ(lp_client_subscription_generation(client), 1ull)
        << "a second lp_subscribe read as a provider restart";
    EXPECT_EQ(std::count(states.begin(), states.end(), LP_SUB_ARMED), 1);

    lp_unsubscribe(third);
    lp_unsubscribe(second);
    lp_unsubscribe(first);
    lp_client_destroy(client);
}

// DETECTOR for rearm()'s replay: reviving onto a target another subscriber already re-established still answers ARMED.
TEST_F(FastProviderRestartTest, RearmOntoAReestablishedTargetStillReportsArmed)
{
    const QString mod = QStringLiteral("fast_restart_rearm_join_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));

    Provider p1(QStringLiteral("p1")), p2(QStringLiteral("p2"));
    SwappableHost host(mod);
    host.publish(p1);

    LogosAPIClient client(mod, QStringLiteral("caller"), &TokenManager::instance());
    client.setSubscriptionRestartPolicy(mod, LogosRestartPolicy::Manual);
    Log log;
    client.setSubscriptionStatusCallback(mod, log.status());
    const quint64 held = client.onEventWhenAvailable(mod, QStringLiteral("ev"),
                                                     log.events(&client, mod));
    ASSERT_TRUE(pumpUntil([&] { return client.subscriptionGeneration(mod) == 1; }, kBudgetMs));

    host.replace(p1, p2);
    ASSERT_TRUE(pumpUntil([&] {
        return client.eventSubscriptionState(held) == LogosSubscriptionState::Held;
    }, kBudgetMs));

    // A new subscriber re-establishes the target while the first one stays held.
    ASSERT_NE(client.onEventWhenAvailable(mod, QStringLiteral("other"),
                                          [](const QString&, const QVariantList&) {}), 0u);
    ASSERT_TRUE(pumpUntil([&] { return client.subscriptionGeneration(mod) == 2; }, kBudgetMs));
    EXPECT_EQ(client.eventSubscriptionState(held), LogosSubscriptionState::Held);

    const int armedBefore = log.countEdges(LogosSubscriptionEvent::Armed);
    ASSERT_TRUE(client.rearmSubscriptions(mod));
    EXPECT_EQ(log.countEdges(LogosSubscriptionEvent::Armed), armedBefore + 1)
        << "rearm() revived the subscription without answering ARMED";
    EXPECT_EQ(log.items.back().gen, 2u);
    EXPECT_EQ(client.subscriptionGeneration(mod), 2u);
    EXPECT_EQ(client.eventSubscriptionState(held), LogosSubscriptionState::Armed);
    ASSERT_TRUE(emitUntil(p2, [&] { return log.firstEvent(p2.tag) >= 0; }, kBudgetMs));
}

// CONTROL: calls and extra handles on the same module are not a restart.
TEST_F(FastProviderRestartTest, ChurnOnAHealthyModuleIsNotARestart)
{
    const QString mod = QStringLiteral("fast_restart_churn_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));

    Provider p1(QStringLiteral("p1"));
    SwappableHost host(mod);
    host.publish(p1);

    LogosAPIClient client(mod, QStringLiteral("caller"), &TokenManager::instance());
    Log log;
    client.setSubscriptionStatusCallback(mod, log.status());
    ASSERT_NE(client.onEventWhenAvailable(mod, QStringLiteral("ev"), log.events(&client, mod)), 0u);
    ASSERT_TRUE(pumpUntil([&] { return client.subscriptionGeneration(mod) == 1; }, kBudgetMs));

    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(client.invokeRemoteMethod(mod, QStringLiteral("echo"), QVariantList{i},
                                            Timeout(5000)).toInt(), i);
        if (LogosObject* raw = client.requestObject(mod, Timeout(5000))) raw->release();
        pump(50);
    }
    pump(2500);   // more than two liveness ticks

    EXPECT_TRUE(emitUntil(p1, [&] { return log.firstEvent(p1.tag) >= 0; }, kBudgetMs));
    EXPECT_EQ(log.countEdges(LogosSubscriptionEvent::Lost), 0);
    EXPECT_EQ(log.countEdges(LogosSubscriptionEvent::Armed), 1);
    EXPECT_EQ(client.subscriptionGeneration(mod), 1u);
    for (const Seen& s : log.items)
        if (s.isEvent) EXPECT_EQ(s.gen, 1u);
}
