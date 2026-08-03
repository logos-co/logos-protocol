// The same call-error channel, over the DEFAULT transport.
//
// LogosProtocol::LocalSocket (qt_remote / QtRO) is the default in
// LogosTransportConfig, so it is what an in-process module host actually uses.
// test_call_error_after_acquire.cpp proves the channel over plain TCP; if the
// fix stopped there, the transport most calls go over would still be reporting
// a timed-out call as a method that returned null.
//
// The failure exercised here is a DEFERRED ("multi") call whose completion
// event never arrives: RemoteLogosObject answers the pending sentinel, arms its
// bounded wait, and gives up. That branch used to deliver a bare QVariant();
// it now delivers a "timeout" CallError. Using the deferred path rather than a
// sleeping provider is deliberate — QtRO dispatches the source call on this
// same event loop, so a provider that blocked would stall the very loop the
// consumer needs, and the test would be measuring the harness.
//
// The assertions are made at LogosAPIConsumer::invokeRemoteMethodAsync, which
// is the site that used to hard-code `logos::CallError{}` next to every result.
// lp_invoke_async is a thin renderer over that CallError (proved end-to-end in
// test_call_error_after_acquire.cpp), so pinning it here pins the C ABI too.

#include <gtest/gtest.h>

#include "logos_api_consumer.h"
#include "logos_async_dispatch.h"
#include "logos_instance.h"
#include "logos_object.h"
#include "logos_provider_interface.h"
#include "logos_transport_config.h"
#include "module_proxy.h"
#include "remote_transport.h"
#include "token_manager.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

namespace {

QCoreApplication* ensureApp() {
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance())
        new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

// compute() answers straight away; stall() answers the pending sentinel and
// then never pushes the completion event, so the consumer's bounded wait is
// the only thing that ends the call.
class StallProvider : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList& /*args*/) override {
        if (method == QLatin1String("compute")) return QVariant(7);
        if (method == QLatin1String("stall")) {
            QVariantMap sentinel;
            sentinel[logos::pendingCallKey()] = QStringLiteral("never-completes");
            return sentinel;
        }
        return QVariant();
    }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    QJsonArray getMethods() override { return QJsonArray{}; }
    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("qtro_module"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }
};

} // namespace

class QtRemoteCallErrorTest : public ::testing::Test {
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

// ── control: a successful QtRO async call reports its value and NO error ────
TEST_F(QtRemoteCallErrorTest, AsyncSuccessCarriesAnEmptyError)
{
    const QString registryUrl = LogosInstance::id("qtro_ok_module");

    RemoteTransportHost host(registryUrl);
    StallProvider provider;
    ModuleProxy proxy(&provider);
    ASSERT_TRUE(proxy.saveToken(QStringLiteral("origin"), QStringLiteral("tok-1")));
    ASSERT_TRUE(host.publishObject("qtro_ok_module", &proxy));

    LogosAPIConsumer consumer(QStringLiteral("qtro_ok_module"),
                              QStringLiteral("origin"),
                              &TokenManager::instance());
    ASSERT_TRUE(consumer.isConnected());

    std::atomic<int> delivered{0};
    QVariant got;
    logos::CallError err;
    consumer.invokeRemoteMethodAsync(
        QStringLiteral("tok-1"), QStringLiteral("qtro_ok_module"),
        QStringLiteral("compute"), QVariantList{},
        [&](QVariant v, const logos::CallError& e) {
            got = std::move(v);
            err = e;
            delivered.fetch_add(1);
        },
        Timeout(3000));

    for (int i = 0; i < 60 && delivered.load() == 0; ++i) pumpEventLoop(50);

    std::cout << "  QtRO success -> ok=" << err.ok()
              << " value=" << got.toInt() << std::endl;

    ASSERT_EQ(delivered.load(), 1) << "async callback never fired";
    EXPECT_TRUE(err.ok()) << "a successful QtRO call reported error " << err.code;
    EXPECT_EQ(got.toInt(), 7);
}

// ── the deadline elapsed on the default transport ───────────────────────────
TEST_F(QtRemoteCallErrorTest, AsyncTimeoutCarriesTheCanonicalError)
{
    const QString registryUrl = LogosInstance::id("qtro_stall_module");

    RemoteTransportHost host(registryUrl);
    StallProvider provider;
    ModuleProxy proxy(&provider);
    ASSERT_TRUE(proxy.saveToken(QStringLiteral("origin"), QStringLiteral("tok-1")));
    ASSERT_TRUE(host.publishObject("qtro_stall_module", &proxy));

    LogosAPIConsumer consumer(QStringLiteral("qtro_stall_module"),
                              QStringLiteral("origin"),
                              &TokenManager::instance());
    ASSERT_TRUE(consumer.isConnected());

    std::atomic<int> delivered{0};
    QVariant got;
    logos::CallError err;
    consumer.invokeRemoteMethodAsync(
        QStringLiteral("tok-1"), QStringLiteral("qtro_stall_module"),
        QStringLiteral("stall"), QVariantList{},
        [&](QVariant v, const logos::CallError& e) {
            got = std::move(v);
            err = e;
            delivered.fetch_add(1);
        },
        Timeout(400));

    for (int i = 0; i < 100 && delivered.load() == 0; ++i) pumpEventLoop(50);

    std::cout << "  QtRO timeout -> code='" << err.code
              << "' origin='" << err.origin
              << "' message='" << err.message << "'" << std::endl;

    ASSERT_EQ(delivered.load(), 1) << "async callback never fired";
    EXPECT_EQ(err.code, "timeout") << "a timed-out QtRO call reported success";
    EXPECT_EQ(err.origin, "qtro_stall_module");
    EXPECT_FALSE(err.message.empty());
    EXPECT_FALSE(got.isValid());
}
