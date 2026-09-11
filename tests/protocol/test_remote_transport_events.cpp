// Event delivery over the qt_remote (LogosProtocol::LocalSocket) transport.
//
// LocalSocket is the DEFAULT transport — the one every UI plugin's LogosAPI
// uses (a universal `type: ui_qml` backend running in the out-of-process
// ui-host subscribes to a dependency module's typed events via
// modules().<dep>.on<Event>(...), which lands on RemoteLogosObject::onEvent).
// Method calls over this transport are exercised throughout, but typed event
// delivery was only ever covered over the plain/TCP transport
// (test_plain_transport_tcp.cpp, currently #if 0) and the mock transport —
// never over qt_remote. This test closes that gap and reproduces the reported
// bug "UI plugins cannot subscribe to module events".
//
// Unlike the plain transport (whose in-process fixture deadlocks because
// callMethod blocks on a std::future while the provider's queued dispatch
// needs the same event loop), the pure-event qt_remote path has no blocking
// future: requestObject()'s waitForSource() pumps its own nested event loop,
// and onEvent() is a local signal-connect onto the replica. So we can drive
// host + consumer on one event loop in-process.

#include <gtest/gtest.h>

#include "logos_object.h"
#include "logos_async_dispatch.h"
#include "logos_api_consumer.h"
#include "logos_instance.h"
#include "logos_mode.h"
#include "logos_provider_interface.h"
#include "module_proxy.h"
#include "remote_transport.h"

#include <QVector>

#include <QCoreApplication>
#include <QJsonArray>
#include <QString>
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

// A minimal provider that emits a typed `ticked` event from bump(), exactly
// like a universal module's generated event glue: the impl calls its event
// method, which routes the payload through the provider's event listener. The
// listener is wired by ModuleProxy's ctor to emit eventResponse — so this
// exercises the *full* provider→ModuleProxy→transport emit chain, not just a
// direct eventResponse() emission.
class TickProvider : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList& /*args*/) override {
        if (method == QLatin1String("bump")) {
            ++m_count;
            if (m_eventCb) m_eventCb(QStringLiteral("ticked"), QVariantList{ m_count });
            return m_count;
        }
        return QVariant();
    }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    QJsonArray getMethods() override { return QJsonArray{}; }
    void setEventListener(EventCallback cb) override { m_eventCb = std::move(cb); }
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("tick_module"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }
private:
    EventCallback m_eventCb;
    int m_count = 0;
};

} // anonymous namespace

class RemoteEventTest : public ::testing::Test {
protected:
    void SetUp() override { ensureApp(); }

    void pumpEventLoop(int ms) {
        auto end = std::chrono::steady_clock::now()
                 + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};

// Publish a ModuleProxy (the same QObject the real provider publishes) over a
// LocalSocket host, acquire it through a LocalSocket connection, subscribe to
// one of its events, then emit that event from the source side. The callback
// must fire with the typed payload — exactly what a UI backend expects from
// modules().<dep>.on<Event>(...).
TEST_F(RemoteEventTest, EventDeliveredToSubscriberOverLocalSocket)
{
    // Provider and consumer derive matching registry URLs from the shared
    // LOGOS_INSTANCE_ID, just like LogosAPIProvider / LogosAPIConsumer do.
    const QString registryUrl = LogosInstance::id("emitter");

    RemoteTransportHost host(registryUrl);
    ModuleProxy fake(nullptr);
    ASSERT_TRUE(host.publishObject("emitter", &fake));

    RemoteTransportConnection conn(registryUrl);
    ASSERT_TRUE(conn.connectToHost());

    LogosObject* obj = conn.requestObject("emitter", 5000);
    ASSERT_NE(obj, nullptr);

    std::atomic<int> received{0};
    QString lastEvent;
    QVariantList lastData;
    obj->onEvent("ticked", [&](const QString& name, const QVariantList& data) {
        lastEvent = name;
        lastData = data;
        received.fetch_add(1);
    });

    // Let the subscription settle (replica signal connection is local, but
    // give QtRO a beat regardless).
    pumpEventLoop(200);

    // Emit from the source side — fires ModuleProxy::eventResponse, which QtRO
    // forwards to the replica the consumer holds.
    emit fake.eventResponse("ticked", QVariantList{ 42 });

    for (int i = 0; i < 60 && received.load() == 0; ++i) pumpEventLoop(50);

    EXPECT_GE(received.load(), 1);
    EXPECT_EQ(lastEvent, "ticked");
    ASSERT_EQ(lastData.size(), 1);
    EXPECT_EQ(lastData[0].toInt(), 42);

    obj->release();
}

// Same as above, but the event is emitted through the *real* provider chain
// (TickProvider::callMethod → provider event listener → ModuleProxy →
// eventResponse), the way a universal module actually emits. Proves the
// full server-side emit path lands on a qt_remote subscriber's callback.
TEST_F(RemoteEventTest, ProviderEmittedEventReachesSubscriberOverLocalSocket)
{
    const QString registryUrl = LogosInstance::id("tick_module");

    RemoteTransportHost host(registryUrl);
    TickProvider provider;
    ModuleProxy proxy(&provider);   // ctor wires provider listener → eventResponse
    ASSERT_TRUE(host.publishObject("tick_module", &proxy));

    RemoteTransportConnection conn(registryUrl);
    ASSERT_TRUE(conn.connectToHost());

    LogosObject* obj = conn.requestObject("tick_module", 5000);
    ASSERT_NE(obj, nullptr);

    std::atomic<int> received{0};
    QVariantList lastData;
    obj->onEvent("ticked", [&](const QString&, const QVariantList& data) {
        lastData = data;
        received.fetch_add(1);
    });
    pumpEventLoop(200);

    // Trigger the module method on the source side — emits "ticked" internally.
    provider.callMethod("bump", QVariantList{});

    for (int i = 0; i < 60 && received.load() == 0; ++i) pumpEventLoop(50);

    EXPECT_GE(received.load(), 1);
    ASSERT_EQ(lastData.size(), 1);
    EXPECT_EQ(lastData[0].toInt(), 1);

    obj->release();
}

// A provider that defers every business call: callMethod returns the pending
// sentinel (logos::pendingCallKey -> callId) instead of a value, exactly like a
// `concurrency:"multi"` module that hands the work to a worker and later pushes
// the real result as a logos::callCompleteEvent. This is the path the EVM
// wallet backend hits when it fans balance reads out to eth_rpc via call_async.
namespace {
class DeferredProvider : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList& /*args*/) override {
        if (method == QLatin1String("asyncWork")) {
            QVariantMap sentinel;
            sentinel[logos::pendingCallKey()] = m_callId;   // "defer me"
            return sentinel;
        }
        return QVariant();
    }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    QJsonArray getMethods() override { return QJsonArray{}; }
    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("async_module"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }
    QString m_callId = QStringLiteral("call-1");
};
} // anonymous namespace

