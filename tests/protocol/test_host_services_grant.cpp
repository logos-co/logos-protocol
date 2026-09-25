// The trust-root grant (lp_grant_host_services) and the host service it gates:
// "token_delivery" (lp_inform_module_token_to, lp_revoke_module_token_to), which
// lets capability_module push a token AT another module rather than back at core.
//
// "token_registry" (lp_token_keys) is retired since 0.13: capability_module is
// the token authority and keeps no registry. The name is still accepted and
// grants nothing, because an older host names it and a refused name costs the
// whole grant.
//
// What these cases pin above all is WHERE the gate is read. The grant is
// per-IMAGE: a host binary and a module cdylib each link their own copy of
// logos-protocol, so each has its own process-global state — the same reason a
// token the host holds has to travel over logos_module_accept_token before the
// cdylib's own TokenManager has it. A gate "simplified" into the host would be
// checked against state the calling image can never set, and the whole surface
// would fail shut with nothing to point at.
//
// Everything here is closed by default, so the first assertion of every case is
// really the same one: an image nobody granted anything to has exactly the
// pre-0.3 surface.

#include <gtest/gtest.h>

#include "local_host.h"
#include "logos_instance.h"
#include "logos_mode.h"
#include "logos_protocol.h"
#include "logos_provider_interface.h"
#include "module_proxy.h"
#include "remote_transport.h"
#include "token_manager.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QString>
#include <QVariantList>

#include <string>


namespace {

QCoreApplication* ensureApp() {
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance())
        new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

// Records the pushes that reach the implementation, so a refusal at the gate
// above it is distinguishable from a delivery that never arrived.
class RecordingProvider : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList&) override {
        return QStringLiteral("ran:") + method;
    }
    bool informModuleToken(const QString& moduleName, const QString& token) override {
        ++tokenPushes;
        lastModule = moduleName;
        TokenManager::instance().saveToken(moduleName, token);
        return true;
    }
    QJsonArray getMethods() override { return QJsonArray{}; }
    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("grant_target_module"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }

    int tokenPushes = 0;
    QString lastModule;
};

} // namespace

class HostServicesGrantTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensureApp();
        // Other suites in this binary switch the process mode; the delivery
        // case below needs the Qt-affine transport it publishes over.
        LogosModeConfig::setMode(LogosMode::Remote);
        // The grant and the token store are both process-global, so every case
        // starts from the ungranted, empty state rather than from whatever the
        // previous one left.
        ASSERT_EQ(lp_grant_host_services(nullptr), LP_OK);
        TokenManager::instance().clearAllTokens();
    }
    void TearDown() override {
        lp_grant_host_services(nullptr);
        TokenManager::instance().clearAllTokens();
    }
};

// ── token_registry, retired ─────────────────────────────────────────────────

TEST_F(HostServicesGrantTest, TokenRegistryIsAcceptedAndGrantsNothing)
{
    TokenManager::instance().saveToken(QStringLiteral("alpha"), QStringLiteral("alpha-tok"));

    ASSERT_EQ(lp_grant_host_services(R"(["token_registry"])"), LP_OK);
    EXPECT_EQ(lp_token_keys(), nullptr) << "the key set is never readable any more";
    EXPECT_EQ(lp_inform_module_token_to(nullptr, "coretok", "target", "peer", "peertok", 100),
              LP_ERR_UNSUPPORTED)
        << "the retired name must not confer anything else either";
}

TEST_F(HostServicesGrantTest, TokenRegistryBesideDeliveryKeepsDelivery)
{
    // What an older host grants capability_module. Refusing the retired name
    // would refuse token_delivery with it, and no pair token could be pushed.
    ASSERT_EQ(lp_grant_host_services(R"(["token_registry","token_delivery"])"), LP_OK);
    EXPECT_EQ(lp_token_keys(), nullptr);
    EXPECT_EQ(lp_inform_module_token_to(nullptr, "coretok", "target", "peer", "peertok", 100),
              LP_ERR_INVALID_ARG);   // past the gate, refused on arguments
}

TEST_F(HostServicesGrantTest, AnInboundPushNeverWritesTheOutboundStore)
{
    // The registry carve-out made a push to a granted image outbound as well.
    ASSERT_EQ(lp_grant_host_services(R"(["token_registry","token_delivery"])"), LP_OK);
    ASSERT_EQ(lp_token_save_inbound("peer", "peer-tok"), LP_OK);
    char* outbound = lp_token_get("peer");
    EXPECT_EQ(outbound, nullptr) << "a pushed token became one this image presents";
    lp_string_free(outbound);
    EXPECT_EQ(TokenManager::instance().inbound().token(QStringLiteral("peer")),
              QStringLiteral("peer-tok"));
}

// ── token_delivery ──────────────────────────────────────────────────────────

TEST_F(HostServicesGrantTest, TokenDeliveryIsClosedUntilGranted)
{
    // The gate is read before any argument is, so an ungranted image gets the
    // same answer whatever it passes — including a null client, which would
    // otherwise be LP_ERR_INVALID_ARG.
    EXPECT_EQ(lp_inform_module_token_to(nullptr, "coretok", "target", "peer", "peertok", 100),
              LP_ERR_UNSUPPORTED);

    // The retired name confers nothing.
    ASSERT_EQ(lp_grant_host_services(R"(["token_registry"])"), LP_OK);
    EXPECT_EQ(lp_inform_module_token_to(nullptr, "coretok", "target", "peer", "peertok", 100),
              LP_ERR_UNSUPPORTED);

    ASSERT_EQ(lp_grant_host_services(R"(["token_delivery"])"), LP_OK);
    EXPECT_EQ(lp_token_keys(), nullptr);
    // Granted, the arguments are finally what is wrong with this call.
    EXPECT_EQ(lp_inform_module_token_to(nullptr, "coretok", "target", "peer", "peertok", 100),
              LP_ERR_INVALID_ARG);
}

