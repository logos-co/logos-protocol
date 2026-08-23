// DIRECTION. One image, one store, both directions writing the SAME name.
//
// This file is the detector for the split described in token_manager.h's
// m_tokens comment. Every test here is RED on the tree it was written against
// (c698402, "a private store is created EMPTY"); the numbers are at the bottom.
//
// THE SHAPE IT REPRODUCES IS PRODUCTION, NOT A HYPOTHETICAL. A module loaded by
// logos-module-loader-qt runs in its own process, and in THAT process one
// TokenManager takes all three writes:
//
//   * OUTBOUND  LogosAPIClient caches the token capability_module minted for
//               <me -> B> under B's name (logos_api_client.cpp:201, async twin
//               :357), reading it back at :124 / :313.
//   * INBOUND   LogosProviderBase::informModuleToken saves the token a caller
//               will present under the CALLER's name (logos-plugin-qt
//               logos_provider_object.cpp:53) — into
//               LogosAPI::getTokenManager(), the same object.
//   * ANCHOR    the module's own host-issued credential, under BOTH
//               bootstrapKeys() (module_initializer.cpp:169-170; for a cdylib
//               the generated glue's logos_module_accept_token("core") /
//               ("capability_module"), lidl_gen_cdylib_glue.cpp:412-413).
//
// ModuleProxy::authorize scans that whole map (module_proxy.cpp:419-424) and
// accepts ANY value in it. So an OUTBOUND token authorizes an INBOUND call.
//
// WHAT THAT COSTS, CONCRETELY: every capability grant is silently BIDIRECTIONAL.
// capability_module mints one value, hands it to M (which caches it outbound
// under "B") and pushes it to B (which records it inbound under "M"). B may then
// call M with it — M finds it in its own outbound cache and authorizes — though
// nothing ever granted B -> M. The grant graph the access policy
// (capability_module_plugin.cpp:99-106) is written against is directed; the
// enforcement is not.
#include <gtest/gtest.h>

#include "logos_provider_interface.h"
#include "logos_rpc_status.h"
#include "module_proxy.h"
#include "token_manager.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVariantList>

namespace {

QCoreApplication* ensureDirectionApp() {
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance())
        new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

// Stands in for LogosProviderBase::informModuleToken, whose real body is
// `m_logosAPI->getTokenManager()->saveToken(moduleName, token)` — i.e. it writes
// the SAME store the proxy authorizes against. That identity is the whole point:
// a provider that wrote somewhere else would not reproduce the collision.
class DirectionProvider : public LogosProviderObject {
public:
    explicit DirectionProvider(TokenManager* store) : m_store(store) {}

    QVariant callMethod(const QString& method, const QVariantList&) override {
        if (method == QLatin1String("work")) return QStringLiteral("worked");
        return QVariant();
    }
    bool informModuleToken(const QString& moduleName, const QString& token) override {
        if (m_store) m_store->saveToken(moduleName, token);
        return true;
    }
    QJsonArray getMethods() override {
        QJsonObject work;
        work["name"] = QStringLiteral("work");
        work["type"] = QStringLiteral("method");
        return QJsonArray{ work };
    }
    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("direction_module"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }

private:
    TokenManager* m_store;
};

bool callSucceeds(ModuleProxy& proxy, const QString& token) {
    const QVariant r = proxy.callRemoteMethod(token, QStringLiteral("work"), {});
    return !logos::isUnauthorizedSentinel(r) && r.toString() == QStringLiteral("worked");
}

// The ONE store a module process has: isolated so it is a distinct object from
// the suite-wide ambient ring (forIdentity returns instance() until a name is
// isolated), then given its OWN credential exactly as logos::admitConsumer does.
struct ModuleImage {
    explicit ModuleImage(const QString& identity, const QString& credential)
        : store(nullptr), cred(credential)
    {
        EXPECT_TRUE(TokenManager::isolateIdentity(identity));
        store = &TokenManager::forIdentity(identity);
        EXPECT_NE(store, &TokenManager::instance());
        store->adoptCredential(credential);
    }
    TokenManager* store;
    QString cred;
};

} // namespace

// ── 1. THE DETECTOR ─────────────────────────────────────────────────────────
//
// An OUTBOUND cache entry must not authorize an INBOUND call.
//
// Nothing else in this suite asserts it. test_call_caller's
// ATokenFoundOnlyInTheDirectionMixedStoreNamesNobody covers the NAMING half —
// such a token resolves to Unknown — and deliberately asserts that it still
// AUTHORIZES. That is the line this test moves.
TEST(TokenDirection, AnOutboundTokenDoesNotAuthorizeAnInboundCall)
{
    ensureDirectionApp();
    ModuleImage m(QStringLiteral("direction_outbound_only"),
                  QStringLiteral("direction-own-credential-1"));
    DirectionProvider provider(m.store);
    ModuleProxy proxy(&provider, nullptr, m.store);

    // Exactly logos_api_client.cpp:201 — the token capability_module minted for
    // <this module -> peer_b>, cached under the CALLEE's name. peer_b holds this
    // value too; it is the value peer_b was informed of.
    m.store->saveToken(QStringLiteral("peer_b"),
                       QStringLiteral("minted-for-me-to-call-peer_b"));

    EXPECT_FALSE(callSucceeds(proxy, QStringLiteral("minted-for-me-to-call-peer_b")))
        << "an outbound per-target token authorized an inbound call: the grant "
           "me->peer_b silently also grants peer_b->me";
}

