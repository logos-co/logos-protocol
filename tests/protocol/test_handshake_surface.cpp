// The handshake surface (logos::handshakeObjectName + ModuleHandshakeProxy).
//
// A module's initializer is synchronous and routinely calls out — including
// capability_module's requestModule, which capability answers by pushing a
// token back to that same module. The business object is published only once
// the initializer returns, so that push had nothing to reach: capability waited
// for a source that could not appear until the initializer returned, and the
// initializer could not return until capability answered.
//
// The fix publishes a token-delivery-only object BEFORE the initializer runs.
// What these tests pin is that it delivers tokens into the SAME store the
// business object consults, and that it exposes nothing else — the business
// object's own publish timing is unchanged, so a caller of a real method still
// waits at acquire exactly as it always has.

#include <gtest/gtest.h>

#include "logos_provider_interface.h"
#include "module_proxy.h"
#include "token_manager.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QMetaMethod>
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
    QVariant callMethod(const QString& method, const QVariantList&) override {
        ++dispatches;
        return QStringLiteral("ran:") + method;
    }
    bool informModuleToken(const QString& moduleName, const QString& token) override {
        lastModule = moduleName;
        lastToken = token;
        ++tokenPushes;
        // Mirror what a real provider does: it stores the grant in the
        // TokenManager, which is where ModuleProxy::isAuthorized looks it up
        // later. Counting alone would not exercise the property this suite
        // cares about — that a token delivered early is honoured afterwards.
        TokenManager::instance().saveToken(moduleName, token);
        return true;
    }
    QJsonArray getMethods() override { return QJsonArray{}; }
    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("hs_module"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }

    int dispatches = 0;
    int tokenPushes = 0;
    QString lastModule;
    QString lastToken;
};

// Restores a TokenManager key: the singleton is process-global and every case in
// this binary shares it, so a leaked key would make later cases order-dependent.
class ScopedToken {
public:
    ScopedToken(const QString& key, const QString& value)
        : m_key(key), m_previous(TokenManager::instance().getToken(key))
    {
        TokenManager::instance().saveToken(key, value);
    }
    explicit ScopedToken(const QString& key)   // restore-only: for keys a test causes to appear
        : m_key(key), m_previous(TokenManager::instance().getToken(key)) {}
    ~ScopedToken() {
        if (m_previous.isEmpty()) {
            TokenManager::instance().removeToken(m_key);
        } else {
            TokenManager::instance().saveToken(m_key, m_previous);
        }
    }
private:
    QString m_key;
    QString m_previous;
};

class HandshakeSurfaceTest : public ::testing::Test {
protected:
    void SetUp() override { ensureApp(); }
};

} // namespace

// The derived name is what the publisher and capability_module must agree on.
TEST_F(HandshakeSurfaceTest, HandshakeNameIsDerivedFromTheModuleName)
{
    EXPECT_EQ(logos::handshakeObjectName(QStringLiteral("eth_rpc_module")),
              QStringLiteral("eth_rpc_module__handshake"));
    // Distinct from the business name, so publishing one never shadows the other.
    EXPECT_NE(logos::handshakeObjectName(QStringLiteral("m")), QStringLiteral("m"));
}

// A token delivered through the handshake surface must land in the same store
// the business object authorizes against — otherwise the early delivery would
// be useless once the module finishes starting up.
TEST_F(HandshakeSurfaceTest, TokenDeliveredEarlyAuthorizesLaterBusinessCalls)
{
    CountingProvider provider;
    ModuleProxy proxy(&provider);
    ModuleHandshakeProxy handshake(&proxy);

    ScopedToken coreToken(QStringLiteral("core"), QStringLiteral("coretok"));
    // The push below makes the provider store a "peer" grant; clean that up too.
    ScopedToken peerToken(QStringLiteral("peer"));

    // capability_module's push, arriving while the module is still initializing.
    ASSERT_TRUE(handshake.informModuleToken(QStringLiteral("coretok"),
                                            QStringLiteral("peer"),
                                            QStringLiteral("peertok")));
    EXPECT_EQ(provider.tokenPushes, 1);
    EXPECT_EQ(provider.lastModule, QStringLiteral("peer"));

    // Once the module is up, the peer's call is authorized by that same token.
    const QVariant r = proxy.callRemoteMethod(QStringLiteral("peertok"),
                                              QStringLiteral("doWork"), QVariantList{});
    EXPECT_EQ(r.toString(), QStringLiteral("ran:doWork"));
    EXPECT_EQ(provider.dispatches, 1);
}

// The surface is published early, so it must expose token delivery and nothing
// else: no business dispatch, no introspection of the module's methods.
TEST_F(HandshakeSurfaceTest, HandshakeSurfaceExposesOnlyTokenDelivery)
{
    CountingProvider provider;
    ModuleProxy proxy(&provider);
    ModuleHandshakeProxy handshake(&proxy);

    const QMetaObject* mo = handshake.metaObject();
    QStringList invokables;
    for (int i = mo->methodOffset(); i < mo->methodCount(); ++i) {
        const QMetaMethod m = mo->method(i);
        if (m.methodType() == QMetaMethod::Method)
            invokables << QString::fromUtf8(m.name());
    }
    EXPECT_EQ(invokables, QStringList{ QStringLiteral("informModuleToken") })
        << "the early-published surface must not grow beyond token delivery";

    // And nothing reached the implementation as a business call.
    EXPECT_EQ(provider.dispatches, 0);
}

// An unauthorized push is refused here exactly as it is on the business object —
// publishing early must not become a way around authorization.
TEST_F(HandshakeSurfaceTest, HandshakeSurfaceStillAuthorizes)
{
    CountingProvider provider;
    ModuleProxy proxy(&provider);
    ModuleHandshakeProxy handshake(&proxy);

    EXPECT_FALSE(handshake.informModuleToken(QStringLiteral("not-a-real-token"),
                                             QStringLiteral("peer"),
                                             QStringLiteral("peertok")));
    EXPECT_EQ(provider.tokenPushes, 0);
}
