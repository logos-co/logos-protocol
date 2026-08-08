// The event-delivery regression matrix.
//
// WHY THIS FILE EXISTS. Event delivery in this codebase has broken silently
// three separate times, each time in a cell nothing covered: a one-shot
// subscription that refused a module which was merely not up YET; a deferred
// subscription that never armed on the three transports whose tests were all
// written in Remote mode; a plain-transport host that accepted a Subscribe for
// an unpublished object and dropped it on the floor. Every one of them looked
// like success from the caller's side. A single happy-path test per layer
// cannot catch that class of defect, because the defect IS the happy path
// reporting success.
//
// So this pins the product, not a sample of it:
//
//   TRANSPORT   x  PROVIDER      x  CONSUMER              x  TIMING
//   ---------      --------         --------                 ------
//   qt_remote      Qt-native        onEventWhenAvailable     publish-then-subscribe
//   qt_local       universal/std    lp_subscribe (C ABI)     subscribe-then-publish
//   plain (TCP)                                              unload -> reload
//                                                            reconnect
//                                                            unsubscribe
//
// The two provider kinds are not decoration: a Qt module stores the Qt-side
// callback verbatim, while every Rust cdylib / Nim / universal C++ module emits
// through setEventListenerStdBridge, a different conversion with its own
// history of dropping payloads. The two consumer paths are not decoration
// either: QML reaches events through onEventWhenAvailable, and every non-Qt
// module and ui_qml backend reaches them through lp_subscribe.
//
// mock is deliberately NOT in the matrix: MockLogosObject::onEvent is a
// documented no-op, so mock mode delivers no events by design. It gets its own
// case at the bottom pinning what it DOES have to do — arm immediately and
// never sit pending — because a regression that left mock subscriptions
// permanently deferred would otherwise be invisible.
//
// EVERY delivery case has a control that is green independently of any of these
// fixes. A red test with no control cannot tell "the defect" from "the fixture
// is mis-wired", and a count of zero with nothing to compare it against is not
// evidence.

#include <gtest/gtest.h>

#include "logos_api_client.h"
#include "logos_instance.h"
#include "logos_mode.h"
#include "logos_object.h"
#include "logos_protocol.h"
#include "logos_provider_interface.h"
#include "logos_transport_config.h"
#include "module_proxy.h"
#include "plain_transport_host.h"
#include "local_transport.h"
#include "remote_transport.h"
#include "token_manager.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QString>
#include <QVariantList>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace {

QCoreApplication* ensureApp() {
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance())
        new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

