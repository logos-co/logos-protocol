// Module identity at the proxy layer — name() and version() for every module.
//
// A module built through the LIDL frontend has these generated into its own
// dispatch and never reaches the code under test here. A LEGACY module derives
// no contract and has neither, yet its provider has always known both through
// the providerName()/providerVersion() vtable slots. ModuleProxy answers from
// those, which is what makes identity uniform across the fleet without editing
// a single legacy module.
//
// Two properties carry the whole design, and each has a test that fails if it
// is lost:
//   * the fallback is AFTER dispatch, so a provider that answers for itself
//     keeps its own result (no silent shadowing);
//   * the dispatch answer and the introspection listing agree, so a module
//     cannot answer a method it claims not to have.

#include <gtest/gtest.h>

#include "logos_provider_interface.h"
#include "module_proxy.h"
#include "token_manager.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVariantList>

namespace {

QCoreApplication* ensureIdentityApp() {
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance())
        new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

// A legacy-shaped provider: it knows its identity (every provider does) but
// exposes no identity METHOD, which is exactly the fleet's 40-odd legacy
// modules.
class LegacyProvider : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList& args) override {
        ++calls;
        lastMethod = method;
        lastArgs = args;
        if (method == QLatin1String("work")) return QStringLiteral("worked");
        return QVariant();  // this slot's "unknown method"
    }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    QJsonArray getMethods() override {
        QJsonObject work;
        work["name"] = QStringLiteral("work");
        work["type"] = QStringLiteral("method");
        return QJsonArray{ work };
    }
    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("legacy_module"); }
    QString providerVersion() const override { return QStringLiteral("3.2.1"); }

    int calls = 0;
    QString lastMethod;
    QVariantList lastArgs;
};

// A provider that answers name() itself — the shape a generated (or
// hand-written) module has. logos-delivery-module really does this.
class SelfAnsweringProvider : public LegacyProvider {
public:
    QVariant callMethod(const QString& method, const QVariantList& args) override {
        if (method == QLatin1String("name")) return QStringLiteral("its-own-answer");
        return LegacyProvider::callMethod(method, args);
    }
    QJsonArray getMethods() override {
        QJsonObject name;
        name["name"] = QStringLiteral("name");
        name["type"] = QStringLiteral("method");
        name["description"] = QStringLiteral("hand-written");
        return QJsonArray{ name };
    }
};

// A token this proxy has actually issued, so calls are authorized. Mirrors what
// capability_module's informModuleToken does for a (caller, target) pair.
QString authorize(ModuleProxy& proxy) {
    const QString token = QStringLiteral("identity-test-token");
    proxy.saveToken(QStringLiteral("some_caller"), token);
    return token;
}

QStringList methodNames(const QJsonArray& iface) {
    QStringList names;
    for (const QJsonValue& v : iface)
        if (v.isObject()) names << v.toObject().value("name").toString();
    return names;
}

} // namespace

TEST(ModuleIdentity, ALegacyProviderAnswersNameAndVersion)
{
    ensureIdentityApp();
    LegacyProvider provider;
    ModuleProxy proxy(&provider);
    const QString token = authorize(proxy);

    EXPECT_EQ(proxy.callRemoteMethod(token, QStringLiteral("name"), {}).toString(),
              QStringLiteral("legacy_module"));
    EXPECT_EQ(proxy.callRemoteMethod(token, QStringLiteral("version"), {}).toString(),
              QStringLiteral("3.2.1"));
}

TEST(ModuleIdentity, TheProvidersOwnAnswerWins)
{
    // The fallback runs only when dispatch returned "unknown method". Shadowing
    // a provider that implements name() would silently change its behaviour.
    ensureIdentityApp();
    SelfAnsweringProvider provider;
    ModuleProxy proxy(&provider);
    const QString token = authorize(proxy);

    EXPECT_EQ(proxy.callRemoteMethod(token, QStringLiteral("name"), {}).toString(),
              QStringLiteral("its-own-answer"));
    // ...and the one it does NOT implement still falls back.
    EXPECT_EQ(proxy.callRemoteMethod(token, QStringLiteral("version"), {}).toString(),
              QStringLiteral("3.2.1"));
}

TEST(ModuleIdentity, IntrospectionAndDispatchAgree)
{
    // A module that answers a method it claims not to have is present to
    // whoever already knew to ask and invisible to `lm` and every untyped
    // caller. Whatever the listing says must be callable, and vice versa.
    ensureIdentityApp();
    LegacyProvider provider;
    ModuleProxy proxy(&provider);
    const QString token = authorize(proxy);

    const QStringList listed = methodNames(proxy.getPluginInterface());
    EXPECT_TRUE(listed.contains(QStringLiteral("name"))) << listed.join(", ").toStdString();
    EXPECT_TRUE(listed.contains(QStringLiteral("version"))) << listed.join(", ").toStdString();
    EXPECT_TRUE(listed.contains(QStringLiteral("work"))) << listed.join(", ").toStdString();

    for (const QString& m : { QStringLiteral("name"), QStringLiteral("version") })
        EXPECT_TRUE(proxy.callRemoteMethod(token, m, {}).isValid()) << m.toStdString();
}

TEST(ModuleIdentity, TheListingDoesNotDuplicateWhatTheProviderAlreadyLists)
{
    // Additive only: a provider that lists name() keeps ITS entry, description
    // and all. A duplicate would make `lm` show the method twice and leave
    // consumers guessing which one is real.
    ensureIdentityApp();
    SelfAnsweringProvider provider;
    ModuleProxy proxy(&provider);

    const QJsonArray iface = proxy.getPluginInterface();
    EXPECT_EQ(methodNames(iface).count(QStringLiteral("name")), 1);

    for (const QJsonValue& v : iface) {
        const QJsonObject o = v.toObject();
        if (o.value("name").toString() == QLatin1String("name"))
            EXPECT_EQ(o.value("description").toString(), QStringLiteral("hand-written"));
    }
}

TEST(ModuleIdentity, ASameNamedMethodTakingArgumentsIsUntouched)
{
    // The fallback is gated on an empty argument list, so a module with its own
    // `name(which)` reaches its dispatch exactly as before — including when
    // that call legitimately fails and returns an invalid QVariant.
    ensureIdentityApp();
    LegacyProvider provider;
    ModuleProxy proxy(&provider);
    const QString token = authorize(proxy);

    const QVariant r = proxy.callRemoteMethod(
        token, QStringLiteral("name"), QVariantList{ QStringLiteral("which") });

    EXPECT_FALSE(r.isValid());
    EXPECT_EQ(provider.lastMethod, QStringLiteral("name"));
    EXPECT_EQ(provider.lastArgs.size(), 1);
}

TEST(ModuleIdentity, IdentityIsStillGatedOnAuthorization)
{
    // Identity is a method, not introspection: it must sit behind the same auth
    // gate as any other call. The three getPlugin* calls are ungated ON PURPOSE
    // (they precede the token exchange); name()/version() are not.
    ensureIdentityApp();
    LegacyProvider provider;
    ModuleProxy proxy(&provider);
    authorize(proxy);

    const QVariant r =
        proxy.callRemoteMethod(QStringLiteral("not-a-token"), QStringLiteral("name"), {});
    EXPECT_NE(r.toString(), QStringLiteral("legacy_module"));
    EXPECT_EQ(provider.calls, 0);
}
