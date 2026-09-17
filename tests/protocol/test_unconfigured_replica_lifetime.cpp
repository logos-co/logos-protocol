// Freeing a dynamic replica that never reached Valid is a use-after-free.
//
// QtRO shares ONE QConnectedReplicaImplementation per object name per node.
// While that implementation is still waiting for the source's class definition
// it records every facade built on it as a RAW pointer in
// m_parentsNeedingConnect (qremoteobjectreplica.cpp:589), and
// ~QRemoteObjectReplica is an empty body that never deregisters. The list is
// walked — and every entry dereferenced — the moment the definition arrives, in
// QConnectedReplicaImplementation::setDynamicProperties().
//
// tryAcquireNow() already parks its probes for this reason. requestObject() did
// not: its timeout branch deleted the facade it had just waited on, which is
// exactly a facade that never reached Valid. It now parks that facade and hands
// it to the NEXT wait on the same name -- parking alone would be correct but
// unbounded, since requestObject() is retried against a module that is down. That killed the json_rpc_bridge
// module host on 2026-09-17 — storage_module's host crashed during stop(), the
// bridge's blocking call timed out ("Timeout waiting for replica:
// storage_module") and freed its facade, the module was loaded again a second
// later, and the source's dynamic API landed on the freed object:
//
//   QMetaObjectPrivate::connect
//   QMetaObject::connect
//   QRemoteObjectReplicaImplementation::configurePrivate
//   QConnectedReplicaImplementation::configurePrivate
//   QConnectedReplicaImplementation::setDynamicProperties
//   QRemoteObjectNodePrivate::onClientRead                 SIGSEGV at 0x58
//
// It needs a SECOND facade on the same name to show up at all: if the timed-out
// facade holds the only reference to the implementation, it takes the
// implementation down with itself and the dangling entry dies unread. That is
// the production shape — LogosAPIConsumer::beginAcquire() parks a probe via
// tryAcquireNow() and starts a PendingAcquire before any blocking call runs —
// and it is why the bug survived single-subscriber use for so long.
//
// DETECTOR. Validated on the pre-fix tree (5451819, `delete replica` in
// RemoteTransportConnection::requestObject): the test binary dies with SIGSEGV
// inside QtRO, on the pumpEventLoop() that follows the late publish.

#include <gtest/gtest.h>

#include "logos_instance.h"
#include "logos_object.h"
#include "module_proxy.h"
#include "remote_transport.h"

#include <QCoreApplication>
#include <QString>

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

} // namespace

class UnconfiguredReplicaTest : public ::testing::Test {
protected:
    void SetUp() override { ensureApp(); }

    void pumpEventLoop(int ms) {
        const auto end = std::chrono::steady_clock::now()
                       + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};

TEST_F(UnconfiguredReplicaTest, ATimedOutRequestObjectSurvivesTheModuleComingBack)
{
    const QString registryUrl = LogosInstance::id("latecomer");

    // The endpoint LISTENS while the object itself is absent — the shape a
    // module host that is restarting leaves behind. Publishing something else
    // is what brings the registry up without bringing "latecomer" up.
    RemoteTransportHost host(registryUrl);
    ModuleProxy placeholder(nullptr);
    ASSERT_TRUE(host.publishObject("placeholder", &placeholder));

    RemoteTransportConnection conn(registryUrl);
    ASSERT_TRUE(conn.connectToHost());

    // Facade 1: a parked probe, pinning the shared implementation.
    EXPECT_EQ(nullptr, conn.tryAcquireNow("latecomer"));

    // Facade 2: the blocking acquire. It cannot succeed, so it times out —
    // and before the fix it was freed right here, still in the implementation's
    // raw list.
    EXPECT_EQ(nullptr, conn.requestObject("latecomer", 300));

    // The module comes back and publishes its source. QtRO now walks every
    // facade it recorded.
    ModuleProxy late(nullptr);
    ASSERT_TRUE(host.publishObject("latecomer", &late));
    pumpEventLoop(1000);   // pre-fix: SIGSEGV inside QRemoteObjectNodePrivate::onClientRead

    // Alive, and the connection still works: the parked probe went Valid, so
    // the next acquire hands over a usable object.
    LogosObject* obj = conn.tryAcquireNow("latecomer");
    ASSERT_NE(nullptr, obj);
    obj->release();
}

// The same wait against a module that never appears: nothing to configure, so
// nothing to dereference. Guards the fix's other half — a parked facade whose
// source never arrives must still be reaped by the connection's teardown rather
// than outliving the node it belongs to.
TEST_F(UnconfiguredReplicaTest, ATimedOutRequestObjectIsReapedWithTheConnection)
{
    const QString registryUrl = LogosInstance::id("never");

    RemoteTransportHost host(registryUrl);
    ModuleProxy placeholder(nullptr);
    ASSERT_TRUE(host.publishObject("placeholder", &placeholder));

    {
        RemoteTransportConnection conn(registryUrl);
        ASSERT_TRUE(conn.connectToHost());
        EXPECT_EQ(nullptr, conn.tryAcquireNow("never"));
        EXPECT_EQ(nullptr, conn.requestObject("never", 200));
        EXPECT_EQ(nullptr, conn.requestObject("never", 200));
    }   // ~RemoteTransportConnection: parked facades die BEFORE the node

    pumpEventLoop(200);
    SUCCEED();
}

// Parking alone is not enough. requestObject() acquires a facade per call, so a
// consumer retrying against a module that stays down would park one per attempt
// -- the bridge revalidates every exposed module on a timer, so an outage would
// accumulate hundreds before the recovery burst configured them all. The next
// wait must REUSE the parked facade instead of adding another.
TEST_F(UnconfiguredReplicaTest, RetryingAgainstADownModuleParksExactlyOneFacade)
{
    const QString registryUrl = LogosInstance::id("down");

    RemoteTransportHost host(registryUrl);
    ModuleProxy placeholder(nullptr);
    ASSERT_TRUE(host.publishObject("placeholder", &placeholder));

    RemoteTransportConnection conn(registryUrl);
    ASSERT_TRUE(conn.connectToHost());
    EXPECT_EQ(nullptr, conn.tryAcquireNow("down"));

    for (int i = 0; i < 8; ++i)
        EXPECT_EQ(nullptr, conn.requestObject("down", 100)) << "attempt " << i;

    EXPECT_EQ(1, conn.parkedCount("down"));

    // And the one parked facade is still the live path once the module lands:
    // it is handed to this wait rather than left behind.
    ModuleProxy late(nullptr);
    ASSERT_TRUE(host.publishObject("down", &late));
    LogosObject* obj = conn.requestObject("down", 5000);
    ASSERT_NE(nullptr, obj);
    EXPECT_EQ(0, conn.parkedCount("down"));
    obj->release();
}
