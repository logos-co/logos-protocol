// Every replica of a module receives every completion event, and only the
// replica that made the call claims it. The others must not keep it: when call
// ids repeat (a reloaded "multi" module counts from 0 again), a kept result is
// handed to that replica's next call. Measured under Basecamp: after
// package_downloader was reloaded, refreshCatalog received an old array result.
#include <gtest/gtest.h>

#include "logos_api_consumer.h"
#include "logos_async_dispatch.h"
#include "logos_instance.h"
#include "logos_provider_interface.h"
#include "module_proxy.h"
#include "remote_transport.h"
#include "token_manager.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

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

// Defers every call under the same id and completes it with the call's
// argument, as a module that reuses ids across instances would.
class SameIdProvider : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList& args) override {
        if (method != QLatin1String("value")) return QVariant();
        const QVariant value = args.value(0);
        // m_timers dies with the provider, so a pending completion cannot fire
        // into a later test sharing this process.
        QTimer::singleShot(20, &m_timers, [this, value]() {
            if (m_eventCb)
                m_eventCb(logos::callCompleteEvent(), QVariantList{ QStringLiteral("lc-0"), value });
        });
        QVariantMap pending;
        pending[logos::pendingCallKey()] = QStringLiteral("lc-0");
        return pending;
    }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    QJsonArray getMethods() override { return QJsonArray{}; }
    void setEventListener(EventCallback cb) override { m_eventCb = std::move(cb); }
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("qtro_same_id"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }

private:
    EventCallback m_eventCb;
    QObject m_timers;
};

void pump(int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

QVariant callAndWait(LogosAPIConsumer& consumer, const QString& value) {
    std::atomic<int> delivered{0};
    QVariant got;
    consumer.invokeRemoteMethodAsync(
        QStringLiteral("tok-1"), QStringLiteral("qtro_same_id"), QStringLiteral("value"),
        QVariantList{ value },
        [&](QVariant v, const logos::CallError&) {
            got = std::move(v);
            delivered.fetch_add(1);
        },
        Timeout(3000));
    for (int i = 0; i < 60 && delivered.load() == 0; ++i) pump(50);
    return got;
}

} // namespace

TEST(QtRemoteForeignCompletions, AnotherReplicasCompletionIsNotHandedToTheNextCall)
{
    ensureApp();
    const QString registryUrl = LogosInstance::id("qtro_same_id");

    RemoteTransportHost host(registryUrl);
    SameIdProvider provider;
    ModuleProxy proxy(&provider);
    ASSERT_TRUE(proxy.saveToken(QStringLiteral("origin"), QStringLiteral("tok-1")));
    ASSERT_TRUE(host.publishObject("qtro_same_id", &proxy));

    LogosAPIConsumer a(QStringLiteral("qtro_same_id"), QStringLiteral("origin"),
                       &TokenManager::instance());
    LogosAPIConsumer b(QStringLiteral("qtro_same_id"), QStringLiteral("origin"),
                       &TokenManager::instance());
    ASSERT_TRUE(a.isConnected());
    ASSERT_TRUE(b.isConnected());

    // A's first call gives it a replica, which then sees B's completion go by.
    EXPECT_EQ(callAndWait(a, QStringLiteral("a-1")).toString(), QStringLiteral("a-1"));
    EXPECT_EQ(callAndWait(b, QStringLiteral("b-1")).toString(), QStringLiteral("b-1"));
    pump(100);

    EXPECT_EQ(callAndWait(a, QStringLiteral("a-2")).toString(), QStringLiteral("a-2"));
}
