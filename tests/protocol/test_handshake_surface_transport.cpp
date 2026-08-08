// The handshake surface, over a REAL transport.
//
// test_handshake_surface.cpp exercises ModuleHandshakeProxy by calling it
// directly, with no transport underneath. That is what let a whole class of
// defect through: the surface is only useful if a transport will PUBLISH a
// token-only QObject and a consumer can ACQUIRE it by the derived name, and a
// direct-call test cannot see either half. The adapter survey that followed
// found qt_local silently rejects a non-ModuleProxy on acquire while reporting
// a successful publish — invisible to every test in the suite.
//
// These cases close that gap on the transport the production stack actually
// uses (LogosProtocol::LocalSocket / QtRO, the LogosTransportConfig default).
//
// The scenario is modelled exactly as it occurs at startup: the handshake
// object is published and the BUSINESS object deliberately is not, because the
// business object does not exist until the module's initializer returns. That
// window is the entire reason the surface exists, and it is the one state the
// direct-call tests could never represent.

#include <gtest/gtest.h>

#include "logos_api_consumer.h"
#include "logos_instance.h"
#include "logos_provider_interface.h"
#include "module_proxy.h"
#include "remote_transport.h"
#include "token_manager.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QString>
#include <QVariantList>

#include <chrono>
#include <iostream>

namespace {

QCoreApplication* ensureApp() {
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance())
        new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

class RecordingProvider : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList&) override {
        return QStringLiteral("ran:") + method;
    }
    bool informModuleToken(const QString& moduleName, const QString& token) override {
        ++tokenPushes;
        lastModule = moduleName;
        // Mirror a real provider: the grant lands in the store the business
        // object authorizes against later.
        TokenManager::instance().saveToken(moduleName, token);
        return true;
    }
    QJsonArray getMethods() override { return QJsonArray{}; }
    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("hsx_module"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }

    int tokenPushes = 0;
    QString lastModule;
};

// Restores a process-global TokenManager key: every case in this binary shares
// the singleton, so a leaked key makes later cases order-dependent.
class ScopedToken {
public:
    ScopedToken(const QString& key, const QString& value)
        : m_key(key), m_previous(TokenManager::instance().getToken(key))
    { TokenManager::instance().saveToken(key, value); }
    explicit ScopedToken(const QString& key)
        : m_key(key), m_previous(TokenManager::instance().getToken(key)) {}
    ~ScopedToken() {
        if (m_previous.isEmpty()) TokenManager::instance().removeToken(m_key);
        else TokenManager::instance().saveToken(m_key, m_previous);
    }
private:
    QString m_key;
    QString m_previous;
};

int elapsedMs(std::chrono::steady_clock::time_point from) {
    return int(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - from).count());
}

} // namespace

class HandshakeTransportTest : public ::testing::Test {
protected:
    void SetUp() override { ensureApp(); }
};

// ── the startup window: handshake published, business object NOT ────────────
//
// If a transport refuses to publish a token-only QObject, or refuses to hand
// one back on acquire, this fails — which is precisely what no existing test
// could detect.
TEST_F(HandshakeTransportTest, TokenReachesAModuleWhoseBusinessObjectIsNotPublishedYet)
{
    const QString module = QStringLiteral("hsx_early_module");
    RemoteTransportHost host(LogosInstance::id(module));

    RecordingProvider provider;
    ModuleProxy proxy(&provider);
    ModuleHandshakeProxy handshake(&proxy);

    // Only the handshake surface goes up. This is the state during init().
    ASSERT_TRUE(host.publishObject(logos::handshakeObjectName(module), &handshake))
        << "the transport refused to publish the token-only surface";

    // The trust anchor the publisher seeds before going live (qt-sdk
    // LogosAPIProvider::seedHandshakeTrustAnchor). Without it the gate refuses
    // every push for the whole window — the defect this surface originally shipped with.
    ScopedToken core(QStringLiteral("core"), QStringLiteral("coretok"));
    ScopedToken peer(QStringLiteral("peer"));

    LogosAPIConsumer consumer(module, QStringLiteral("capability_module"),
                              &TokenManager::instance());
    ASSERT_TRUE(consumer.isConnected());

    EXPECT_TRUE(consumer.informModuleToken_module(
        QStringLiteral("coretok"), module, QStringLiteral("peer"),
        QStringLiteral("peertok"), 3000))
        << "the token did not reach a module that is still initializing — "
           "the surface is published but unreachable over this transport";

    EXPECT_EQ(provider.tokenPushes, 1) << "the push never reached the provider";
    EXPECT_EQ(provider.lastModule, QStringLiteral("peer"));
}