void pump(int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// ── providers ────────────────────────────────────────────────────────────────

// A Qt-native module: setEventListener keeps the Qt-side callback as-is. This
// is what a C++/Qt module built with logos-qt-sdk looks like to ModuleProxy.
class QtProvider : public LogosProviderObject {
public:
    EventCallback emitFn;
    QVariant callMethod(const QString& m, const QVariantList& a) override {
        return (m == QLatin1String("echo") && !a.isEmpty()) ? a.first() : QVariant();
    }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    QJsonArray getMethods() override { return QJsonArray{}; }
    void setEventListener(EventCallback cb) override { emitFn = std::move(cb); }
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("qt_provider"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }
};

// A universal module: it only knows std types, and its events reach Qt through
// setEventListenerStdBridge -> setEventListenerStd, serialising the payload as
// JSON. Every Rust cdylib, Nim and universal C++ module emits through this
// path, so a defect that only affects it would be invisible to a Qt-only test.
class UniversalProvider : public LogosProviderObject {
public:
    UniversalEventCallback stdEmitFn;
    QVariant callMethod(const QString& m, const QVariantList& a) override {
        return callMethodStdBridge(m, a);
    }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    QJsonArray getMethods() override { return QJsonArray{}; }
    void setEventListener(EventCallback cb) override {
        setEventListenerStdBridge(std::move(cb));
    }
    void setEventListenerStd(UniversalEventCallback cb) override {
        stdEmitFn = std::move(cb);
    }
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("universal_provider"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }
};

enum class ProviderKind { Qt, Universal };
enum class TransportKind { QtRemote, QtLocal, Plain };

const char* nameOf(ProviderKind p) { return p == ProviderKind::Qt ? "QtProvider" : "UniversalProvider"; }

// Does a subscription to an ABSENT module sit in the consumer's pending set?
//
// Stated per transport rather than discovered at runtime, because the whole
// point is to catch a change in it. The two answers are both correct and they
// arm the subscription in genuinely different places:
//
//   qt_remote  DEFERS. No listener on the socket, so there is no handle to
//              attach to; the consumer holds the subscription and the QtRO
//              replica arms it when the peer appears.
//   qt_local   DEFERS. The module is not in the in-process PluginRegistry, so
//              requestObject() returns null and the retry tick owns the arm.
//   plain      DOES NOT. requestObject() hands back a handle for any name over
//              a live connection, so the subscription arms immediately and the
//              waiting happens one layer down: PlainTransportHost keeps the
//              sink until the object is published. If this ever flips to true,
//              that host-side hold has been lost and subscribe-before-publish
//              is silently broken again on TCP/TLS.
bool defersWhenModuleAbsent(TransportKind t) { return t != TransportKind::Plain; }

// Does a subscription to a module that is ALREADY up arm before the call
// returns? Everywhere except qt_remote, yes — and it has to.
//
// Those transports have no deferred acquire, so if the first attempt is left to
// the 250 ms retry tick, every event the module emits in that window is
// dropped. The C ABI used to attach synchronously and deliver them, so making
// this asynchronous would not remove the silent event loss, it would relocate
// it. qt_remote is the exception on purpose: its acquire completes on a later
// event-loop turn because delivering inline would run user code on QtRO's read
// stack, which is a documented crash in this codebase, not a style preference.
bool armsSynchronouslyWhenPresent(TransportKind t) { return t != TransportKind::QtRemote; }

// Is a subscription live the instant it is made, on a module that is already
// reachable? Only where registering it is LOCAL to this process:
//   qt_remote  attaches a callback to a replica this node already holds
//   qt_local   connects to the provider's Qt signal, in-process
//   plain      NO -- onSubscribe travels to the host as a wire frame, so an
//              event emitted before that frame lands reaches nobody. That is
//              inherent to a network transport and predates deferral: the old
//              path sent the identical frame. Delivery must still HAPPEN, just
//              not instantly, which is what the plain leg below asserts.
bool subscriptionIsLocallyRegistered(TransportKind t) { return t != TransportKind::Plain; }
const char* nameOf(TransportKind t) {
    switch (t) {
    case TransportKind::QtRemote: return "qt_remote";
    case TransportKind::QtLocal:  return "qt_local";
    case TransportKind::Plain:    return "plain_tcp";
    }
    return "?";
}

// ── the module under test, brought up and taken down on demand ───────────────
//
// One class per transport would triple the file for no gain; the differences
// are three lines each. What "down" MEANS differs per transport and is stated
// where it differs:
//   qt_remote  no host listening on the module's socket at all
//   qt_local   not registered in the in-process PluginRegistry
//   plain      host listening, object not published (the realistic case: a
//              host process serves several modules and publishes them as they
//              finish initialising)
class Module {
public:
    Module(TransportKind transport, ProviderKind provider, QString name)
        : m_transport(transport), m_name(std::move(name))
    {
        if (provider == ProviderKind::Qt) {
            m_qt = std::make_unique<QtProvider>();
            m_proxy = std::make_unique<ModuleProxy>(m_qt.get());
        } else {
            m_universal = std::make_unique<UniversalProvider>();
            m_proxy = std::make_unique<ModuleProxy>(m_universal.get());
        }
        m_proxy->saveToken(QStringLiteral("caller"), QStringLiteral("tok"));

        if (m_transport == TransportKind::Plain) {
            LogosTransportConfig cfg;
            cfg.protocol = LogosProtocol::Tcp;
            cfg.host = "127.0.0.1";
            cfg.port = 0;                       // ephemeral
            m_plainHost = std::make_unique<logos::plain::PlainTransportHost>(cfg);
            m_plainStarted = m_plainHost->start();
            const QString endpoint = m_plainHost->endpoint();
            m_port = endpoint.mid(endpoint.lastIndexOf(':') + 1).toUShort();
        }
    }

    ~Module() { takeDown(); }

    bool hostReady() const {
        return m_transport != TransportKind::Plain || (m_plainStarted && m_port != 0);
    }

    void bringUp()
    {
        if (m_up) return;
        switch (m_transport) {
        case TransportKind::QtRemote:
            m_remoteHost = std::make_unique<RemoteTransportHost>(LogosInstance::id(m_name));
            m_remoteHost->publishObject(m_name, m_proxy.get());
            break;
        case TransportKind::QtLocal:
            m_localHost = std::make_unique<LocalTransportHost>();
            m_localHost->publishObject(m_name, m_proxy.get());
            break;
        case TransportKind::Plain:
            m_plainHost->publishObject(m_name, m_proxy.get());
            break;
        }
        m_up = true;
    }

    void takeDown()
    {
        if (!m_up) return;
        switch (m_transport) {
        case TransportKind::QtRemote: m_remoteHost.reset(); break;
        case TransportKind::QtLocal:  m_localHost->unpublishObject(m_name); m_localHost.reset(); break;
        case TransportKind::Plain:    m_plainHost->unpublishObject(m_name); break;
        }
        m_up = false;
    }

    // True once the provider has been handed its event listener — i.e. the
    // module can actually emit. Firing before this point emits into nothing,
    // which would make a test measure its own setup.
    bool canEmit() const
    {
        return m_qt ? static_cast<bool>(m_qt->emitFn)
                    : static_cast<bool>(m_universal->stdEmitFn);
    }

    // Emit `ev` carrying a single integer. The two providers reach the wire by
    // different routes on purpose (see the class comments above).
    void emitEvent(const QString& ev, int payload)
    {
        if (!canEmit()) return;
        if (m_qt) m_qt->emitFn(ev, QVariantList{ payload });
        else      m_universal->stdEmitFn(ev.toStdString(), "[" + std::to_string(payload) + "]");
    }

    // The transport config a consumer needs to reach this module. Empty (the
    // process default) for everything but plain, whose port is ephemeral.
    LogosTransportConfig clientConfig() const
    {
        LogosTransportConfig cfg;
        if (m_transport == TransportKind::Plain) {
            cfg.protocol = LogosProtocol::Tcp;
            cfg.host = "127.0.0.1";
            cfg.port = m_port;
        }
        return cfg;
    }

    // The same thing in the JSON form lp_client_create takes; nullptr means
    // "process default", which is right for everything but plain.
    std::string lpEndpoint() const
    {
        if (m_transport != TransportKind::Plain) return std::string();
        return "{\"protocol\":\"tcp\",\"host\":\"127.0.0.1\",\"port\":"
             + std::to_string(m_port) + "}";
    }

    const QString& name() const { return m_name; }

private:
    TransportKind m_transport;
    QString m_name;
    bool m_up = false;

    std::unique_ptr<QtProvider> m_qt;
    std::unique_ptr<UniversalProvider> m_universal;
    std::unique_ptr<ModuleProxy> m_proxy;

    std::unique_ptr<RemoteTransportHost> m_remoteHost;
    std::unique_ptr<LocalTransportHost> m_localHost;
    std::unique_ptr<logos::plain::PlainTransportHost> m_plainHost;
    bool m_plainStarted = false;
    uint16_t m_port = 0;
};

// Re-fire on a cadence rather than once. NO transport here buffers events, so a
// single shot would turn every test into a race on whether the emit happened to
// land after the subscription armed — measuring the harness, not the protocol.
// Returns true as soon as the counter moves.
bool fireUntilDelivered(Module& mod, const QString& ev, int payload,
                        std::atomic<int>& counter, int budgetMs)
{
    QElapsedTimer t; t.start();
    while (counter.load() == 0 && t.elapsed() < budgetMs) {
        mod.emitEvent(ev, payload);
        pump(50);
    }
    return counter.load() > 0;
}

bool pumpUntil(const std::function<bool()>& done, int budgetMs)
{
    QElapsedTimer t; t.start();
    while (!done() && t.elapsed() < budgetMs) pump(20);
    return done();
}

std::unique_ptr<LogosAPIClient> makeClient(const Module& mod)
{
    TokenManager::instance().saveToken(mod.name(), QStringLiteral("tok"));
    const LogosTransportConfig cfg = mod.clientConfig();
    return std::make_unique<LogosAPIClient>(mod.name(), QStringLiteral("caller"),
                                            &TokenManager::instance(), cfg, cfg);
}

lp_client* makeLpClient(const Module& mod)
{
    const std::string ep = mod.lpEndpoint();
    const char* epArg = ep.empty() ? nullptr : ep.c_str();
    return lp_client_create(mod.name().toUtf8().constData(), "caller", epArg, epArg);
}

std::string lpPending(lp_client* client)
{
    char* json = lp_pending_subscriptions(client);
    std::string out = json ? json : "<null>";
    lp_string_free(json);
    return out;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────

struct MatrixCase {
    TransportKind transport;
    ProviderKind provider;
};

class EventDeliveryMatrix : public ::testing::TestWithParam<MatrixCase> {
protected:
    void SetUp() override
    {
        ensureApp();
        // The mode is process-global and is read when the consumer is built, so
        // it has to be set before any client in the case is constructed.
        LogosModeConfig::setMode(GetParam().transport == TransportKind::QtLocal
                                     ? LogosMode::Local
                                     : LogosMode::Remote);
    }
    void TearDown() override { LogosModeConfig::setMode(LogosMode::Remote); }

    // Distinct per case AND per test: LogosInstance::id and PluginRegistry are
    // process-global, so a shared name lets one case attach to another case's
    // still-live host and pass for the wrong reason.
    QString moduleName(const char* stem) const
    {
        return QStringLiteral("m_%1_%2_%3")
            .arg(QLatin1String(nameOf(GetParam().transport)))
            .arg(GetParam().provider == ProviderKind::Qt ? "qt" : "uni")
            .arg(QLatin1String(stem));
    }

    Module makeModule(const char* stem) const
    {
        return Module(GetParam().transport, GetParam().provider, moduleName(stem));
    }
};

// ── CONTROL: the module is already up when the subscription is made ──────────
//
// Green before and after every fix in this area. If this is ever red, the
// fixture is broken and no other verdict in the file means anything.
TEST_P(EventDeliveryMatrix, PublishThenSubscribe_Delivers)
{
    Module mod = makeModule("ctl");
    ASSERT_TRUE(mod.hostReady());
    mod.bringUp();

    auto client = makeClient(mod);
    std::atomic<int> got{0};
    QVariantList payload;
    const quint64 id = client->onEventWhenAvailable(mod.name(), QStringLiteral("ev"),
        [&](const QString&, const QVariantList& d) { payload = d; got.fetch_add(1); });
    ASSERT_NE(id, 0u);

    // Pinned BEFORE any pumping: on the transports with no deferred acquire the
    // subscription must be live the moment the call returns, or the events the
    // module emits before the first retry tick are silently lost.
    if (armsSynchronouslyWhenPresent(GetParam().transport)) {
        EXPECT_EQ(client->eventSubscriptionState(id), LogosSubscriptionState::Armed)
            << "an already-present module was deferred to the retry tick -- every "
               "event emitted in that window is dropped";
        EXPECT_TRUE(client->pendingEventSubscriptions().isEmpty());
    }

    ASSERT_TRUE(pumpUntil([&] { return mod.canEmit(); }, 5000)) << "provider never got its listener";
    ASSERT_TRUE(fireUntilDelivered(mod, QStringLiteral("ev"), 42, got, 10000));
    ASSERT_EQ(payload.size(), 1);
    EXPECT_EQ(payload[0].toInt(), 42);
}

// ── THE DEFECT: subscribe first, the module shows up second ──────────────────
//
// A UI plugin's Component.onCompleted, a module's init(), a backend's
// onContextReady() all run here. The pre-fix code answered "not connected" and
// never asked again.
TEST_P(EventDeliveryMatrix, SubscribeThenPublish_Delivers)
{
    Module mod = makeModule("late");
    ASSERT_TRUE(mod.hostReady());
    // NOT brought up yet.

    auto client = makeClient(mod);
    std::atomic<int> got{0};
    const quint64 id = client->onEventWhenAvailable(mod.name(), QStringLiteral("ev"),
        [&](const QString&, const QVariantList&) { got.fetch_add(1); });
    ASSERT_NE(id, 0u);

    // Registered, and it SAYS so rather than vanishing. That durable record is
    // half the fix: the old failure was indistinguishable from success.
    EXPECT_NE(client->eventSubscriptionState(id), LogosSubscriptionState::Unknown);

    pump(300);
    mod.bringUp();

    ASSERT_TRUE(pumpUntil([&] { return mod.canEmit(); }, 5000));
    ASSERT_TRUE(fireUntilDelivered(mod, QStringLiteral("ev"), 7, got, 10000))
        << "a subscription made before the module appeared never armed";
    EXPECT_EQ(client->eventSubscriptionState(id), LogosSubscriptionState::Armed);
    EXPECT_TRUE(client->pendingEventSubscriptions().isEmpty()) << "armed but still reported pending";
}

// ── The C ABI, same two cases ────────────────────────────────────────────────
//
// lp_subscribe is how every non-Qt module and every ui_qml backend inside
// ui-host reaches events. A fix that only covered the Qt consumer would leave
// all of them broken, which is exactly what happened the first time.
TEST_P(EventDeliveryMatrix, LpSubscribe_PublishThenSubscribe_Control)
{
    Module mod = makeModule("lpctl");
    ASSERT_TRUE(mod.hostReady());
    mod.bringUp();

    lp_client* client = makeLpClient(mod);
    ASSERT_NE(client, nullptr);

    std::atomic<int> got{0};
    lp_subscription* sub = lp_subscribe(client, "ev",
        [](const char*, const char*, void* ud) { static_cast<std::atomic<int>*>(ud)->fetch_add(1); },
        &got);
    ASSERT_NE(sub, nullptr);

    ASSERT_TRUE(pumpUntil([&] { return mod.canEmit(); }, 5000));
    EXPECT_TRUE(fireUntilDelivered(mod, QStringLiteral("ev"), 1, got, 10000));

    lp_unsubscribe(sub);
    lp_client_destroy(client);
}

TEST_P(EventDeliveryMatrix, LpSubscribe_SubscribeThenPublish_Delivers)
{
    Module mod = makeModule("lplate");
    ASSERT_TRUE(mod.hostReady());

    lp_client* client = makeLpClient(mod);
    ASSERT_NE(client, nullptr);

    std::atomic<int> got{0};
    lp_subscription* sub = lp_subscribe(client, "ev",
        [](const char*, const char*, void* ud) { static_cast<std::atomic<int>*>(ud)->fetch_add(1); },
        &got);
    ASSERT_NE(sub, nullptr) << "lp_subscribe refused a module that is merely not up yet";

    pump(300);
    mod.bringUp();

    ASSERT_TRUE(pumpUntil([&] { return mod.canEmit(); }, 5000));
    EXPECT_TRUE(fireUntilDelivered(mod, QStringLiteral("ev"), 1, got, 10000))
        << "lp_subscribe subscription never armed";

    lp_unsubscribe(sub);
    lp_client_destroy(client);
}

// ── The module goes away and comes back ──────────────────────────────────────
//
// The package manager's core flow, and the shape that used to silently kill an
// armed subscription: the handle for the object goes invalid, and replacing it
// released the event helper every armed subscription was attached to.
TEST_P(EventDeliveryMatrix, SubscriptionSurvivesUnloadAndReload)
{
    Module mod = makeModule("reload");
    ASSERT_TRUE(mod.hostReady());
    mod.bringUp();

    auto client = makeClient(mod);
    std::atomic<int> got{0};
    const quint64 id = client->onEventWhenAvailable(mod.name(), QStringLiteral("ev"),
        [&](const QString&, const QVariantList&) { got.fetch_add(1); });
    ASSERT_NE(id, 0u);

    ASSERT_TRUE(pumpUntil([&] { return mod.canEmit(); }, 5000));
    ASSERT_TRUE(fireUntilDelivered(mod, QStringLiteral("ev"), 1, got, 10000)) << "control leg never delivered";

    mod.takeDown();
    pump(300);
    mod.bringUp();

    std::atomic<int> after{0};
    // Re-subscribing is NOT part of the contract under test — the point is that
    // the ORIGINAL subscription still fires.
    ASSERT_TRUE(pumpUntil([&] { return mod.canEmit(); }, 5000));
    QElapsedTimer t; t.start();
    while (after.load() == 0 && t.elapsed() < 10000) {
        const int before = got.load();
        mod.emitEvent(QStringLiteral("ev"), 2);
        pump(50);
        if (got.load() > before) after.fetch_add(1);
    }
    EXPECT_GT(after.load(), 0)
        << "the subscription was silently killed by the module reload -- it still "
           "reports healthy, which is the failure shape this whole area exists to remove";
    EXPECT_NE(client->eventSubscriptionState(id), LogosSubscriptionState::Unknown);
}

// ── Cancellation actually un-registers ───────────────────────────────────────
//
// An unsubscribed-while-pending subscription used to stay in the registry
// forever: holding the retry timer up, emitting the 3 s / 60 s watchdog
// warnings about a subscription nobody wanted, and showing in the very
// diagnostics this design leans on to be credible.
TEST_P(EventDeliveryMatrix, CancelWhilePending_LeavesTheRegistry)
{
    Module mod = makeModule("cancel");
    ASSERT_TRUE(mod.hostReady());
    // Never brought up: the subscription stays pending.

    auto client = makeClient(mod);
    std::atomic<int> got{0};
    const quint64 id = client->onEventWhenAvailable(mod.name(), QStringLiteral("ev"),
        [&](const QString&, const QVariantList&) { got.fetch_add(1); });
    ASSERT_NE(id, 0u);
    // Control: it has to be TRACKED for un-tracking it to mean anything. Which
    // of the two live states it is in is a property of the transport, pinned
    // here so a change to it is a test failure rather than a surprise.
    const LogosSubscriptionState expected = defersWhenModuleAbsent(GetParam().transport)
                                                ? LogosSubscriptionState::Pending
                                                : LogosSubscriptionState::Armed;
    ASSERT_EQ(client->eventSubscriptionState(id), expected);
    ASSERT_EQ(client->pendingEventSubscriptions().isEmpty(),
              expected == LogosSubscriptionState::Armed);

    EXPECT_TRUE(client->cancelEventSubscription(id));
    EXPECT_EQ(client->eventSubscriptionState(id), LogosSubscriptionState::Unknown);
    EXPECT_TRUE(client->pendingEventSubscriptions().isEmpty());
    EXPECT_FALSE(client->cancelEventSubscription(id)) << "cancelling twice should report 'not known'";
}

// lp_unsubscribe has to reach that same cancellation, or the C ABI leaks a
// registry entry per subscription for the life of the process.
TEST_P(EventDeliveryMatrix, LpUnsubscribeWhilePending_LeavesTheRegistry)
{
    Module mod = makeModule("lpcancel");
    ASSERT_TRUE(mod.hostReady());

    lp_client* client = makeLpClient(mod);
    ASSERT_NE(client, nullptr);

    std::atomic<int> got{0};
    lp_subscription* sub = lp_subscribe(client, "ev",
        [](const char*, const char*, void* ud) { static_cast<std::atomic<int>*>(ud)->fetch_add(1); },
        &got);
    ASSERT_NE(sub, nullptr);

    // Control: on the transports that defer, the C ABI must be able to SEE the
    // pending subscription. That visibility is the point — it did not exist
    // before, which is why a subscription that silently never armed was
    // undetectable from Rust, Nim or a universal C++ module.
    const std::string expectedPending =
        defersWhenModuleAbsent(GetParam().transport)
            ? "[\"" + mod.name().toStdString() + "::ev\"]"
            : "[]";
    EXPECT_EQ(lpPending(client), expectedPending);

    lp_unsubscribe(sub);

    // Un-registration is EVENTUAL by design — it is posted to the owner thread
    // rather than done under a lock that thread's delivery callback also takes,
    // which would deadlock. So pump; what must hold is that it drains, not that
    // it drained by the time lp_unsubscribe returned.
    EXPECT_TRUE(pumpUntil([&] { return lpPending(client) == "[]"; }, 5000))
        << "lp_unsubscribe left the entry in the registry: it keeps the retry timer "
           "alive and keeps warning about a subscription nobody wants. Still pending: "
        << lpPending(client);

    // The ABI's own promise, and the half that IS observable on every transport
    // including the ones that armed immediately: bring the module up, fire
    // repeatedly, and the cancelled callback must stay silent. Without this the
    // plain leg above asserts "[] before, [] after" and proves nothing.
    mod.bringUp();
    ASSERT_TRUE(pumpUntil([&] { return mod.canEmit(); }, 5000));
    for (int i = 0; i < 20; ++i) { mod.emitEvent(QStringLiteral("ev"), 9); pump(25); }
    EXPECT_EQ(got.load(), 0) << "the callback fired after lp_unsubscribe returned";

    lp_client_destroy(client);
}

// ── A reconnect must re-arm, or say it could not ─────────────────────────────
//
// reconnect() tears the connection down and rebuilds it, so every handle a
// subscription holds is dead. Re-arming is one half; the other is that the
// retry machinery is running again afterwards — leaving it stopped is a
// subscription that is both dead AND silent, which is strictly worse than the
// bug it replaced.
TEST_P(EventDeliveryMatrix, ReconnectReArmsAnArmedSubscription)
{
    Module mod = makeModule("recon");
    ASSERT_TRUE(mod.hostReady());
    mod.bringUp();

    auto client = makeClient(mod);
    std::atomic<int> got{0};
    const quint64 id = client->onEventWhenAvailable(mod.name(), QStringLiteral("ev"),
        [&](const QString&, const QVariantList&) { got.fetch_add(1); });
    ASSERT_NE(id, 0u);

    ASSERT_TRUE(pumpUntil([&] { return mod.canEmit(); }, 5000));
    ASSERT_TRUE(fireUntilDelivered(mod, QStringLiteral("ev"), 1, got, 10000))
        << "control leg never delivered -- nothing after this means anything";

    ASSERT_TRUE(client->reconnect());

    std::atomic<int> after{0};
    QElapsedTimer t; t.start();
    while (after.load() == 0 && t.elapsed() < 10000) {
        const int before = got.load();
        mod.emitEvent(QStringLiteral("ev"), 3);
        pump(50);
        if (got.load() > before) after.fetch_add(1);
    }
    EXPECT_GT(after.load(), 0)
        << "reconnect() left the subscription permanently dead. It is also SILENT: "
           "the watchdog only runs from the retry tick, so nothing ever says so.";
}

// The case the reconnect path actually gets wrong: reconnecting while the
// module is DOWN. Re-arming immediately is easy — the object is still there. If
// it is not, the subscription goes back into the pending set, and unless the
// retry machinery is running again it sits there forever: no retry, and no
// watchdog either, because the watchdog only speaks from the retry tick. That
// is a subscription which is dead AND silent, strictly worse than the "not
// connected" warning it replaced.
TEST_P(EventDeliveryMatrix, ReconnectWhileModuleIsDown_StillReArmsWhenItReturns)
{
    Module mod = makeModule("recondown");
    ASSERT_TRUE(mod.hostReady());
    mod.bringUp();

    auto client = makeClient(mod);
    std::atomic<int> got{0};
    const quint64 id = client->onEventWhenAvailable(mod.name(), QStringLiteral("ev"),
        [&](const QString&, const QVariantList&) { got.fetch_add(1); });
    ASSERT_NE(id, 0u);

    ASSERT_TRUE(pumpUntil([&] { return mod.canEmit(); }, 5000));
    ASSERT_TRUE(fireUntilDelivered(mod, QStringLiteral("ev"), 1, got, 10000))
        << "control leg never delivered -- nothing after this means anything";

    mod.takeDown();
    pump(200);
    ASSERT_TRUE(client->reconnect());
    pump(200);
    mod.bringUp();

    std::atomic<int> after{0};
    ASSERT_TRUE(pumpUntil([&] { return mod.canEmit(); }, 5000));
    QElapsedTimer t; t.start();
    while (after.load() == 0 && t.elapsed() < 10000) {
        const int before = got.load();
        mod.emitEvent(QStringLiteral("ev"), 4);
        pump(50);
        if (got.load() > before) after.fetch_add(1);
    }
    EXPECT_GT(after.load(), 0)
        << "a subscription that was pending across a reconnect never armed again";
}

// ── the call-then-subscribe shape ────────────────────────────────────────────
//
// The most common real consumer: talk to a module, then subscribe to it in the
// same function. wallet-ui's backend calls get_chains() and subscribes on the
// next line; the tutorial's C++ UI backend does the same.
//
// Before deferral the generated Qt wrapper acquired synchronously, so the
// subscription was live before on() returned and an event emitted immediately
// after was delivered. Deferring to the next event-loop turn silently drops it
// -- the same event loss this area exists to remove, in a narrower window. The
// event is fired ONCE, synchronously, with no pumping in between, because
// re-firing would hide exactly the gap under test.
TEST_P(EventDeliveryMatrix, SubscribeRightAfterACall_DeliversImmediately)
{
    Module mod = makeModule("aftercall");
    ASSERT_TRUE(mod.hostReady());
    mod.bringUp();

    auto client = makeClient(mod);

    // A real call first: that is what leaves the consumer already talking to
    // the module, which is the state the regression needs (on qt_remote it is
    // what drives the node's replica for this object to Valid).
    //
    // ASYNC deliberately. A synchronous call would deadlock on the plain
    // transport in this harness: PlainTransportHost::onCall dispatches to the
    // ModuleProxy's thread, which here is the calling thread, so the call would
    // sit until its timeout waiting for an event loop it is itself blocking.
    // That is a property of the fixture, not of the product — the plain suite's
    // own LiveHost puts the proxy on a worker thread for the same reason.
    std::atomic<int> called{0};
    client->invokeRemoteMethodAsync(mod.name(), QStringLiteral("echo"), QVariantList() << 5,
        LogosAPIClient::AsyncResultCallback([&](QVariant) { called.fetch_add(1); }));
    ASSERT_TRUE(pumpUntil([&] { return called.load() > 0; }, 15000))
        << "control: the preceding call never completed, so the consumer was never "
           "'already talking to the module' and the case below tests nothing";
    ASSERT_TRUE(pumpUntil([&] { return mod.canEmit(); }, 10000))
        << "control: the provider never came up, so nothing below means anything";

    std::atomic<int> got{0};
    const quint64 id = client->onEventWhenAvailable(mod.name(), QStringLiteral("ev"),
        [&](const QString&, const QVariantList&) { got.fetch_add(1); });
    ASSERT_NE(id, 0u);

    if (subscriptionIsLocallyRegistered(GetParam().transport)) {
        // Fire ONCE, right now, without returning to the event loop first.
        // Re-firing would hide exactly the gap under test.
        mod.emitEvent(QStringLiteral("ev"), 1);
        pump(500);
        EXPECT_GE(got.load(), 1)
            << "an event emitted immediately after subscribing to an ALREADY-REACHABLE "
               "module was dropped: the subscription had not armed yet. A consumer that "
               "calls a module and then subscribes is the common shape, and it used to "
               "arm synchronously.";
    } else {
        // plain: the subscribe frame has to reach the host first, so instant
        // delivery was never on offer. What must hold is that it arms shortly
        // and delivers -- i.e. the deferral did not break it.
        EXPECT_TRUE(fireUntilDelivered(mod, QStringLiteral("ev"), 1, got, 10000))
            << "subscribing right after a call never armed at all on this transport";
    }
}

// ── readiness, the call-path counterpart ─────────────────────────────────────
//
// whenObjectAvailable() answers "tell me when this module is reachable" without
// blocking and without giving up on the first no. It is what lets a CALL issued
// before its module exists be held and dispatched rather than failing fast, and
// it shares this registry — so it is pinned on every transport here rather than
// only end-to-end through the QML bridge.
TEST_P(EventDeliveryMatrix, WhenObjectAvailable_ModuleAlreadyUp_FiresTrue)
{
    Module mod = makeModule("rdyctl");
    ASSERT_TRUE(mod.hostReady());
    mod.bringUp();

    auto client = makeClient(mod);
    std::atomic<int> fired{0};
    std::atomic<int> ready{0};
    ASSERT_NE(client->whenObjectAvailable(mod.name(), [&](bool ok) {
        if (ok) ready.fetch_add(1);
        fired.fetch_add(1);
    }), 0u);

    ASSERT_TRUE(pumpUntil([&] { return fired.load() > 0; }, 10000))
        << "readiness never answered for a module that was already up";
    EXPECT_EQ(ready.load(), 1);
}

TEST_P(EventDeliveryMatrix, WhenObjectAvailable_ModuleAppearsLater_FiresExactlyOnce)
{
    Module mod = makeModule("rdylate");
    ASSERT_TRUE(mod.hostReady());
    // NOT brought up.

    auto client = makeClient(mod);
    std::atomic<int> fired{0};
    std::atomic<int> ready{0};
    ASSERT_NE(client->whenObjectAvailable(mod.name(), [&](bool ok) {
        if (ok) ready.fetch_add(1);
        fired.fetch_add(1);
    }), 0u);

    if (defersWhenModuleAbsent(GetParam().transport)) {
        pump(300);
        EXPECT_EQ(fired.load(), 0) << "answered 'not reachable' for a module that is "
                                      "merely not up YET -- that is the defect, not the answer";
    }

    mod.bringUp();
    ASSERT_TRUE(pumpUntil([&] { return fired.load() > 0; }, 10000))
        << "readiness never fired after the module appeared";
    EXPECT_EQ(ready.load(), 1);

    // Exactly once, and never again: a one-shot readiness answer that arrives
    // twice would dispatch a held call twice.
    pump(500);
    EXPECT_EQ(fired.load(), 1);
}

// It must NOT be resurrected by a reconnect. Event subscriptions are re-armed
// there on purpose; a readiness answer already delivered is spent, and
// re-firing it would re-dispatch whatever call it was gating.
TEST_P(EventDeliveryMatrix, WhenObjectAvailable_NotReArmedOnReconnect)
{
    Module mod = makeModule("rdyrecon");
    ASSERT_TRUE(mod.hostReady());
    mod.bringUp();

    auto client = makeClient(mod);
    std::atomic<int> fired{0};
    ASSERT_NE(client->whenObjectAvailable(mod.name(),
                                          [&](bool) { fired.fetch_add(1); }), 0u);
    ASSERT_TRUE(pumpUntil([&] { return fired.load() > 0; }, 10000)) << "control never fired";
    ASSERT_EQ(fired.load(), 1);

    ASSERT_TRUE(client->reconnect());
    pump(1000);
    EXPECT_EQ(fired.load(), 1) << "a spent readiness answer was re-delivered by reconnect";
}

INSTANTIATE_TEST_SUITE_P(
    AllTransportsAndProviders, EventDeliveryMatrix,
    ::testing::Values(
        MatrixCase{TransportKind::QtRemote, ProviderKind::Qt},
        MatrixCase{TransportKind::QtRemote, ProviderKind::Universal},
        MatrixCase{TransportKind::QtLocal,  ProviderKind::Qt},
        MatrixCase{TransportKind::QtLocal,  ProviderKind::Universal},
        MatrixCase{TransportKind::Plain,    ProviderKind::Qt},
        MatrixCase{TransportKind::Plain,    ProviderKind::Universal}),
    [](const ::testing::TestParamInfo<MatrixCase>& i) {
        return std::string(nameOf(i.param.transport)) + "_" + nameOf(i.param.provider);
    });

// ── mock: the one transport that cannot deliver ──────────────────────────────
//
// MockLogosObject::onEvent is a no-op by design, so no delivery test belongs in
// the matrix above. What mock DOES have to do is arm promptly and stop
// reporting itself pending — its requestObject() always succeeds, so a
// subscription that stayed deferred there would mean the registry had stopped
// making its one synchronous attempt, which is the regression that silently
// dropped every event emitted in the first 250 ms on qt_local and plain too.
TEST(EventDeliveryMock, SubscriptionArmsImmediatelyAndDoesNotLinger)
{
    ensureApp();
    LogosModeConfig::setMode(LogosMode::Mock);

    TokenManager::instance().saveToken(QStringLiteral("mock_ev_module"), QStringLiteral("tok"));
    auto client = std::make_unique<LogosAPIClient>(QStringLiteral("mock_ev_module"),
                                                   QStringLiteral("caller"),
                                                   &TokenManager::instance());
    const quint64 id = client->onEventWhenAvailable(
        QStringLiteral("mock_ev_module"), QStringLiteral("ev"),
        [](const QString&, const QVariantList&) {});

    EXPECT_NE(id, 0u);
    EXPECT_EQ(client->eventSubscriptionState(id), LogosSubscriptionState::Armed)
        << "mock's requestObject() always succeeds, so this must arm without waiting "
           "for a retry tick";
    EXPECT_TRUE(client->pendingEventSubscriptions().isEmpty());

    LogosModeConfig::setMode(LogosMode::Remote);
}

// ── the non-blocking guarantee ───────────────────────────────────────────────
//
// Structural, not budgeted: nothing on this path may block, because it is
// called from the GUI thread during startup. The way this fix decays is
// somebody "just retrying requestObject()" from the timer, which on qt_remote
// means a 250 ms socket probe plus waitForSource()'s nested event loop.
//
// WHAT THE acquireCount ASSERTION DOES AND DOES NOT PROVE. It catches a retry
// that polls qt_remote's blocking requestObject() in the ordinary case, which
// is the likely regression. It cannot catch the narrow one: the poll is only
// reachable at all when the transport DECLINES a deferred acquire while still
// reporting connected, which happens only if acquireDynamic() returns null —
// not forcible from outside the transport. That case is held shut by
// construction instead: beginAcquire() calls requestObject() only when the
// transport reports it has no deferred acquire, and RemoteTransportConnection
// implements that interface, so it can never give that answer. The invariant
// lives in the control flow, not in this test.
TEST(EventDeliveryNonBlocking, SubscribingToAnAbsentModuleReturnsImmediately)
{
    ensureApp();
    LogosModeConfig::setMode(LogosMode::Remote);

    TokenManager::instance().saveToken(QStringLiteral("never_module"), QStringLiteral("tok"));
    auto client = std::make_unique<LogosAPIClient>(QStringLiteral("never_module"),
                                                   QStringLiteral("caller"),
                                                   &TokenManager::instance());
    QElapsedTimer t; t.start();
    const quint64 id = client->onEventWhenAvailable(
        QStringLiteral("never_module"), QStringLiteral("ev"),
        [](const QString&, const QVariantList&) {});
    const qint64 subscribeMs = t.elapsed();

    EXPECT_NE(id, 0u);
    EXPECT_LT(subscribeMs, 250) << "onEventWhenAvailable blocked for " << subscribeMs << " ms";

    // And it stays non-blocking while pending: the retry must never reach
    // qt_remote's requestObject(). Measured by keeping the loop responsive.
    RemoteTransportConnection::resetAcquireCount();
    int beats = 0;
    QElapsedTimer beat; beat.start();
    qint64 worstGapMs = 0, last = 0;
    while (beat.elapsed() < 3000) {
        pump(20);
        ++beats;
        worstGapMs = qMax(worstGapMs, beat.elapsed() - last);
        last = beat.elapsed();
    }
    // Deliberately a floor, not a rate. This counts pump(20) iterations inside a
    // fixed wall-clock window, so on a loaded machine it measures the machine.
    // The assertion that means something is the GAP below.
    EXPECT_GT(beats, 5) << "the event loop did not run at all while a subscription was pending";
    EXPECT_LT(worstGapMs, 400) << "worst event-loop gap " << worstGapMs
                               << " ms -- something on the retry path is blocking";
    EXPECT_EQ(RemoteTransportConnection::acquireCount(), 0)
        << "the retry called qt_remote's BLOCKING requestObject(); on that transport "
           "the deferred acquire is the only legal path";
}
