// Deferred acquisition + deferred event subscription: the layer that makes a
// subscription survive a module that is not reachable YET.
//
// The defect these pin: requestObject() answers "is the module there RIGHT
// NOW", and every subscriber in this codebase asks at the one moment the
// answer is no — a module's init(), a UI backend's onContextReady(), a QML
// view's Component.onCompleted, all of which run while the dependency's host
// process has been spawned but has not called listen(). The old code returned
// nullptr/false there and never asked again: method calls kept working (they
// reach the replica by a path that never asks), events silently never arrived.
//
// Three properties are pinned here:
//   1. the acquire is NON-BLOCKING (it must not sit in waitForSource),
//   2. it arms when the module shows up afterwards,
//   3. lp_subscribe -- the C ABI every C++/Nim/Rust module and UI backend
//      reaches events through -- goes through the same path.
//
// Every case has a published-first control, because a red test with no control
// cannot distinguish "the defect" from "the fixture is mis-wired".

#include <gtest/gtest.h>

#include "logos_api_client.h"
#include "logos_instance.h"
#include "logos_mode.h"
#include "logos_object.h"
#include "logos_protocol.h"
#include "logos_provider_interface.h"
#include "module_proxy.h"
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

// The provider half, brought up on demand so a test controls WHEN the module
// starts listening.
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

} // anonymous namespace

class DeferredSubscriptionTest : public ::testing::Test {
protected:
    void SetUp() override { ensureApp(); LogosModeConfig::setMode(LogosMode::Remote); }
};

// ── transport layer ──────────────────────────────────────────────────────────

// The acquire itself must return promptly against a module that is NOT there.
// The pre-existing requestObject() sits in QRemoteObjectReplica::waitForSource()
// for the full timeout here; this path must not.
TEST_F(DeferredSubscriptionTest, RequestObjectWhenAvailable_AbsentModule_DoesNotBlock)
{
    const QString url = LogosInstance::id("async_absent_module");
    RemoteTransportConnection conn(url);
    ASSERT_TRUE(conn.connectToHost());

    std::atomic<int> delivered{0};
    QElapsedTimer t; t.start();
    const bool accepted = conn.requestObjectWhenAvailable(
        "async_absent_module", [&](LogosObject* obj) { if (obj) obj->release(); delivered.fetch_add(1); });
    const qint64 elapsed = t.elapsed();

    EXPECT_TRUE(accepted);
    EXPECT_LT(elapsed, 250) << "requestObjectWhenAvailable blocked for " << elapsed << " ms";
    // Contract: never delivered synchronously.
    EXPECT_EQ(delivered.load(), 0);
}

// It arms when the module appears afterwards.
TEST_F(DeferredSubscriptionTest, RequestObjectWhenAvailable_ArmsAfterPublish)
{
    const QString mod = QStringLiteral("async_late_module");
    RemoteTransportConnection conn(LogosInstance::id(mod));
    ASSERT_TRUE(conn.connectToHost());

    LogosObject* got = nullptr;
    ASSERT_TRUE(conn.requestObjectWhenAvailable(mod, [&](LogosObject* obj) { got = obj; }));

    pump(300);
    ASSERT_EQ(got, nullptr) << "delivered a handle for a module that was never published";

    Publisher pub(mod);
    for (int i = 0; i < 100 && !got; ++i) pump(50);

    ASSERT_NE(got, nullptr) << "handle never arrived after the module was published";
    got->release();
}

// Control for the two above: when the module is already up, the same call still
// defers delivery to the event loop (never synchronous) and still delivers.
TEST_F(DeferredSubscriptionTest, RequestObjectWhenAvailable_AlreadyPublished_Control)
{
    const QString mod = QStringLiteral("async_ready_module");
    Publisher pub(mod);

    RemoteTransportConnection conn(LogosInstance::id(mod));
    ASSERT_TRUE(conn.connectToHost());

    LogosObject* got = nullptr;
    ASSERT_TRUE(conn.requestObjectWhenAvailable(mod, [&](LogosObject* obj) { got = obj; }));
    EXPECT_EQ(got, nullptr) << "delivered synchronously -- re-entrancy hazard on the QtRO read stack";

    for (int i = 0; i < 100 && !got; ++i) pump(50);
    ASSERT_NE(got, nullptr);
    got->release();
}

// ── consumer layer ───────────────────────────────────────────────────────────

