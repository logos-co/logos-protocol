// Scoped tokens in the Qt runtime: ModuleProxy keeps each scope with its token, matched
// by token value, and refuses a method outside it before the provider runs.
#include <gtest/gtest.h>

#include "live_host_teardown.h"
#include "logos_caller_scope.h"
#include "logos_protocol.h"
#include "logos_provider_interface.h"
#include "logos_rpc_status.h"
#include "logos_transport_config.h"
#include "module_proxy.h"
#include "plain_transport_host.h"
#include "token_manager.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QString>
#include <QThread>
#include <QVariantList>

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

namespace {

using json = nlohmann::json;

QCoreApplication* ensureScopeApp()
{
    static int argc = 0;
    static char* argv[] = {nullptr};
    if (!QCoreApplication::instance()) new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

class ProbeProvider : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList&) override
    {
        ++calls;
        seen = logos::currentInboundCallerJson();
        if (method == QLatin1String("name") || method == QLatin1String("version")) return {};
        return QStringLiteral("ok");
    }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    QJsonArray getMethods() override { return {}; }
    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("probe_module"); }
    QString providerVersion() const override { return QStringLiteral("1.2.3"); }

    int calls = 0;
    std::string seen;
};

TokenManager& isolatedStore(const QString& identity)
{
    EXPECT_TRUE(TokenManager::isolateIdentity(identity));
    TokenManager& store = TokenManager::forIdentity(identity);
    store.saveToken(QStringLiteral("core"), QStringLiteral("anchor-") + identity);
    return store;
}

QString anchorOf(const QString& identity) { return QStringLiteral("anchor-") + identity; }

QString digest(const QString& token)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256).toHex());
}

const QString kScope = QStringLiteral(R"({"methods":["allowed"]})");

bool ran(const QVariant& result) { return result.toString() == QStringLiteral("ok"); }

} // namespace

TEST(ModuleProxyScopes, AScopedTokenReachesOnlyItsMethodsAndSaysSo)
{
    ensureScopeApp();
    const QString id = QStringLiteral("scope_reach");
    TokenManager& store = isolatedStore(id);
    ProbeProvider provider;
    ModuleProxy proxy(&provider, nullptr, &store);
    ASSERT_TRUE(proxy.informScopedModuleToken(anchorOf(id), QStringLiteral("peer"),
                                              QStringLiteral("tok"), kScope));

    ASSERT_TRUE(ran(proxy.callRemoteMethod(QStringLiteral("tok"), QStringLiteral("allowed"), {})));
    const json caller = json::parse(provider.seen, nullptr, false);
    EXPECT_EQ(caller.value("name", std::string{}), "peer");
    EXPECT_EQ(caller.value("scoped", false), true) << provider.seen;

    const int before = provider.calls;
    EXPECT_TRUE(logos::isNotAuthorisedSentinel(
        proxy.callRemoteMethod(QStringLiteral("tok"), QStringLiteral("forbidden"), {})));
    EXPECT_EQ(provider.calls, before) << "the provider ran a method outside the grant";
}

TEST(ModuleProxyScopes, EveryUnscopedPushAndRevocationClearsTheScope)
{
    ensureScopeApp();
    const QString id = QStringLiteral("scope_clear");
    TokenManager& store = isolatedStore(id);
    ProbeProvider provider;
    ModuleProxy proxy(&provider, nullptr, &store);
    const QString peer = QStringLiteral("peer");
    const QString tok = QStringLiteral("tok");
    const auto forbidden = [&] {
        return proxy.callRemoteMethod(tok, QStringLiteral("forbidden"), {});
    };

    ASSERT_TRUE(proxy.informScopedModuleToken(anchorOf(id), peer, tok, kScope));
    ASSERT_TRUE(logos::isNotAuthorisedSentinel(forbidden()));
    ASSERT_TRUE(proxy.informModuleToken(anchorOf(id), peer, tok));
    EXPECT_TRUE(ran(forbidden()));
    EXPECT_FALSE(json::parse(provider.seen, nullptr, false).contains("scoped")) << provider.seen;

    ASSERT_TRUE(proxy.informScopedModuleToken(anchorOf(id), peer, tok, kScope));
    ASSERT_TRUE(proxy.saveToken(peer, tok));
    EXPECT_TRUE(ran(forbidden()));

    // A stale revocation spares the scope; the matching one takes token and scope.
    ASSERT_TRUE(proxy.informScopedModuleToken(anchorOf(id), peer, tok, kScope));
    EXPECT_FALSE(proxy.revokeModuleToken(anchorOf(id), peer, digest(QStringLiteral("older"))));
    EXPECT_TRUE(logos::isNotAuthorisedSentinel(forbidden()));
    EXPECT_TRUE(proxy.revokeModuleToken(anchorOf(id), peer, digest(tok)));
    EXPECT_TRUE(logos::isUnauthorizedSentinel(forbidden()));
    ASSERT_TRUE(proxy.informModuleToken(anchorOf(id), peer, tok));
    EXPECT_TRUE(ran(forbidden())) << "a revoked scope came back with its token";
}