// ── 2. the same name from both sides, inbound written first ─────────────────
//
// The mutual-call case: I call B and B calls me. Two DIFFERENT values, one key.
// Post-split each must survive under its own direction and only its own
// direction; today one map means the second write wins and the first value is
// simply gone.
//
// Written inbound-first so the outbound value is the survivor, which is what
// makes the third expectation a detector rather than an artefact of clobbering.
TEST(TokenDirection, TheSameNameInBothDirectionsKeepsBothValues)
{
    ensureDirectionApp();
    ModuleImage m(QStringLiteral("direction_both_ways"),
                  QStringLiteral("direction-own-credential-2"));
    DirectionProvider provider(m.store);
    ModuleProxy proxy(&provider, nullptr, m.store);

    const QString inbound  = QStringLiteral("peer_b-presents-this-to-me");
    const QString outbound = QStringLiteral("i-present-this-to-peer_b");

    // INBOUND first: capability_module pushes over the trusted channel, which
    // this store's own credential satisfies (module_proxy.cpp:287-291).
    ASSERT_TRUE(proxy.informModuleToken(m.cred, QStringLiteral("peer_b"), inbound));
    // OUTBOUND second.
    m.store->saveToken(QStringLiteral("peer_b"), outbound);

    // (a) the inbound token still authorizes — the pin.
    EXPECT_TRUE(callSucceeds(proxy, inbound))
        << "the token peer_b was issued no longer authorizes it";

    // (b) the outbound token does not — the detector.
    EXPECT_FALSE(callSucceeds(proxy, outbound))
        << "the token I present to peer_b authorized an inbound call";

    // (c) the outbound value is readable back under peer_b's name — the round
    //     trip logos_api_client.cpp:124 makes on the next call.
    EXPECT_EQ(m.store->getToken(QStringLiteral("peer_b")), outbound);
}

// ── 3. the same pair, outbound written first ────────────────────────────────
//
// The order that shows the FUNCTIONAL half, independent of any security claim:
// today the inbound push CLOBBERS the outbound cache, so this module's next call
// to peer_b goes out carrying peer_b's own inbound token. peer_b rejects it, and
// the client burns its one re-exchange (logos_api_client.cpp:136-143) recovering
// from a collision it caused itself.
TEST(TokenDirection, AnInboundPushDoesNotClobberTheOutboundCache)
{
    ensureDirectionApp();
    ModuleImage m(QStringLiteral("direction_clobber"),
                  QStringLiteral("direction-own-credential-3"));
    DirectionProvider provider(m.store);
    ModuleProxy proxy(&provider, nullptr, m.store);

    const QString outbound = QStringLiteral("cached-for-calling-peer_c");
    const QString inbound  = QStringLiteral("peer_c-presents-this-to-me");

    m.store->saveToken(QStringLiteral("peer_c"), outbound);
    ASSERT_TRUE(proxy.informModuleToken(m.cred, QStringLiteral("peer_c"), inbound));

    EXPECT_EQ(m.store->getToken(QStringLiteral("peer_c")), outbound)
        << "an inbound push overwrote the outbound per-target cache";
}

// ── 4. PINS. Everything the split must NOT break. ───────────────────────────

// The identity's own credential still authorizes inbound (the host arm), and is
// still what the trusted-channel gate compares against.
TEST(TokenDirection, TheOwnCredentialStillAuthorizesAndStillGatesPushes)
{
    ensureDirectionApp();
    ModuleImage m(QStringLiteral("direction_pin_anchor"),
                  QStringLiteral("direction-own-credential-4"));
    DirectionProvider provider(m.store);
    ModuleProxy proxy(&provider, nullptr, m.store);

    EXPECT_TRUE(callSucceeds(proxy, m.cred));
    EXPECT_TRUE(proxy.informModuleToken(m.cred, QStringLiteral("peer_d"),
                                        QStringLiteral("granted-to-peer_d")));
    EXPECT_FALSE(proxy.informModuleToken(QStringLiteral("not-the-credential"),
                                         QStringLiteral("peer_e"),
                                         QStringLiteral("granted-to-peer_e")));
}