// The gate one layer below LogosAPIConsumer: a raw LogosObject handle, the
// interface every consumer path bottoms out in.
//
// The control is the whole point. Refusing the subscription is easy; refusing it
// WITHOUT breaking the transport's own completion callback — registered on the
// same helper, for the same name, from RemoteLogosObject's constructor — is the
// part that can regress. So this asserts the outside subscriber gets nothing and
// the deferred call still returns its result.
TEST_F(RemoteEventTest, ReservedEventCannotBeSubscribedButStillCompletesTheCall)
{
    const QString registryUrl = LogosInstance::id("async_module");

    RemoteTransportHost host(registryUrl);
    DeferredProvider provider;
    ModuleProxy proxy(&provider);
    ASSERT_TRUE(proxy.saveToken(QStringLiteral("test_caller"), QStringLiteral("tok-1")));
    ASSERT_TRUE(host.publishObject("async_module", &proxy));

    RemoteTransportConnection conn(registryUrl);
    ASSERT_TRUE(conn.connectToHost());

    LogosObject* obj = conn.requestObject("async_module", 5000);
    ASSERT_NE(obj, nullptr);

    std::atomic<int> leaked{0};
    obj->onEvent(logos::callCompleteEvent(),
                 [&](const QString&, const QVariantList&) { leaked.fetch_add(1); });

    std::atomic<int> delivered{0};
    QVariant got;
    obj->callMethodAsync(QStringLiteral("tok-1"), QStringLiteral("asyncWork"),
                         QVariantList{}, 5000,
        [&](QVariant result) { got = result; delivered.fetch_add(1); });

    pumpEventLoop(300);
    emit proxy.eventResponse(logos::callCompleteEvent(),
                             QVariantList{ provider.m_callId, QVariant(123) });
    for (int i = 0; i < 60 && delivered.load() == 0; ++i) pumpEventLoop(50);

    EXPECT_EQ(leaked.load(), 0) << "onEvent handed out the completion channel";
    EXPECT_EQ(delivered.load(), 1) << "the refusal took the transport's own completion with it";
    EXPECT_EQ(got.toInt(), 123);

    obj->release();
    pumpEventLoop(200);
}