// The fold names keys up to 64 bytes; the scope is found by token value regardless.
TEST(ModuleProxyScopes, ALongOperatorKeyIsStillScoped)
{
    ensureScopeApp();
    const QString id = QStringLiteral("scope_long_key");
    TokenManager& store = isolatedStore(id);
    ProbeProvider provider;
    ModuleProxy proxy(&provider, nullptr, &store);
    const QString key = QStringLiteral("@op:") + QString(80, QLatin1Char('x'));
    ASSERT_TRUE(proxy.informScopedModuleToken(anchorOf(id), key, QStringLiteral("op-tok"), kScope));
    EXPECT_TRUE(ran(proxy.callRemoteMethod(QStringLiteral("op-tok"), QStringLiteral("allowed"), {})));
    EXPECT_EQ(json::parse(provider.seen, nullptr, false).value("scoped", false), true);
    EXPECT_TRUE(logos::isNotAuthorisedSentinel(
        proxy.callRemoteMethod(QStringLiteral("op-tok"), QStringLiteral("forbidden"), {})));
}

TEST(ModuleProxyScopes, AScopedTokenUnderTwoKeysIsRefused)
{
    ensureScopeApp();
    const QString id = QStringLiteral("scope_two_keys");
    TokenManager& store = isolatedStore(id);
    ProbeProvider provider;
    ModuleProxy proxy(&provider, nullptr, &store);
    ASSERT_TRUE(proxy.informModuleToken(anchorOf(id), QStringLiteral("a"), QStringLiteral("shared")));
    ASSERT_TRUE(proxy.informScopedModuleToken(anchorOf(id), QStringLiteral("b"),
                                              QStringLiteral("shared"), kScope));
    EXPECT_TRUE(logos::isNotAuthorisedSentinel(
        proxy.callRemoteMethod(QStringLiteral("shared"), QStringLiteral("allowed"), {})));
}

TEST(ModuleProxyScopes, IdentityAndIntrospectionStayOpen)
{
    ensureScopeApp();
    const QString id = QStringLiteral("scope_identity");
    TokenManager& store = isolatedStore(id);
    ProbeProvider provider;
    ModuleProxy proxy(&provider, nullptr, &store);
    ASSERT_TRUE(proxy.informScopedModuleToken(anchorOf(id), QStringLiteral("peer"),
                                              QStringLiteral("tok"), kScope));
    EXPECT_EQ(proxy.callRemoteMethod(QStringLiteral("tok"), QStringLiteral("name"), {}).toString(),
              QStringLiteral("probe_module"));
    EXPECT_EQ(proxy.callRemoteMethod(QStringLiteral("tok"), QStringLiteral("version"), {}).toString(),
              QStringLiteral("1.2.3"));
    EXPECT_TRUE(proxy.callRemoteMethod(QStringLiteral("tok"), QStringLiteral("getPluginMethods"), {})
                    .canConvert<QJsonArray>());
    EXPECT_TRUE(logos::isNotAuthorisedSentinel(
        proxy.callRemoteMethod(QStringLiteral("tok"), QStringLiteral("name"), {1})));
}