// The whole point: subscribe first, load the module second, still get events.
TEST_F(DeferredSubscriptionTest, OnEventWhenAvailable_SubscribeBeforePublish_Delivers)
{
    const QString mod = QStringLiteral("sub_late_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));
    auto client = std::make_unique<LogosAPIClient>(mod, QStringLiteral("caller"),
                                                   &TokenManager::instance());

    std::atomic<int> received{0};
    QVariantList last;
    client->onEventWhenAvailable(mod, QStringLiteral("ev0"),
        [&](const QString&, const QVariantList& d) { last = d; received.fetch_add(1); });

    // Registered, not armed -- and it SAYS so rather than vanishing.
    EXPECT_FALSE(client->pendingEventSubscriptions().isEmpty());

    pump(300);
    Publisher pub(mod);

    // Events are not buffered by QtRO, so re-fire while waiting for the arm.
    for (int i = 0; i < 200 && received.load() == 0; ++i) {
        if (pub.echo.emitFn) pub.echo.emitFn(QStringLiteral("ev0"), QVariantList{ 42 });
        pump(50);
    }

    ASSERT_GE(received.load(), 1) << "deferred subscription never armed";
    ASSERT_EQ(last.size(), 1);
    EXPECT_EQ(last[0].toInt(), 42);
    EXPECT_TRUE(client->pendingEventSubscriptions().isEmpty()) << "armed but still reported pending";
}

// Control: published first. Must be green regardless of the fix.
TEST_F(DeferredSubscriptionTest, OnEventWhenAvailable_PublishBeforeSubscribe_Control)
{
    const QString mod = QStringLiteral("sub_ready_module");
    Publisher pub(mod);

    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));
    auto client = std::make_unique<LogosAPIClient>(mod, QStringLiteral("caller"),
                                                   &TokenManager::instance());

    std::atomic<int> received{0};
    client->onEventWhenAvailable(mod, QStringLiteral("ev0"),
        [&](const QString&, const QVariantList&) { received.fetch_add(1); });

    for (int i = 0; i < 200 && received.load() == 0; ++i) {
        if (pub.echo.emitFn) pub.echo.emitFn(QStringLiteral("ev0"), QVariantList{ 7 });
        pump(50);
    }
    EXPECT_GE(received.load(), 1);
}

// ── C ABI (lp_subscribe) ─────────────────────────────────────────────────────
//
// This is the half that a LogosQmlBridge-only fix would have missed: a ui_qml
// package with a C++/Nim/Rust backend subscribes from INSIDE ui-host through
// the generated `dep.on<Event>()` wrapper -> logos::qt::subscribe ->
// lp_subscribe, which used to return nullptr in exactly this window.
TEST_F(DeferredSubscriptionTest, LpSubscribe_BeforePublish_Delivers)
{
    const QString mod = QStringLiteral("lp_late_module");

    lp_client* client = lp_client_create(mod.toUtf8().constData(), "caller", nullptr, nullptr);
    ASSERT_NE(client, nullptr);

    std::atomic<int> received{0};
    lp_subscription* sub = lp_subscribe(
        client, "ev0",
        [](const char*, const char*, void* ud) {
            static_cast<std::atomic<int>*>(ud)->fetch_add(1);
        },
        &received);

    ASSERT_NE(sub, nullptr) << "lp_subscribe refused a subscription for a module that is "
                               "not reachable yet -- it will never be retried";

    pump(300);
    Publisher pub(mod);

    for (int i = 0; i < 200 && received.load() == 0; ++i) {
        if (pub.echo.emitFn) pub.echo.emitFn(QStringLiteral("ev0"), QVariantList{ 1 });
        pump(50);
    }
    EXPECT_GE(received.load(), 1) << "lp_subscribe subscription never armed";

    lp_unsubscribe(sub);
    lp_client_destroy(client);
}

// Control for the case above.
TEST_F(DeferredSubscriptionTest, LpSubscribe_AfterPublish_Control)
{
    const QString mod = QStringLiteral("lp_ready_module");
    Publisher pub(mod);

    lp_client* client = lp_client_create(mod.toUtf8().constData(), "caller", nullptr, nullptr);
    ASSERT_NE(client, nullptr);

    std::atomic<int> received{0};
    lp_subscription* sub = lp_subscribe(
        client, "ev0",
        [](const char*, const char*, void* ud) {
            static_cast<std::atomic<int>*>(ud)->fetch_add(1);
        },
        &received);
    ASSERT_NE(sub, nullptr);

    for (int i = 0; i < 200 && received.load() == 0; ++i) {
        if (pub.echo.emitFn) pub.echo.emitFn(QStringLiteral("ev0"), QVariantList{ 1 });
        pump(50);
    }
    EXPECT_GE(received.load(), 1);

    lp_unsubscribe(sub);
    lp_client_destroy(client);
}