// Regression test for the refresh_balances SIGSEGV.
//
// The deferred-completion result is delivered by RemoteEventHelper::onEventResponse
// (a slot fired by the replica's eventResponse signal). The consumer's async
// callback — mirroring LogosAPIConsumer::invokeRemoteMethodAsync, which runs the
// user callback and *then* calls plugin->release() — releases the object from
// INSIDE that slot dispatch. release() used to `delete m_helper` (the very QObject
// whose slot is on the stack) and `delete m_replica` (the signal's sender) inline,
// corrupting the connection list QtRO was still iterating → use-after-free crash
// in QMetaObjectPrivate / the QtRO read path. The fix defers both deletions with
// deleteLater(); this test drives the exact reentrancy and must complete without
// crashing, with the result still delivered.
TEST_F(RemoteEventTest, ReleaseFromAsyncCompletionCallbackDoesNotCrash)
{
    const QString registryUrl = LogosInstance::id("async_module");

    RemoteTransportHost host(registryUrl);
    DeferredProvider provider;
    ModuleProxy proxy(&provider);
    // Authorize the caller so callRemoteMethod dispatches to the provider
    // (business methods are token-gated; saveToken is the per-proxy issue store).
    ASSERT_TRUE(proxy.saveToken(QStringLiteral("test_caller"), QStringLiteral("tok-1")));
    ASSERT_TRUE(host.publishObject("async_module", &proxy));

    RemoteTransportConnection conn(registryUrl);
    ASSERT_TRUE(conn.connectToHost());

    LogosObject* obj = conn.requestObject("async_module", 5000);
    ASSERT_NE(obj, nullptr);

    std::atomic<int> delivered{0};
    QVariant got;
    // Fire the deferred call. The result will arrive later as a completion event;
    // the callback releases `obj` from within that completion dispatch.
    obj->callMethodAsync(QStringLiteral("tok-1"), QStringLiteral("asyncWork"),
                         QVariantList{}, 5000,
        [&](QVariant result) {
            got = result;
            delivered.fetch_add(1);
            obj->release();   // reentrant release during completion-event dispatch
        });

    // Let the call round-trip and register the async completion callback.
    pumpEventLoop(300);

    // Push the deferred result as a completion event keyed by the same callId,
    // from the source side — QtRO forwards it to the consumer's replica.
    emit proxy.eventResponse(logos::callCompleteEvent(),
                             QVariantList{ provider.m_callId, QVariant(123) });

    for (int i = 0; i < 60 && delivered.load() == 0; ++i) pumpEventLoop(50);

    // If release() corrupted the dispatch this process would have crashed above.
    EXPECT_GE(delivered.load(), 1);
    EXPECT_EQ(got.toInt(), 123);

    // Give the deferred deleteLater() of the helper/replica a beat to run, so the
    // test also exercises the deferred-teardown path cleanly.
    pumpEventLoop(200);
    // NOTE: obj was already released inside the callback — do not touch it here.
}

// An echo provider: every call returns its first argument, so a caller can
// verify results across many calls. Business methods are token-gated by the
// ModuleProxy in front of it.
namespace {
class EchoProvider : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList& args) override {
        if (method == QLatin1String("echo") && !args.isEmpty())
            return args.first();
        return QVariant();
    }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    QJsonArray getMethods() override { return QJsonArray{}; }
    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("echo_module"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }
};
} // namespace

// The consumer must acquire a remote-object handle ONCE and reuse it across
// calls — both sync and async — instead of re-acquiring (acquireDynamic +
// waitForSource) per call. Re-acquiring per call made a proxy's rapid forwarding
// loop ~10x slower and starved the nested synchronous calls (the UI cross-version
// hang). This drives 2*N calls through a LogosAPIConsumer and asserts exactly
// one acquisition, with every result correct on both paths.
TEST_F(RemoteEventTest, ConsumerReusesCachedHandleAcrossSyncAndAsyncCalls)
{
    const QString registryUrl = LogosInstance::id("echo_module");

    RemoteTransportHost host(registryUrl);
    EchoProvider provider;
    ModuleProxy proxy(&provider);
    ASSERT_TRUE(proxy.saveToken(QStringLiteral("caller"), QStringLiteral("tok")));
    ASSERT_TRUE(host.publishObject("echo_module", &proxy));

    // Remote mode → the qt_remote local-socket transport (RemoteTransportConnection,
    // the same path a UI plugin uses), matching the RemoteTransportHost above.
    LogosModeConfig::setMode(LogosMode::Remote);
    LogosAPIConsumer consumer(QStringLiteral("echo_module"), QStringLiteral("caller"),
                              /*token_manager=*/nullptr);

    RemoteTransportConnection::resetAcquireCount();

    constexpr int N = 12;

    // Sync calls.
    for (int i = 0; i < N; ++i) {
        const QVariant r = consumer.invokeRemoteMethod(
            QStringLiteral("tok"), QStringLiteral("echo_module"),
            QStringLiteral("echo"), QVariantList{ i });
        EXPECT_EQ(r.toInt(), i) << "sync echo " << i;
    }

    // Async calls — fired without waiting between them, then drained.
    std::atomic<int> done{0};
    QVector<int> results(N, -1);
    for (int i = 0; i < N; ++i) {
        consumer.invokeRemoteMethodAsync(
            QStringLiteral("tok"), QStringLiteral("echo_module"),
            QStringLiteral("echo"), QVariantList{ i },
            [&results, &done, i](QVariant r) { results[i] = r.toInt(); done.fetch_add(1); });
    }
    for (int k = 0; k < 1000 && done.load() < N; ++k) pumpEventLoop(5);
    ASSERT_EQ(done.load(), N);
    for (int i = 0; i < N; ++i) EXPECT_EQ(results[i], i) << "async echo " << i;

    // The whole point: 2*N calls, but the handle was acquired exactly once.
    EXPECT_EQ(RemoteTransportConnection::acquireCount(), 1);
}