// An inbound token authorizes, from a store nobody wrote outbound.
TEST(TokenDirection, AnInboundTokenStillAuthorizes)
{
    ensureDirectionApp();
    ModuleImage m(QStringLiteral("direction_pin_inbound"),
                  QStringLiteral("direction-own-credential-5"));
    DirectionProvider provider(m.store);
    ModuleProxy proxy(&provider, nullptr, m.store);

    ASSERT_TRUE(proxy.informModuleToken(m.cred, QStringLiteral("peer_f"),
                                        QStringLiteral("granted-to-peer_f")));
    EXPECT_TRUE(callSucceeds(proxy, QStringLiteral("granted-to-peer_f")));
}

// The outbound read path is untouched: what the client cached is what the client
// reads back. A split that broke this would break every second cross-module call.
TEST(TokenDirection, TheOutboundCacheStillReadsBack)
{
    ensureDirectionApp();
    ModuleImage m(QStringLiteral("direction_pin_outbound"),
                  QStringLiteral("direction-own-credential-6"));

    m.store->saveToken(QStringLiteral("peer_g"), QStringLiteral("tok-g"));
    EXPECT_EQ(m.store->getToken(QStringLiteral("peer_g")), QStringLiteral("tok-g"));
    EXPECT_TRUE(m.store->hasToken(QStringLiteral("peer_g")));
    EXPECT_TRUE(m.store->getTokenKeys().contains(QStringLiteral("peer_g")));
}

// ── 5. THE WHOLE MECHANISM, both halves, in the shape production builds it ──
//
// capability_module mints ONE value and it lands in two places
// (capability_module_plugin.cpp:108-131):
//
//   * pushed to B as "the token M will present"   -> B's store, keyed "M"
//   * returned to M and cached as "what I present to B"
//                                                 -> M's store, keyed "B"
//
// B's client then reads its OWN store for a token to present to M
// (logos_api_client.cpp:124 -> m_token_manager->getToken("M"), and that
// m_token_manager is the same object LogosProviderBase::informModuleToken wrote,
// logos-plugin-qt logos_api.cpp:53 / logos_provider_object.cpp:46). It finds a
// non-empty value, so it SKIPS requestModule entirely — and M authorizes it,
// because M cached the identical value outbound.
//
// So one grant M -> B silently produces B -> M, with no handshake, nothing
// logged, and capability_module's access policy
// (capability_module_plugin.cpp:99-106) never consulted. Both halves are
// asserted, because either one alone would close it.
TEST(TokenDirection, AGrantOneWayIsNotAGrantTheOtherWay)
{
    ensureDirectionApp();
    ModuleImage mi(QStringLiteral("direction_pair_m"),
                   QStringLiteral("direction-own-credential-m"));
    ModuleImage bi(QStringLiteral("direction_pair_b"),
                   QStringLiteral("direction-own-credential-b"));
    DirectionProvider providerM(mi.store);
    DirectionProvider providerB(bi.store);
    ModuleProxy proxyM(&providerM, nullptr, mi.store);
    ModuleProxy proxyB(&providerB, nullptr, bi.store);

    const QString minted = QStringLiteral("capability-minted-for-M-calling-B");

    // capability_module -> B: "M may present this".
    ASSERT_TRUE(proxyB.informModuleToken(bi.cred, QStringLiteral("direction_pair_m"),
                                         minted));
    // capability_module -> M: the return value of requestModule, cached.
    mi.store->saveToken(QStringLiteral("direction_pair_b"), minted);

    // Half one: B's client must find NOTHING to present to M, so it is forced
    // through requestModule — where the access policy lives.
    EXPECT_TRUE(bi.store->getToken(QStringLiteral("direction_pair_m")).isEmpty())
        << "B's outbound lookup for M returned the token M was issued for "
           "calling B; B's next call to M skips requestModule entirely";

    // Half two: even handed the value, M must refuse it. M never issued it.
    EXPECT_FALSE(callSucceeds(proxyM, minted))
        << "B authorized at M using the token minted for M -> B: the grant is "
           "bidirectional and the access policy was never consulted";
}

// ── HOW THIS WAS RUN, AND WHAT IT SAID ──────────────────────────────────────
//
// UNMODIFIED c698402 plus this file only — no production edit, so the reds
// below are the shipped behaviour and not a neutered build.
// `nix build .#checks.x86_64-linux.tests`, x86_64-linux, 24 cores.
//
//   99% tests passed, 4 tests failed out of 513   (160.7s)
//
//     422 - AnOutboundTokenDoesNotAuthorizeAnInboundCall   Actual: true,  want false
//     423 - TheSameNameInBothDirectionsKeepsBothValues     Actual: true,  want false
//     424 - AnInboundPushDoesNotClobberTheOutboundCache    got the INBOUND value
//                                                          under the callee's key
//     428 - AGrantOneWayIsNotAGrantTheOtherWay             BOTH halves red:
//              B's outbound lookup for M returned M's token (want empty), and
//              M authorized B with it (want refused)
//
//   425, 426, 427 PASSED — the three pins. They are green here and must stay
//   green after the split; read none of them as evidence of anything it adds.
//
// The other 509 are untouched, which is the second half of the claim: the
// collision is reachable from a store nothing else in the suite disturbs.