// ── The probe must not free a replica QtRO is still holding ──────────────────
//
// Regression for the use-after-free introduced by e9f82ac and fixed by 09f684f.
//
// tryAcquireNow() used to acquire a dynamic replica and, when it was not
// already Valid, DELETE it. QtRO shares one replica IMPLEMENTATION per object
// name per node, and while that implementation is still waiting for the
// source's metaobject it records every facade built on it as a RAW pointer in
// QConnectedReplicaImplementation::m_parentsNeedingConnect.
// ~QRemoteObjectReplica is an empty body, so destroying a facade never
// deregisters it -- and the implementation dereferences the whole list when the
// class definition arrives.
//
// WHY IT TAKES MORE THAN ONE SUBSCRIPTION. The first probe owns the only
// implementation and takes it down with itself, so a single subscription is
// harmless. It needs a second whose implementation is pinned by an in-flight
// PendingAcquire before a freed facade can outlive the implementation holding a
// pointer to it. A consumer that subscribes once sees nothing wrong; a view
// that registers every event it cares about up front dies. That asymmetry is
// why this went unnoticed, and it is why the control below matters as much as
// the case.
//
// WHERE IT CRASHES. Not at the subscribe call -- in the event loop, one turn
// later, when the handshake lands and QtRO walks the list. So the failure mode
// of this test is the BINARY DYING (SIGBUS / BUS_ADRALN on arm64, SIGSEGV
// elsewhere), not a failed assertion. A green run is the whole signal.
//
// The pumping at the end is therefore not incidental: without an event-loop
// turn after the subscriptions, the pre-fix code passes this test.
TEST_F(DeferredSubscriptionTest, ManySubscriptionsBeforeArm_DoNotFreeAReplicaQtRoStillHolds)
{
    const QString mod = QStringLiteral("probe_churn_module");

    // Published FIRST and never pumped before subscribing: the host exists, so
    // an acquire can pin an implementation, but the replica cannot have reached
    // Valid yet -- which is exactly the window every probe lands in.
    Publisher pub(mod);

    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));
    auto client = std::make_unique<LogosAPIClient>(mod, QStringLiteral("caller"),
                                                   &TokenManager::instance());

    // 24 is comfortably past the ~14 at which the original crash reproduced,
    // and far enough past 1 that the "one probe is harmless" case cannot mask it.
    constexpr int kSubscriptions = 24;
    std::atomic<int> received{0};
    for (int i = 0; i < kSubscriptions; ++i) {
        const quint64 id = client->onEventWhenAvailable(
            mod, QStringLiteral("ev%1").arg(i),
            [&](const QString&, const QVariantList&) { received.fetch_add(1); });
        ASSERT_NE(id, 0u) << "subscription " << i << " was refused";
    }

    // The turn that used to kill the process.
    for (int i = 0; i < 200 && received.load() == 0; ++i) {
        if (pub.echo.emitFn) pub.echo.emitFn(QStringLiteral("ev0"), QVariantList{ 1 });
        pump(50);
    }

    // Surviving to here is the regression assertion. Delivery is asserted too,
    // so that a build which somehow armed nothing at all cannot pass by being
    // quietly inert.
    EXPECT_GE(received.load(), 1) << "no subscription ever armed";
    EXPECT_TRUE(client->pendingEventSubscriptions().isEmpty())
        << "still reporting pending after arming";
}

// Control: ONE subscription in the same window. Green both before and after the
// fix -- it is here to pin that the case above needs the churn, not merely a
// deferred subscribe, so a future reader cannot conclude the whole path was
// broken.
TEST_F(DeferredSubscriptionTest, OneSubscriptionBeforeArm_Control)
{
    const QString mod = QStringLiteral("probe_single_module");
    Publisher pub(mod);

    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));
    auto client = std::make_unique<LogosAPIClient>(mod, QStringLiteral("caller"),
                                                   &TokenManager::instance());

    std::atomic<int> received{0};
    ASSERT_NE(client->onEventWhenAvailable(mod, QStringLiteral("ev0"),
        [&](const QString&, const QVariantList&) { received.fetch_add(1); }), 0u);

    for (int i = 0; i < 200 && received.load() == 0; ++i) {
        if (pub.echo.emitFn) pub.echo.emitFn(QStringLiteral("ev0"), QVariantList{ 1 });
        pump(50);
    }
    EXPECT_GE(received.load(), 1);
}
