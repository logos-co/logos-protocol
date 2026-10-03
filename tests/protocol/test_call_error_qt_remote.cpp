// The same call-error channel, over the DEFAULT transport.
//
// LogosProtocol::LocalSocket (qt_remote / QtRO) is the default in
// LogosTransportConfig, so it is what an in-process module host actually uses.
// A fix that stopped at the plain runtime would leave the transport most calls
// go over reporting a timed-out call as a method that returned null.
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
// lp_invoke_async is a thin renderer over that CallError, so pinning it here
// pins the C ABI too.
//
// An unknown method NAME is the provider's unknown_method refusal, which both
// C ABI entry points must carry intact as a RESULT, distinct from a null return.

#include <gtest/gtest.h>

#include "local_host.h"
#include "logos_api_consumer.h"
#include "logos_protocol.h"
#include "logos_async_dispatch.h"
#include "logos_instance.h"
#include "logos_object.h"
#include "logos_provider_interface.h"
#include "logos_transport_config.h"
#include "module_proxy.h"
#include "remote_transport.h"
#include "token_manager.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
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

// compute() answers straight away; stall() answers the pending sentinel and
// then never pushes the completion event, so the consumer's bounded wait is
// the only thing that ends the call. echo() returns null, and any other name
// is refused with the canonical unknown_method object, as every provider does.
class StallProvider : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList& /*args*/) override {
        if (method == QLatin1String("compute")) return QVariant(7);
        if (method == QLatin1String("echo")) return QVariant();
        if (method == QLatin1String("stall")) {
            QVariantMap sentinel;
            sentinel[logos::pendingCallKey()] = QStringLiteral("never-completes");
            return sentinel;
        }
        QVariantMap unknown;
        unknown.insert(QStringLiteral("code"), QStringLiteral("unknown_method"));
        unknown.insert(QStringLiteral("message"), QStringLiteral("unknown method '%1'").arg(method));
        unknown.insert(QStringLiteral("origin"), QStringLiteral("qtro_module"));
        return unknown;
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

    auto host = makeLocalHost(registryUrl);
    StallProvider provider;
    ModuleProxy proxy(&provider);
    ASSERT_TRUE(proxy.saveToken(QStringLiteral("origin"), QStringLiteral("tok-1")));
    ASSERT_TRUE(host->publishObject("qtro_ok_module", &proxy));

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

    auto host = makeLocalHost(registryUrl);
    StallProvider provider;
    ModuleProxy proxy(&provider);
    ASSERT_TRUE(proxy.saveToken(QStringLiteral("origin"), QStringLiteral("tok-1")));
    ASSERT_TRUE(host->publishObject("qtro_stall_module", &proxy));

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

// ── an unknown method NAME, and the null it must not be confused with ────────
//
// Over the C ABI, against a module published on the local transport. Both
// entry points must carry the provider's refusal as a RESULT, intact, and a
// method that legitimately returns null must stay a null result.
namespace {

struct Capture {
    std::atomic<bool> fired{false};
    int ok = -1;
    std::string json;
};

void captureCb(int ok, const char* json, void* userData)
{
    auto* c = static_cast<Capture*>(userData);
    c->ok = ok;
    c->json = json ? json : "";
    c->fired = true;
}

bool pumpUntil(const std::function<bool()>& done, int budgetMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < budgetMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

// StallProvider published as `name` on the local transport, its token saved on both sides.
struct LocalModule {
    explicit LocalModule(const char* name)
        : host(makeLocalHost(LogosInstance::id(QString::fromLatin1(name)))), proxy(&provider)
    {
        proxy.saveToken(QStringLiteral("origin"), QStringLiteral("tok-unknown"));
        published = host && host->publishObject(QString::fromLatin1(name), &proxy);
        lp_token_save(name, "tok-unknown");
        client = lp_client_create(name, "origin", nullptr, nullptr);
    }
    ~LocalModule()
    {
        lp_client_destroy(client);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    std::unique_ptr<LogosTransportHost> host;
    StallProvider provider;
    ModuleProxy proxy;
    bool published = false;
    lp_client* client = nullptr;
};

void expectUnknownMethodRefusal(const std::string& json)
{
    const nlohmann::json v = nlohmann::json::parse(json, nullptr, false);
    ASSERT_TRUE(v.is_object()) << json;
    EXPECT_EQ(v.size(), 3u) << json;
    EXPECT_EQ(v.value("code", std::string{}), "unknown_method") << json;
    EXPECT_EQ(v.value("message", std::string{}), "unknown method 'noSuchMethod'") << json;
    EXPECT_EQ(v.value("origin", std::string{}), "qtro_module") << json;
}

} // namespace

TEST_F(QtRemoteCallErrorTest, AsyncUnknownMethodArrivesAsTheProvidersRefusal)
{
    LocalModule module("qtro_unknown_async_module");
    ASSERT_TRUE(module.published);
    ASSERT_NE(module.client, nullptr);

    Capture c;
    ASSERT_EQ(lp_invoke_async(module.client, "noSuchMethod", "[]", 5000, &captureCb, &c), LP_OK);
    ASSERT_TRUE(pumpUntil([&] { return c.fired.load(); }, 15000)) << "async callback never fired";

    EXPECT_EQ(c.ok, 1) << "a provider's refusal is a result, not a transport failure";
    expectUnknownMethodRefusal(c.json);
}

// From another thread, so this one keeps the loop running for the host and the client.
TEST_F(QtRemoteCallErrorTest, SyncUnknownMethodArrivesAsTheProvidersRefusal)
{
    LocalModule module("qtro_unknown_sync_module");
    ASSERT_TRUE(module.published);
    ASSERT_NE(module.client, nullptr);

    char* result = nullptr;
    char* error = nullptr;
    int rc = -1;
    std::atomic<bool> done{false};
    std::thread caller([&] {
        rc = lp_invoke(module.client, "noSuchMethod", "[]", 5000, &result, &error);
        done = true;
    });
    pumpUntil([&] { return done.load(); }, 15000);
    caller.join();

    EXPECT_EQ(rc, LP_OK) << "a provider's refusal is a result, not a transport failure";
    EXPECT_EQ(error, nullptr);
    ASSERT_NE(result, nullptr);
    expectUnknownMethodRefusal(result);
    lp_string_free(result);
    lp_string_free(error);
}

TEST_F(QtRemoteCallErrorTest, ALegitimateNullReturnIsStillANullResult)
{
    LocalModule module("qtro_null_module");
    ASSERT_TRUE(module.published);
    ASSERT_NE(module.client, nullptr);

    Capture c;
    ASSERT_EQ(lp_invoke_async(module.client, "echo", "[]", 5000, &captureCb, &c), LP_OK);
    ASSERT_TRUE(pumpUntil([&] { return c.fired.load(); }, 15000)) << "async callback never fired";

    EXPECT_EQ(c.ok, 1);
    EXPECT_EQ(c.json, "null");
}