// The direction that makes this function necessary at all.
//
// lp_inform_module_token hardcodes capability_module as the destination
// (LogosAPIConsumer::informModuleToken), which is the consumer half of the
// exchange; a trust root pushes the other way and had no C entry point for it.
// Nothing named capability_module is published here — the only module in this
// test is grant_target_module, so the push can only be counted if it actually
// reached that module's handshake surface.
TEST_F(HostServicesGrantTest, GrantedDeliveryReachesAModuleThatIsNotCapabilityModule)
{
    const QString module = QStringLiteral("grant_target_module");
    auto host = makeLocalHost(LogosInstance::id(module));

    RecordingProvider provider;
    ModuleProxy proxy(&provider);
    ModuleHandshakeProxy handshake(&proxy);
    ASSERT_TRUE(host->publishObject(logos::handshakeObjectName(module), &handshake));

    // The trust anchor the publisher seeds before going live; without it
    // ModuleProxy's gate refuses every push (see test_handshake_surface.cpp).
    TokenManager::instance().saveToken(QStringLiteral("core"), QStringLiteral("coretok"));

    lp_client* client = lp_client_create("grant_target_module", "capability_module",
                                         nullptr, nullptr);
    ASSERT_NE(client, nullptr);

    EXPECT_EQ(lp_inform_module_token_to(client, "coretok", "grant_target_module",
                                        "peer", "peertok", 3000),
              LP_ERR_UNSUPPORTED);
    EXPECT_EQ(provider.tokenPushes, 0)
        << "a refused call must stop at the gate, not reach the module";

    ASSERT_EQ(lp_grant_host_services(R"(["token_delivery"])"), LP_OK);
    EXPECT_EQ(lp_inform_module_token_to(client, "coretok", "grant_target_module",
                                        "peer", "peertok", 3000),
              LP_OK);
    EXPECT_EQ(provider.tokenPushes, 1) << "the push never reached the module";
    EXPECT_EQ(provider.lastModule, QStringLiteral("peer"));

    lp_client_destroy(client);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}

// ── the grant itself ────────────────────────────────────────────────────────

TEST_F(HostServicesGrantTest, ClearingTheGrantReclosesTheGate)
{
    ASSERT_EQ(lp_grant_host_services(R"(["token_delivery"])"), LP_OK);
    EXPECT_EQ(lp_inform_module_token_to(nullptr, "coretok", "target", "peer", "peertok", 100),
              LP_ERR_INVALID_ARG);   // past the gate, refused on arguments

    ASSERT_EQ(lp_grant_host_services(nullptr), LP_OK);
    EXPECT_EQ(lp_inform_module_token_to(nullptr, "coretok", "target", "peer", "peertok", 100),
              LP_ERR_UNSUPPORTED);

    // An empty array is the same revoke, spelled in JSON.
    ASSERT_EQ(lp_grant_host_services(R"(["token_delivery"])"), LP_OK);
    ASSERT_EQ(lp_grant_host_services("[]"), LP_OK);
    EXPECT_EQ(lp_inform_module_token_to(nullptr, "coretok", "target", "peer", "peertok", 100),
              LP_ERR_UNSUPPORTED);

    // A grant REPLACES the previous one rather than accumulating, so a caller
    // cannot widen its own privilege one service at a time.
    ASSERT_EQ(lp_grant_host_services(R"(["token_delivery"])"), LP_OK);
    ASSERT_EQ(lp_grant_host_services(R"(["token_registry"])"), LP_OK);
    EXPECT_EQ(lp_inform_module_token_to(nullptr, "coretok", "target", "peer", "peertok", 100),
              LP_ERR_UNSUPPORTED);
}

TEST_F(HostServicesGrantTest, UnknownServiceIsRejectedAndLeavesTheGrantIntact)
{
    // Silently ignoring an unknown name would leave the caller running with a
    // mistaken idea of what it may do, and the mistake would surface much later
    // as an unexplained LP_ERR_UNSUPPORTED from an unrelated call.
    ASSERT_EQ(lp_grant_host_services(R"(["token_registry"])"), LP_OK);
    EXPECT_EQ(lp_grant_host_services(R"(["token_delivery","token_bureau"])"),
              LP_ERR_INVALID_ARG);
    EXPECT_EQ(lp_inform_module_token_to(nullptr, "coretok", "target", "peer", "peertok", 100),
              LP_ERR_UNSUPPORTED)
        << "a rejected grant must not confer the services it named";

    ASSERT_EQ(lp_grant_host_services(R"(["token_delivery"])"), LP_OK);
    EXPECT_EQ(lp_grant_host_services(R"(["token_delivery",7])"), LP_ERR_INVALID_ARG);
    EXPECT_EQ(lp_grant_host_services(R"({"token_delivery":true})"), LP_ERR_INVALID_ARG);
    EXPECT_EQ(lp_grant_host_services("not json"), LP_ERR_INVALID_ARG);
    // Every rejection is wholesale: the grant already in place survives.
    EXPECT_EQ(lp_inform_module_token_to(nullptr, "coretok", "target", "peer", "peertok", 100),
              LP_ERR_INVALID_ARG)
        << "a rejected grant must not revoke the existing one";
}
