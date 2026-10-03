// A deferred ("multi") call that is still waiting for its completion when its
// object is released must still be answered exactly once, as the plain
// transport answers it: abandoned. On QtRO its callback was dropped, and its
// timeout timer kept a context that outlives the object (released objects are
// deleted at once, their event helper later, and never without a running
// event loop), so in a test process the timer fired into the freed object.
#include <gtest/gtest.h>

#include "logos_async_dispatch.h"
#include "logos_instance.h"
#include "logos_object.h"
#include "logos_provider_interface.h"
#include "module_proxy.h"
#include "remote_transport.h"
#include "token_manager.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QVariantMap>

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

// Defers every call and never completes it.
class NeverCompletesProvider : public LogosProviderObject {
public:
    QVariant callMethod(const QString&, const QVariantList&) override {
        QVariantMap pending;
        pending[logos::pendingCallKey()] = QStringLiteral("lc-never");
        return pending;
    }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    QJsonArray getMethods() override { return QJsonArray{}; }
    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("qtro_never_completes"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }
};

void pump(int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

} // namespace

TEST(QtRemoteReleaseDeferred, AReleasedDeferredCallIsAnsweredOnceAsAbandoned)
{
    ensureApp();
    const QString registryUrl = LogosInstance::id("qtro_never_completes");

    RemoteTransportHost host(registryUrl);
    NeverCompletesProvider provider;
    ModuleProxy proxy(&provider);
    ASSERT_TRUE(proxy.saveToken(QStringLiteral("origin"), QStringLiteral("tok-1")));
    ASSERT_TRUE(host.publishObject("qtro_never_completes", &proxy));

    // The object itself, not a LogosAPIConsumer: a consumer drops callbacks
    // once it is gone, which would hide what the transport does.
    RemoteTransportConnection conn(registryUrl);
    ASSERT_TRUE(conn.connectToHost());
    LogosObject* obj = conn.requestObject(QStringLiteral("qtro_never_completes"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = dynamic_cast<LogosObjectErrorChannel*>(obj);
    ASSERT_NE(ch, nullptr);

    auto count = std::make_shared<std::atomic<int>>(0);
    auto code = std::make_shared<std::string>();
    ch->callMethodAsyncWithError(
        QStringLiteral("tok-1"), QStringLiteral("defer"), QVariantList{}, 600,
        [count, code](QVariant, const logos::CallError& e) {
            *code = e.code;
            count->fetch_add(1);
        });
    pump(150);  // the sentinel is back: the call now waits for its completion
    ASSERT_EQ(count->load(), 0);

    obj->release();
    pump(100);  // well before the 600 ms timeout
    EXPECT_EQ(count->load(), 1) << "the released call was never answered";
    EXPECT_EQ(*code, "transport_error");

    pump(800);  // past the timeout: nothing more, and nothing touches the freed object
    EXPECT_EQ(count->load(), 1);
}
