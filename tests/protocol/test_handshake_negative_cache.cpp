// A probe that missed because the module was LATE must not write it off.
//
// `informModuleToken` prefers the handshake surface, because that one is published before
// the target's initializer runs and is therefore reachable while the target is still
// starting up. `acquireCachedObject` caches successes only, so a module built before that
// surface existed would otherwise pay the full 250 ms blocking probe on every single grant
// — hence the negative cache.
//
// The trap is that a probe misses for TWO reasons and only one is a property of the build:
// the module publishes no such surface, or it had not published yet. Nothing re-probes an
// entry once made — `clearObjectCache()` covers destroy and registry reconnect, and a
// module that was merely slow is neither — so caching the second reason blinds the process
// to exactly the module the handshake surface exists to serve, on every later grant.
//
// So the absence is recorded only once the BUSINESS object has answered, which is the
// evidence that the module is up and genuinely has no such surface.

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

namespace {

QCoreApplication* ensureApp() {
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance())
        new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

class CountingProvider : public LogosProviderObject {
public:
    QVariant callMethod(const QString& m, const QVariantList&) override {
        return QStringLiteral("ran:") + m;
    }
    bool informModuleToken(const QString& moduleName, const QString& token) override {
        ++tokenPushes;
        TokenManager::instance().saveToken(moduleName, token);
        return true;
    }
    QJsonArray getMethods() override { return QJsonArray{}; }
    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("nc_module"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }
    int tokenPushes = 0;
};

class ScopedToken {
public:
    ScopedToken(const QString& k, const QString& v)
        : m_key(k), m_prev(TokenManager::instance().getToken(k))
    { TokenManager::instance().saveToken(k, v); }
    explicit ScopedToken(const QString& k)
        : m_key(k), m_prev(TokenManager::instance().getToken(k)) {}
    ~ScopedToken() {
        if (m_prev.isEmpty()) TokenManager::instance().removeToken(m_key);
        else TokenManager::instance().saveToken(m_key, m_prev);
    }
private:
    QString m_key, m_prev;
};

} // namespace

class HandshakeNegativeCacheTest : public ::testing::Test {
protected:
    void SetUp() override { ensureApp(); }
};

// THE REGRESSION. The module is absent for the first grant, then comes up with ONLY its
// handshake surface — the startup state. Publishing just that surface is what makes the
// assertion discriminating: success is reachable through no other path, so a consumer that
// wrote the module off on the first miss cannot deliver.
TEST_F(HandshakeNegativeCacheTest, AModuleThatWasMerelyLateIsProbedAgainRatherThanWrittenOff)
{
    const QString module = QStringLiteral("nc_late_module");

    ScopedToken core(QStringLiteral("core"), QStringLiteral("coretok"));
    ScopedToken peer(QStringLiteral("peer"));

    LogosAPIConsumer consumer(module, QStringLiteral("capability_module"),
                              &TokenManager::instance());

    // Nothing published: both the handshake probe and the business object miss.
    EXPECT_FALSE(consumer.informModuleToken_module(
        QStringLiteral("coretok"), module, QStringLiteral("peer"),
        QStringLiteral("peertok"), 300))
        << "a grant to a module that is not there cannot succeed";

    // The module comes up. Only the handshake surface, as during init().
    RemoteTransportHost host(LogosInstance::id(module));
    CountingProvider provider;
    ModuleProxy proxy(&provider);
    ModuleHandshakeProxy handshake(&proxy);
    ASSERT_TRUE(host.publishObject(logos::handshakeObjectName(module), &handshake));

    EXPECT_TRUE(consumer.informModuleToken_module(
        QStringLiteral("coretok"), module, QStringLiteral("peer"),
        QStringLiteral("peertok"), 3000))
        << "the first probe missed because the module had not published yet, and that "
           "was cached as 'publishes no handshake surface' — so this grant skipped the "
           "one surface that could serve it and went to a business object that does not "
           "exist. Nothing would ever re-probe: clearObjectCache runs on destroy and on "
           "registry reconnect, and a late module is neither.";
    EXPECT_EQ(provider.tokenPushes, 1);
}

// AND THE OPTIMISATION SURVIVES. A module that is genuinely up and genuinely has no
// handshake surface must still be remembered, or every grant pays the 250 ms blocking
// probe for the life of the process — which is why the negative cache exists at all.
TEST_F(HandshakeNegativeCacheTest, AModuleWithNoHandshakeSurfaceIsStillRememberedOnceProven)
{
    const QString module = QStringLiteral("nc_nosurface_module");

    ScopedToken core(QStringLiteral("core"), QStringLiteral("coretok"));
    ScopedToken peer(QStringLiteral("peer"));

    // Business object only — a module built before the handshake surface existed.
    RemoteTransportHost host(LogosInstance::id(module));
    CountingProvider provider;
    ModuleProxy proxy(&provider);
    ASSERT_TRUE(host.publishObject(module, &proxy));

    LogosAPIConsumer consumer(module, QStringLiteral("capability_module"),
                              &TokenManager::instance());
    ASSERT_TRUE(consumer.isConnected());

    ASSERT_TRUE(consumer.informModuleToken_module(
        QStringLiteral("coretok"), module, QStringLiteral("peer"),
        QStringLiteral("peertok"), 3000));

    // The absence is now proven, so the second grant must not go looking again.
    RemoteTransportConnection::resetAcquireCount();
    EXPECT_TRUE(consumer.informModuleToken_module(
        QStringLiteral("coretok"), module, QStringLiteral("peer2"),
        QStringLiteral("peertok2"), 3000));
    EXPECT_EQ(RemoteTransportConnection::acquireCount(), 0)
        << "the second grant re-acquired something: the proven absence was not remembered, "
           "so every token from here pays the blocking handshake probe again";
    EXPECT_EQ(provider.tokenPushes, 2);
}