// ── an unseeded trust anchor is refused even over a real transport ──────────
//
// The transport-level twin of PushIsRefusedWhileTheTrustAnchorIsUnseeded: proves
// the refusal seen in production (29 of 34 app runs) is the gate rejecting the
// push, not the transport failing to deliver it.
TEST_F(HandshakeTransportTest, AnUnseededAnchorRefusesEvenThoughTheSurfaceIsReachable)
{
    const QString module = QStringLiteral("hsx_unseeded_module");
    RemoteTransportHost host(LogosInstance::id(module));

    RecordingProvider provider;
    ModuleProxy proxy(&provider);
    ModuleHandshakeProxy handshake(&proxy);
    ASSERT_TRUE(host.publishObject(logos::handshakeObjectName(module), &handshake));

    // Neither trust key exists — the pre-seeding state.
    ScopedToken restoreCore(QStringLiteral("core"));
    ScopedToken restoreCap(QStringLiteral("capability_module"));
    TokenManager::instance().removeToken(QStringLiteral("core"));
    TokenManager::instance().removeToken(QStringLiteral("capability_module"));

    LogosAPIConsumer consumer(module, QStringLiteral("capability_module"),
                              &TokenManager::instance());
    ASSERT_TRUE(consumer.isConnected());

    // The business object is absent too, so the fallback cannot rescue it and
    // the overall result is false — but the provider must never have been
    // reached, which is what distinguishes "refused" from "undelivered".
    EXPECT_FALSE(consumer.informModuleToken_module(
        QStringLiteral("coretok"), module, QStringLiteral("peer"),
        QStringLiteral("peertok"), 300));
    EXPECT_EQ(provider.tokenPushes, 0)
        << "a refused push must stop at the gate, not reach the provider";
}

// ── a module built before the surface existed: fallback, and probed ONCE ────
//
// acquireCachedObject caches successes only, so without the negative cache the
// missing handshake object costs a full blocking probe on EVERY grant. Asserted
// by timing: the first grant pays the probe, later grants must not.
TEST_F(HandshakeTransportTest, ALegacyModuleFallsBackAndIsNotReProbed)
{
    const QString module = QStringLiteral("hsx_legacy_module");
    RemoteTransportHost host(LogosInstance::id(module));

    RecordingProvider provider;
    ModuleProxy proxy(&provider);

    // Only the BUSINESS object — a module built before the handshake surface.
    ASSERT_TRUE(host.publishObject(module, &proxy));

    ScopedToken core(QStringLiteral("core"), QStringLiteral("coretok"));
    ScopedToken peer(QStringLiteral("peer"));

    LogosAPIConsumer consumer(module, QStringLiteral("capability_module"),
                              &TokenManager::instance());
    ASSERT_TRUE(consumer.isConnected());

    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_TRUE(consumer.informModuleToken_module(
        QStringLiteral("coretok"), module, QStringLiteral("peer"),
        QStringLiteral("peertok"), 3000))
        << "a module with no handshake surface must still be reachable";
    const int first = elapsedMs(t0);

    const auto t1 = std::chrono::steady_clock::now();
    EXPECT_TRUE(consumer.informModuleToken_module(
        QStringLiteral("coretok"), module, QStringLiteral("peer"),
        QStringLiteral("peertok"), 3000));
    const int second = elapsedMs(t1);

    std::cout << "  legacy module: first grant " << first
              << " ms, second " << second << " ms" << std::endl;

    EXPECT_EQ(provider.tokenPushes, 2);
    // The probe budget is 250 ms. The second grant must not pay it again.
    // Generous bound so this pins the negative cache without becoming a
    // machine-speed test: without the cache the second grant costs ~250 ms.
    EXPECT_LT(second, 150)
        << "the missing handshake surface was probed again (" << second
        << " ms) — the negative cache is not holding, so every grant pays the probe";
}
