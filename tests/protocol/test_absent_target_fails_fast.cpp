// A blocking acquire for a module nothing listens for fails within the grace, not the caller's budget.
// The host publishes before it reports a module loaded, so a missing listener means it is not running.

#include <gtest/gtest.h>

#include "logos_api_client.h"
#include "logos_call_error.h"
#include "logos_instance.h"
#include "logos_object.h"
#include "module_proxy.h"
#include "remote_transport.h"
#include "token_manager.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

namespace {

QCoreApplication* ensureApp() {
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance())
        new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

constexpr int kGraceMs = RemoteTransportConnection::kNoListenerGraceMs;
// Slack for the probe and a loaded runner; far below the 20 s the bug cost.
constexpr int kSlackMs = 1500;

} // namespace

class AbsentTargetTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensureApp();
        TokenManager::instance().clearAllTokens();
    }
    void TearDown() override { TokenManager::instance().clearAllTokens(); }
};

TEST_F(AbsentTargetTest, ARequestForAModuleNothingListensForFailsWithinTheGrace)
{
    RemoteTransportConnection conn(LogosInstance::id("absent_target"));
    ASSERT_TRUE(conn.connectToHost());

    QElapsedTimer t;
    t.start();
    EXPECT_EQ(nullptr, conn.requestObject("absent_target", 20000));
    EXPECT_LT(t.elapsed(), kGraceMs + kSlackMs)
        << "a module that is not running cost the whole 20 s budget";
}

// The path a module's typed call takes (logos-tutorial's sumVia bound to no_such_module).
TEST_F(AbsentTargetTest, ACallToAModuleThatIsNotLoadedReportsObjectUnavailablePromptly)
{
    LogosAPIClient client(QStringLiteral("absent_target"), QStringLiteral("test_origin"),
                          &TokenManager::instance());
    logos::CallError err;
    QElapsedTimer t;
    t.start();
    const QVariant r = client.invokeRemoteMethod(QStringLiteral("absent_target"),
                                                 QStringLiteral("ping"), QVariantList{},
                                                 Timeout(20000), &err);
    EXPECT_FALSE(r.isValid());
    EXPECT_EQ(err.code, "object_unavailable");
    EXPECT_LT(t.elapsed(), kGraceMs + kSlackMs);
}

TEST_F(AbsentTargetTest, AModuleThatStartsListeningInsideTheGraceIsStillReached)
{
    const QString url = LogosInstance::id("starting_target");
    RemoteTransportConnection conn(url);
    ASSERT_TRUE(conn.connectToHost());

    RemoteTransportHost host(url);
    ModuleProxy proxy(nullptr);
    QObject ctx;   // declared last, so a timer that has not fired dies before what it touches
    QTimer::singleShot(kGraceMs / 3, &ctx, [&] { host.publishObject("starting_target", &proxy); });

    LogosObject* obj = conn.requestObject("starting_target", 5000);
    ASSERT_NE(nullptr, obj);
    obj->release();
}

// A host binds before its module's init runs and publishes the module after it, so listening keeps the full budget.
TEST_F(AbsentTargetTest, AModuleThatBindsInsideTheGraceButPublishesAfterItIsReached)
{
    const QString url = LogosInstance::id("slow_init_target");
    RemoteTransportConnection conn(url);
    ASSERT_TRUE(conn.connectToHost());

    RemoteTransportHost host(url);
    ModuleProxy handshake(nullptr);
    ModuleProxy business(nullptr);
    QObject ctx;
    QTimer::singleShot(kGraceMs / 3, &ctx,
                       [&] { host.publishObject(logos::handshakeObjectName("slow_init_target"), &handshake); });
    QTimer::singleShot(kGraceMs + 800, &ctx, [&] { host.publishObject("slow_init_target", &business); });

    LogosObject* obj = conn.requestObject("slow_init_target", 6000);
    ASSERT_NE(nullptr, obj);
    obj->release();
}

TEST_F(AbsentTargetTest, AListeningModuleKeepsTheFullBudgetPastTheGrace)
{
    const QString url = LogosInstance::id("warming_target");
    RemoteTransportHost host(url);
    ModuleProxy placeholder(nullptr);
    ASSERT_TRUE(host.publishObject("placeholder", &placeholder));

    RemoteTransportConnection conn(url);
    ASSERT_TRUE(conn.connectToHost());

    ModuleProxy late(nullptr);
    QObject ctx;
    QTimer::singleShot(kGraceMs + 800, &ctx, [&] { host.publishObject("warming_target", &late); });

    LogosObject* obj = conn.requestObject("warming_target", 6000);
    ASSERT_NE(nullptr, obj);
    obj->release();
}