TEST(ModuleProxyScopes, AMalformedScopeAnAnchorKeyOrAStrangerIsRefused)
{
    ensureScopeApp();
    const QString id = QStringLiteral("scope_refusals");
    TokenManager& store = isolatedStore(id);
    ProbeProvider provider;
    ModuleProxy proxy(&provider, nullptr, &store);
    for (const char* scope : {"", "{}", R"({"methods":[]})", R"({"methods":["a","a"]})",
                              R"({"methods":["a"],"events":["e"]})"})
        EXPECT_FALSE(proxy.informScopedModuleToken(anchorOf(id), QStringLiteral("peer"),
                                                   QStringLiteral("tok"),
                                                   QString::fromLatin1(scope))) << scope;
    EXPECT_FALSE(proxy.informScopedModuleToken(anchorOf(id), QStringLiteral("core"),
                                               QStringLiteral("tok"), kScope));
    EXPECT_FALSE(proxy.informScopedModuleToken(QStringLiteral("not-the-anchor"),
                                               QStringLiteral("peer"), QStringLiteral("tok"),
                                               kScope));
    EXPECT_TRUE(logos::isUnauthorizedSentinel(
        proxy.callRemoteMethod(QStringLiteral("tok"), QStringLiteral("allowed"), {})));
}

TEST(ModuleProxyScopes, TheQtRuntimeCannotPushAScopedToken)
{
    EXPECT_EQ(lp_inform_scoped_module_token_to(nullptr, "a", "t", "m", "tok",
                                               R"({"methods":["x"]})", 100),
              LP_ERR_UNSUPPORTED);
}

// Over the Qt runtime's tcp host a grant refusal is an RPC failure with its own
// code, and token control names never reach the provider as methods.
TEST(ModuleProxyScopes, TheTcpHostRefusesOutsideTheGrantAndNeverDispatchesControlNames)
{
    ensureScopeApp();
    LogosTransportConfig cfg;
    cfg.protocol = LogosProtocol::Tcp;
    cfg.host = "127.0.0.1";
    cfg.port = 0;
    auto host = std::make_unique<logos::plain::PlainTransportHost>(cfg);
    ASSERT_TRUE(host->start());
    TokenManager& store = isolatedStore(QStringLiteral("scope_tcp"));
    ProbeProvider provider;
    auto* proxy = new ModuleProxy(&provider, nullptr, &store);
    ASSERT_TRUE(proxy->informScopedModuleToken(anchorOf(QStringLiteral("scope_tcp")),
                                               QStringLiteral("origin"),
                                               QStringLiteral("tcp-tok"), kScope));
    ASSERT_TRUE(proxy->informModuleToken(anchorOf(QStringLiteral("scope_tcp")),
                                         QStringLiteral("unscoped"), QStringLiteral("open-tok")));
    auto* thread = new QThread;
    proxy->moveToThread(thread);
    thread->start();
    ASSERT_TRUE(host->publishObject("scope_tcp_module", proxy));
    const QString endpoint = host->endpoint();
    const std::string target = R"({"protocol":"tcp","host":"127.0.0.1","port":)"
        + endpoint.mid(endpoint.lastIndexOf(':') + 1).toStdString() + "}";

    ASSERT_EQ(lp_token_save("scope_tcp_module", "tcp-tok"), LP_OK);
    lp_client* client = lp_client_create("scope_tcp_module", "origin", target.c_str(),
                                          target.c_str());
    ASSERT_NE(client, nullptr);
    const auto invoke = [&](const char* method, std::string& code) {
        char* result = nullptr;
        char* error = nullptr;
        const int rc = lp_invoke(client, method, "[]", 3000, &result, &error);
        code = error ? json::parse(error, nullptr, false).value("code", std::string{}) : "";
        lp_string_free(result);
        lp_string_free(error);
        return rc;
    };
    std::string code;
    EXPECT_EQ(invoke("allowed", code), LP_OK) << code;
    const int before = provider.calls;
    EXPECT_NE(invoke("forbidden", code), LP_OK);
    EXPECT_EQ(code, "not_authorised");
    // Refused whatever the caller's grant: an unscoped token too.
    ASSERT_EQ(lp_token_save("scope_tcp_module", "open-tok"), LP_OK);
    for (const char* control : {"informModuleToken", "revokeModuleToken", "informScopedModuleToken"})
        EXPECT_NE(invoke(control, code), LP_OK) << control;
    EXPECT_EQ(provider.calls, before) << "a refused or control call reached the provider";

    lp_client_destroy(client);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    logos::testing::destroyHostOnProxyThread(host, proxy);
    thread->quit();
    thread->wait();
    delete proxy;
    delete thread;
}
