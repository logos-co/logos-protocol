// Per-identity token stores: TokenManager::forIdentity / isolateIdentity, the
// construction paths that resolve a store, and the lp_* C ABI over them.
//
// WHAT THIS IS FOR. A host that loads plugins in-process writes `name -> that
// module's root auth token` into TokenManager::instance() for EVERY loaded
// module, and a client presents a cached token before it ever mints one
// (LogosAPIClient::invokeRemoteMethod reads the store first). So plugin A asking
// for module B finds B's own root token already sitting in the shared store,
// presents it, and the provider accepts — B's full authority, with no
// requestModule anywhere in the log. Giving each plugin its own ORIGIN STRING
// does not move that by one byte, because origin was never consulted on the path
// taken. forIdentity() is what makes origin SELECT the store.
//
// The file is in two halves, and both matter:
//
//   * OLD BEHAVIOUR INTACT. Every assertion that forIdentity() returns the SAME
//     OBJECT instance() returns — pointer identity, not equal contents — for a
//     name nobody isolated. That is the entire back-compatibility argument:
//     unless a host opts a specific name in, there is one store and it is the
//     one that was always there. (test_token_manager.cpp separately pins that
//     instance()'s own semantics are unchanged.)
//
//   * NEW BEHAVIOUR. Two identities do not see each other's tokens, and an
//     isolated identity cannot reach a token it was never given — asserted
//     against an ambient CONTROL in the same shape, because a test that only
//     shows the isolated case is empty of information: it would pass just as
//     well if the token had never been reachable in the first place. Every
//     escalation case here has that control, and the control fails the isolated
//     assertion, which is what makes these detectors rather than pins.
//
// WHICH OF THESE ARE DETECTORS, checked the way this suite's CMakeLists demands
// — by running them against code that does not have the mechanism, not by
// reasoning about them. The neutering was a throwaway edit made in a throwaway
// checkout: forIdentity()'s isolation branch replaced by `if (true)`, so origin
// is a LABEL again and never selects a store. That is exactly the measured dead
// end this change exists to escape (a per-plugin origin STRING with one shared
// store underneath), and everything else — isolateIdentity's bookkeeping, the
// registry, the C ABI — was left intact so only the tests that assert a SEPARATE
// store can notice.
//
// 15 of the 30 cases here go RED on that build; the other 15 are PINS of
// behaviour that is meant to be identical either way (the image store, the
// refusals, the diagnostics, the version) and correctly stay green. The three
// sharpest readings:
//
//   AnIsolatedIdentityCannotReachTheAmbientRing
//       forIdentity("walled").hasToken("target_module") is TRUE on the neutered
//       build — the walled identity is holding the target's own root token,
//       which is the escalation in one line.
//   EachIsolatedIdentityMintsAndCachesItsOwnToken
//       requestModule handshake count is 0 instead of 2: BOTH identities found
//       the target's ambient token and neither ever asked capability_module for
//       anything. Not "one handshake shared" — none at all.
//   TheKnownCallerGateStillSeesEveryHostWrittenName
//       lp_token_keys() lists "private_target" on the neutered build: the
//       identity's own minted token went straight into the shared ring, which is
//       both the leak and the reason the trust root's view has to be checked
//       rather than assumed.
//
// Do not read a green run here as evidence on its own; re-do the neutering if
// you change what these assert.

#include <gtest/gtest.h>

#include "logos_api_client.h"
#include "logos_mock.h"
#include "logos_protocol.h"
#include "token_manager.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>

#include <algorithm>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

// Every test uses identity names of its own. The registry is process-global and
// isolation is deliberately irreversible (see isolateIdentity's refusal rule),
// so there is no reset hook to call in SetUp — distinct names are what keeps the
// cases independent even when the whole binary is run in one process.
QString id(const char* suffix)
{
    return QStringLiteral("tsi_") + QString::fromLatin1(suffix);
}

struct LpClientGuard {
    explicit LpClientGuard(lp_client* c) : client(c) {}
    ~LpClientGuard() { lp_client_destroy(client); }
    lp_client* client;
};

// Read an lp_* string return into a std::string and free it. Empty for NULL,
// which every lp_token_get* uses for "absent".
std::string takeString(char* s)
{
    if (!s) return {};
    std::string out(s);
    lp_string_free(s);
    return out;
}

} // namespace

class TokenStoreIdentityTest : public ::testing::Test {
protected:
    void SetUp() override { TokenManager::instance().clearAllTokens(); }
    void TearDown() override { TokenManager::instance().clearAllTokens(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// Old behaviour intact
// ─────────────────────────────────────────────────────────────────────────────

// The load-bearing back-compat claim: same OBJECT, not merely same contents.
TEST_F(TokenStoreIdentityTest, ANeverIsolatedIdentityGetsTheImageStoreItself)
{
    EXPECT_EQ(&TokenManager::forIdentity(id("plain_a")), &TokenManager::instance());
    EXPECT_FALSE(TokenManager::isIsolated(id("plain_a")));
}

TEST_F(TokenStoreIdentityTest, EveryNonIsolatedIdentitySharesTheOneStore)
{
    EXPECT_EQ(&TokenManager::forIdentity(id("share_a")),
              &TokenManager::forIdentity(id("share_b")));
}

TEST_F(TokenStoreIdentityTest, WritesThroughTheImageStoreAreVisibleThroughForIdentity)
{
    TokenManager::instance().saveToken("some_module", "tok-ambient");
    EXPECT_EQ(TokenManager::forIdentity(id("rw_a")).getToken("some_module"),
              QStringLiteral("tok-ambient"));

    // ...and back the other way: it is one store, so a write through
    // forIdentity() lands in instance().
    TokenManager::forIdentity(id("rw_a")).saveToken("other_module", "tok-via-identity");
    EXPECT_EQ(TokenManager::instance().getToken("other_module"),
              QStringLiteral("tok-via-identity"));
}

// "" is what every un-named caller passes. Isolating it would put all of them in
// ONE shared pseudo-store, which is strictly worse than leaving them ambient.
TEST_F(TokenStoreIdentityTest, TheEmptyIdentityIsTheImageStoreAndCannotBeIsolated)
{
    EXPECT_EQ(&TokenManager::forIdentity(QString()), &TokenManager::instance());
    EXPECT_FALSE(TokenManager::isolateIdentity(QString()));
    EXPECT_FALSE(TokenManager::isolateIdentity(QStringLiteral("")));
    EXPECT_EQ(&TokenManager::forIdentity(QString()), &TokenManager::instance());
}

// ─────────────────────────────────────────────────────────────────────────────
// New behaviour: isolation
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TokenStoreIdentityTest, AnIsolatedIdentityGetsAStoreOfItsOwn)
{
    ASSERT_TRUE(TokenManager::isolateIdentity(id("own")));
    EXPECT_TRUE(TokenManager::isIsolated(id("own")));
    EXPECT_NE(&TokenManager::forIdentity(id("own")), &TokenManager::instance());
}

// The address a client captures at construction must stay valid and stay the
// same — LogosAPIClient holds it by raw pointer and dereferences it from async
// continuations that can outlive their caller.
TEST_F(TokenStoreIdentityTest, TheStoreAddressIsStableAcrossLookups)
{
    ASSERT_TRUE(TokenManager::isolateIdentity(id("stable")));
    TokenManager* first = &TokenManager::forIdentity(id("stable"));
    for (int i = 0; i < 100; ++i)
        EXPECT_EQ(&TokenManager::forIdentity(id("stable")), first);
}

// Two clients with separate stores do not see each other's tokens.
TEST_F(TokenStoreIdentityTest, TwoIsolatedIdentitiesDoNotSeeEachOthersTokens)
{
    ASSERT_TRUE(TokenManager::isolateIdentity(id("alpha")));
    ASSERT_TRUE(TokenManager::isolateIdentity(id("beta")));

    TokenManager& alpha = TokenManager::forIdentity(id("alpha"));
    TokenManager& beta  = TokenManager::forIdentity(id("beta"));
    ASSERT_NE(&alpha, &beta);

    alpha.saveToken("target", "tok-for-alpha");
    beta.saveToken("target", "tok-for-beta");

    EXPECT_EQ(alpha.getToken("target"), QStringLiteral("tok-for-alpha"));
    EXPECT_EQ(beta.getToken("target"),  QStringLiteral("tok-for-beta"));
    EXPECT_FALSE(alpha.getToken("target") == beta.getToken("target"));

    // Neither leaks into the image store, so a third, non-isolated caller does
    // not inherit either of them.
    EXPECT_TRUE(TokenManager::instance().getToken("target").isEmpty());
    EXPECT_TRUE(TokenManager::forIdentity(id("gamma")).getToken("target").isEmpty());
}

// THE ESCALATION, at the store layer. `target_module -> <target's own root
// token>` is exactly what a host writes for every loaded module; an isolated
// identity must not be able to read it. The AMBIENT CONTROL in the same test
// is what proves the assertion has content: without isolation, that token is
// right there.
TEST_F(TokenStoreIdentityTest, AnIsolatedIdentityCannotReachTheAmbientRing)
{
    TokenManager::instance().saveToken("target_module", "targets-root-token");

    // Control: an identity nobody isolated sees it, which is today's behaviour
    // and the reason this change exists.
    EXPECT_EQ(TokenManager::forIdentity(id("ambient_ctl")).getToken("target_module"),
              QStringLiteral("targets-root-token"));

    ASSERT_TRUE(TokenManager::isolateIdentity(id("walled")));
    EXPECT_TRUE(TokenManager::forIdentity(id("walled")).getToken("target_module").isEmpty());
    EXPECT_FALSE(TokenManager::forIdentity(id("walled")).hasToken("target_module"));
    EXPECT_FALSE(TokenManager::forIdentity(id("walled")).getTokenKeys()
                     .contains(QStringLiteral("target_module")));
}

// A token minted later for a DIFFERENT identity must not become visible either:
// isolation is not just a snapshot taken at creation.
TEST_F(TokenStoreIdentityTest, TokensAddedToTheRingLaterStayInvisible)
{
    ASSERT_TRUE(TokenManager::isolateIdentity(id("late")));
    TokenManager& walled = TokenManager::forIdentity(id("late"));

    TokenManager::instance().saveToken("appears_later", "root-token-added-after");
    EXPECT_TRUE(walled.getToken("appears_later").isEmpty());
    EXPECT_EQ(TokenManager::instance().getToken("appears_later"),
              QStringLiteral("root-token-added-after"));
}

TEST_F(TokenStoreIdentityTest, IsolationIsIdempotent)
{
    ASSERT_TRUE(TokenManager::isolateIdentity(id("idem")));
    TokenManager* first = &TokenManager::forIdentity(id("idem"));
    EXPECT_TRUE(TokenManager::isolateIdentity(id("idem")));
    EXPECT_EQ(&TokenManager::forIdentity(id("idem")), first);
}

// The refusal that keeps a half-isolated identity from existing. A client
// captured the shared store at construction; isolating now would leave one
// client on the ambient ring and one on the private store — "looks fixed,
// isn't", which is the failure mode this whole change exists to avoid.
TEST_F(TokenStoreIdentityTest, IsolationIsRefusedOnceTheSharedStoreWasVended)
{
    TokenManager& vended = TokenManager::forIdentity(id("too_late"));
    ASSERT_EQ(&vended, &TokenManager::instance());

    EXPECT_FALSE(TokenManager::isolateIdentity(id("too_late")));
    EXPECT_FALSE(TokenManager::isIsolated(id("too_late")));
    // ...and nothing changed: the name still resolves to the store its existing
    // clients are already pointing at.
    EXPECT_EQ(&TokenManager::forIdentity(id("too_late")), &TokenManager::instance());
}

TEST_F(TokenStoreIdentityTest, IsolatedIdentitiesAreListedForDiagnostics)
{
    ASSERT_TRUE(TokenManager::isolateIdentity(id("listed_one")));
    ASSERT_TRUE(TokenManager::isolateIdentity(id("listed_two")));
    const QStringList listed = TokenManager::isolatedIdentities();
    EXPECT_TRUE(listed.contains(id("listed_one")));
    EXPECT_TRUE(listed.contains(id("listed_two")));
    EXPECT_FALSE(listed.contains(id("never_isolated")));
}

// ─────────────────────────────────────────────────────────────────────────────
// Bootstrap seeding — the first-call path must survive isolation
// ─────────────────────────────────────────────────────────────────────────────

// A module's first call to an unknown target runs capability_module.requestModule,
// and that call authenticates with the token stored under "capability_module".
// Withhold it and an isolated identity could never obtain any token at all.
TEST_F(TokenStoreIdentityTest, APrivateStoreIsSeededWithTheBootstrapTokens)
{
    TokenManager::instance().saveToken("core", "core-tok");
    TokenManager::instance().saveToken("capability_module", "cap-tok");
    TokenManager::instance().saveToken("unrelated_module", "unrelated-root-tok");

    ASSERT_TRUE(TokenManager::isolateIdentity(id("seeded")));
    TokenManager& store = TokenManager::forIdentity(id("seeded"));

    EXPECT_EQ(store.getToken("core"), QStringLiteral("core-tok"));
    EXPECT_EQ(store.getToken("capability_module"), QStringLiteral("cap-tok"));

    // ...and NOTHING else. The bootstrap is the trust root, not a copy of the
    // ring: every other module's root token — the thing that made the ring an
    // escalation — stays behind.
    EXPECT_TRUE(store.getToken("unrelated_module").isEmpty());
    EXPECT_EQ(store.tokenCount(), 2);
    EXPECT_EQ(TokenManager::bootstrapKeys().size(), 2);
}

TEST_F(TokenStoreIdentityTest, SeedingCopiesOnlyTheKeysThatExistInTheRing)
{
    // Host seeded capability_module but not core: the private store gets what
    // there is, and no empty placeholder for what there isn't.
    TokenManager::instance().saveToken("capability_module", "cap-only");

    ASSERT_TRUE(TokenManager::isolateIdentity(id("partial")));
    TokenManager& store = TokenManager::forIdentity(id("partial"));
    EXPECT_EQ(store.getToken("capability_module"), QStringLiteral("cap-only"));
    EXPECT_FALSE(store.hasToken("core"));
    EXPECT_EQ(store.tokenCount(), 1);
}

// The ordering escape hatch: a host that learns a bootstrap token AFTER the
// private store already existed can top it up.
TEST_F(TokenStoreIdentityTest, BootstrapTokensCanBeSeededLate)
{
    ASSERT_TRUE(TokenManager::isolateIdentity(id("late_seed")));
    TokenManager& store = TokenManager::forIdentity(id("late_seed"));
    ASSERT_EQ(store.tokenCount(), 0);

    TokenManager::instance().saveToken("capability_module", "cap-late");
    EXPECT_EQ(TokenManager::seedBootstrapTokens(id("late_seed")), 1);
    EXPECT_EQ(store.getToken("capability_module"), QStringLiteral("cap-late"));

    // Idempotent: a second pass copies nothing and does not clobber a token the
    // identity has since been given directly.
    store.saveToken("capability_module", "cap-identity-specific");
    EXPECT_EQ(TokenManager::seedBootstrapTokens(id("late_seed")), 0);
    EXPECT_EQ(store.getToken("capability_module"), QStringLiteral("cap-identity-specific"));
}

// A no-op must not have side effects. seedBootstrapTokens() on a name nobody
// isolated has to avoid vending the shared store for it, or "seed then isolate"
// would silently become impossible.
TEST_F(TokenStoreIdentityTest, SeedingANonIsolatedIdentityDoesNotBlockLaterIsolation)
{
    TokenManager::instance().saveToken("capability_module", "cap-tok");
    EXPECT_EQ(TokenManager::seedBootstrapTokens(id("seed_then_iso")), 0);
    EXPECT_TRUE(TokenManager::isolateIdentity(id("seed_then_iso")));
    EXPECT_NE(&TokenManager::forIdentity(id("seed_then_iso")), &TokenManager::instance());
}

// ─────────────────────────────────────────────────────────────────────────────
// resetIdentity — the plugin-unload hook
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TokenStoreIdentityTest, ResetClearsIssuedTokensAndKeepsTheBootstrap)
{
    TokenManager::instance().saveToken("capability_module", "cap-tok");
    ASSERT_TRUE(TokenManager::isolateIdentity(id("reset_me")));

    TokenManager& store = TokenManager::forIdentity(id("reset_me"));
    store.saveToken("target", "minted-for-previous-incarnation");
    ASSERT_EQ(store.tokenCount(), 2);

    EXPECT_TRUE(TokenManager::resetIdentity(id("reset_me")));

    // The store OBJECT survives — a client mid-flight holds it by raw pointer —
    // and only its CONTENTS have a lifetime.
    EXPECT_EQ(&TokenManager::forIdentity(id("reset_me")), &store);
    EXPECT_TRUE(store.getToken("target").isEmpty());
    EXPECT_EQ(store.getToken("capability_module"), QStringLiteral("cap-tok"));
}

// Clearing the shared ring would take the host's tokens and every other
// identity's with it, so the non-isolated case must refuse rather than obey.
TEST_F(TokenStoreIdentityTest, ResetRefusesANonIsolatedIdentityAndTouchesNothing)
{
    TokenManager::instance().saveToken("capability_module", "cap-tok");
    TokenManager::instance().saveToken("some_module", "root-tok");

    EXPECT_FALSE(TokenManager::resetIdentity(id("not_isolated")));
    EXPECT_EQ(TokenManager::instance().tokenCount(), 2);
    EXPECT_EQ(TokenManager::instance().getToken("some_module"), QStringLiteral("root-tok"));
}

// ─────────────────────────────────────────────────────────────────────────────
// The construction paths: which store a client ends up holding
// ─────────────────────────────────────────────────────────────────────────────

class TokenStoreClientTest : public ::testing::Test {
protected:
    void SetUp() override { m_mock = new LogosMockSetup(); }
    void TearDown() override { delete m_mock; }
    LogosMockSetup* m_mock = nullptr;
};

// The escalation at the CLIENT layer, which is the layer that actually reads the
// store: LogosAPIClient::getToken is the hot-path lookup that finds a cached
// token and skips minting. An isolated client must come up empty for a module it
// was never given — with the ambient client in the same test showing it
// otherwise comes up full.
TEST_F(TokenStoreClientTest, AnIsolatedClientCannotPresentAnotherModulesRootToken)
{
    // Exactly what a host writes for every module it loads.
    TokenManager::instance().saveToken("victim_module", "victims-root-token");

    LogosAPIClient ambient(QStringLiteral("victim_module"),
                           id("client_ambient"),
                           &TokenManager::forIdentity(id("client_ambient")));
    EXPECT_EQ(ambient.getToken(QStringLiteral("victim_module")),
              QStringLiteral("victims-root-token"))
        << "control failed: without isolation the ambient ring must still hand "
           "over the victim's root token, or this test proves nothing";

    ASSERT_TRUE(TokenManager::isolateIdentity(id("client_walled")));
    LogosAPIClient walled(QStringLiteral("victim_module"),
                          id("client_walled"),
                          &TokenManager::forIdentity(id("client_walled")));
    EXPECT_TRUE(walled.getToken(QStringLiteral("victim_module")).isEmpty());
}

// A NULL store is the construction path lp_client_create is stuck with (its
// signature cannot be handed one), so it must mean "the store for the identity I
// said I am" rather than a crash on first use.
TEST_F(TokenStoreClientTest, ANullStoreResolvesToTheOriginsStore)
{
    ASSERT_TRUE(TokenManager::isolateIdentity(id("null_store")));
    TokenManager::forIdentity(id("null_store")).saveToken("t", "private-tok");
    TokenManager::instance().saveToken("t", "ambient-tok");

    LogosAPIClient client(QStringLiteral("t"), id("null_store"), nullptr);
    EXPECT_EQ(client.getTokenManager(), &TokenManager::forIdentity(id("null_store")));
    EXPECT_EQ(client.getToken(QStringLiteral("t")), QStringLiteral("private-tok"));

    LogosAPIClient ambient(QStringLiteral("t"), id("null_store_ambient"), nullptr);
    EXPECT_EQ(ambient.getTokenManager(), &TokenManager::instance());
    EXPECT_EQ(ambient.getToken(QStringLiteral("t")), QStringLiteral("ambient-tok"));
}

// End to end through invokeRemoteMethod: each isolated identity runs its OWN
// requestModule handshake and caches the result in its OWN store, and neither
// mint reaches the shared ring. The handshake COUNT is the observable that
// separates the two worlds — on the shared store the second identity finds the
// first one's cached token and never handshakes at all.
TEST_F(TokenStoreClientTest, EachIsolatedIdentityMintsAndCachesItsOwnToken)
{
    ASSERT_TRUE(TokenManager::isolateIdentity(id("mint_alpha")));
    ASSERT_TRUE(TokenManager::isolateIdentity(id("mint_beta")));

    // when() also seeds instance() with a dummy token for the module, which is
    // what makes the shared ring look exactly like a host's: a token for the
    // target is sitting there for the taking.
    m_mock->when("shared_target", "ping").thenReturn(QVariant("pong"));
    m_mock->when("capability_module", "requestModule")
        .withArgs(QVariantList{id("mint_alpha"), QStringLiteral("shared_target")})
        .thenReturn(QVariant("minted-for-alpha"));
    m_mock->when("capability_module", "requestModule")
        .withArgs(QVariantList{id("mint_beta"), QStringLiteral("shared_target")})
        .thenReturn(QVariant("minted-for-beta"));

    LogosAPIClient alpha(QStringLiteral("shared_target"), id("mint_alpha"),
                         &TokenManager::forIdentity(id("mint_alpha")));
    LogosAPIClient beta(QStringLiteral("shared_target"), id("mint_beta"),
                        &TokenManager::forIdentity(id("mint_beta")));

    // The bootstrap reached the CLIENT, not just the store: mintAndCacheToken
    // authenticates its requestModule with whatever getToken("capability_module")
    // returns, so an isolated identity that did not inherit this token could
    // never obtain any token at all. This is the first-call guarantee, checked
    // where it is actually consumed.
    EXPECT_EQ(alpha.getToken(QStringLiteral("capability_module")),
              QStringLiteral("mock-token-capability_module"));
    EXPECT_EQ(beta.getToken(QStringLiteral("capability_module")),
              QStringLiteral("mock-token-capability_module"));

    EXPECT_EQ(alpha.invokeRemoteMethod(QStringLiteral("shared_target"),
                                       QStringLiteral("ping"), QVariantList{}).toString(),
              QStringLiteral("pong"));
    EXPECT_EQ(beta.invokeRemoteMethod(QStringLiteral("shared_target"),
                                      QStringLiteral("ping"), QVariantList{}).toString(),
              QStringLiteral("pong"));

    EXPECT_EQ(m_mock->callCount("capability_module", "requestModule"), 2)
        << "each isolated identity must run its own handshake; a shared store "
           "would let the second caller reuse the first's cached token (or the "
           "target's own root token) and handshake zero more times";

    EXPECT_EQ(TokenManager::forIdentity(id("mint_alpha")).getToken("shared_target"),
              QStringLiteral("minted-for-alpha"));
    EXPECT_EQ(TokenManager::forIdentity(id("mint_beta")).getToken("shared_target"),
              QStringLiteral("minted-for-beta"));

    // The minted tokens stayed out of the shared ring, so a third caller does
    // not inherit either identity's authority.
    EXPECT_EQ(TokenManager::instance().getToken("shared_target"),
              QStringLiteral("mock-token-shared_target"));
}

// The control for the test above, run separately so the handshake count is its
// own: with nothing isolated, the ambient token is found and NO handshake fires.
// This is the behaviour being preserved for every host that does not opt in.
TEST_F(TokenStoreClientTest, ANonIsolatedIdentityStillShortCircuitsOnTheAmbientToken)
{
    m_mock->when("shared_target", "ping").thenReturn(QVariant("pong"));
    m_mock->when("capability_module", "requestModule").thenReturn(QVariant("should-not-mint"));

    LogosAPIClient client(QStringLiteral("shared_target"), id("ambient_mint"),
                          &TokenManager::forIdentity(id("ambient_mint")));
    EXPECT_EQ(client.invokeRemoteMethod(QStringLiteral("shared_target"),
                                        QStringLiteral("ping"), QVariantList{}).toString(),
              QStringLiteral("pong"));

    EXPECT_EQ(m_mock->callCount("capability_module", "requestModule"), 0);
    EXPECT_EQ(TokenManager::instance().getToken("shared_target"),
              QStringLiteral("mock-token-shared_target"));
}

// ─────────────────────────────────────────────────────────────────────────────
// The C ABI
// ─────────────────────────────────────────────────────────────────────────────

class TokenStoreAbiTest : public ::testing::Test {
protected:
    void SetUp() override { m_mock = new LogosMockSetup(); }
    void TearDown() override { delete m_mock; }
    LogosMockSetup* m_mock = nullptr;
};

TEST_F(TokenStoreAbiTest, IsolateAndQueryRoundTrip)
{
    EXPECT_EQ(lp_token_identity_is_isolated(id("abi_iso").toUtf8().constData()), 0);
    EXPECT_EQ(lp_token_isolate_identity(id("abi_iso").toUtf8().constData()), LP_OK);
    EXPECT_EQ(lp_token_identity_is_isolated(id("abi_iso").toUtf8().constData()), 1);
    // Idempotent, same as the C++ twin.
    EXPECT_EQ(lp_token_isolate_identity(id("abi_iso").toUtf8().constData()), LP_OK);

    EXPECT_EQ(lp_token_isolate_identity(nullptr), LP_ERR_INVALID_ARG);
    EXPECT_EQ(lp_token_isolate_identity(""), LP_ERR_INVALID_ARG);
    EXPECT_EQ(lp_token_identity_is_isolated(nullptr), LP_ERR_INVALID_ARG);
}

TEST_F(TokenStoreAbiTest, PerIdentityGetAndSaveDoNotTouchTheImageStore)
{
    ASSERT_EQ(lp_token_isolate_identity(id("abi_rw").toUtf8().constData()), LP_OK);
    const QByteArray who = id("abi_rw").toUtf8();

    ASSERT_EQ(lp_token_save("mod", "ambient-tok"), LP_OK);
    ASSERT_EQ(lp_token_save_for(who.constData(), "mod", "private-tok"), LP_OK);

    EXPECT_EQ(takeString(lp_token_get("mod")), "ambient-tok");
    EXPECT_EQ(takeString(lp_token_get_for(who.constData(), "mod")), "private-tok");

    // An identity nobody isolated reads and writes the image store — the
    // unchanged path.
    const QByteArray plain = id("abi_plain").toUtf8();
    EXPECT_EQ(takeString(lp_token_get_for(plain.constData(), "mod")), "ambient-tok");

    EXPECT_EQ(lp_token_save_for(nullptr, "mod", "x"), LP_ERR_INVALID_ARG);
    EXPECT_EQ(lp_token_save_for(who.constData(), nullptr, "x"), LP_ERR_INVALID_ARG);
    EXPECT_EQ(lp_token_save_for(who.constData(), "mod", nullptr), LP_ERR_INVALID_ARG);
    EXPECT_EQ(lp_token_get_for(nullptr, "mod"), nullptr);
    EXPECT_EQ(lp_token_get_for(who.constData(), nullptr), nullptr);
    // Absent, not refused — NULL is the documented "no token here".
    EXPECT_EQ(lp_token_get_for(who.constData(), "never_stored"), nullptr);
}

TEST_F(TokenStoreAbiTest, ResetIdentityClearsTheIssuedTokensOnly)
{
    ASSERT_EQ(lp_token_save("capability_module", "cap-tok"), LP_OK);
    const QByteArray who = id("abi_reset").toUtf8();
    ASSERT_EQ(lp_token_isolate_identity(who.constData()), LP_OK);
    ASSERT_EQ(lp_token_save_for(who.constData(), "target", "stale-tok"), LP_OK);

    EXPECT_EQ(lp_token_reset_identity(who.constData()), LP_OK);
    EXPECT_EQ(lp_token_get_for(who.constData(), "target"), nullptr);
    EXPECT_EQ(takeString(lp_token_get_for(who.constData(), "capability_module")), "cap-tok");

    // Refused for a shared store rather than silently clearing everyone's.
    EXPECT_EQ(lp_token_reset_identity(id("abi_not_isolated").toUtf8().constData()),
              LP_ERR_UNSUPPORTED);
    EXPECT_EQ(takeString(lp_token_get("capability_module")), "cap-tok");
    EXPECT_EQ(lp_token_reset_identity(nullptr), LP_ERR_INVALID_ARG);
}

TEST_F(TokenStoreAbiTest, IsolationIsRefusedOnceAClientForThatOriginExists)
{
    // lp_client_create resolves the store through the origin, so creating one
    // vends the shared store for that name — after which isolating it would
    // split the identity in half. The ABI must report that, not swallow it.
    m_mock->when("t", "ping").thenReturn(QVariant("pong"));
    const QByteArray who = id("abi_too_late").toUtf8();

    lp_client* client = lp_client_create("t", who.constData(), nullptr, nullptr);
    ASSERT_NE(client, nullptr);
    LpClientGuard guard(client);

    EXPECT_EQ(lp_token_isolate_identity(who.constData()), LP_ERR_UNSUPPORTED);
    EXPECT_EQ(lp_token_identity_is_isolated(who.constData()), 0);
}

// lp_client_create's store selection, end to end: an isolated origin's client
// cannot use the ambient token, mints its own, and caches it in its own store.
TEST_F(TokenStoreAbiTest, AnLpClientForAnIsolatedOriginUsesThatIdentitysStore)
{
    const QByteArray who = id("abi_client_iso").toUtf8();
    ASSERT_EQ(lp_token_isolate_identity(who.constData()), LP_OK);

    m_mock->when("lp_target", "ping").thenReturn(QVariant("pong"));
    m_mock->when("capability_module", "requestModule").thenReturn(QVariant("minted-for-lp"));

    lp_client* client = lp_client_create("lp_target", who.constData(), nullptr, nullptr);
    ASSERT_NE(client, nullptr);
    LpClientGuard guard(client);

    char* result = nullptr;
    ASSERT_EQ(lp_invoke(client, "ping", nullptr, 0, &result, nullptr), LP_OK);
    ASSERT_NE(result, nullptr);
    lp_string_free(result);

    EXPECT_EQ(m_mock->callCount("capability_module", "requestModule"), 1)
        << "an isolated origin must not find the target's ambient token";
    EXPECT_EQ(takeString(lp_token_get_for(who.constData(), "lp_target")), "minted-for-lp");
    // The ambient ring is untouched — still the dummy the harness seeded.
    EXPECT_EQ(takeString(lp_token_get("lp_target")), "mock-token-lp_target");
}

// The matching control: a non-isolated origin behaves exactly as it did before
// this change — ambient token found, no handshake.
TEST_F(TokenStoreAbiTest, AnLpClientForANonIsolatedOriginKeepsUsingTheImageStore)
{
    m_mock->when("lp_target", "ping").thenReturn(QVariant("pong"));
    m_mock->when("capability_module", "requestModule").thenReturn(QVariant("should-not-mint"));

    const QByteArray who = id("abi_client_ambient").toUtf8();
    lp_client* client = lp_client_create("lp_target", who.constData(), nullptr, nullptr);
    ASSERT_NE(client, nullptr);
    LpClientGuard guard(client);

    char* result = nullptr;
    ASSERT_EQ(lp_invoke(client, "ping", nullptr, 0, &result, nullptr), LP_OK);
    ASSERT_NE(result, nullptr);
    lp_string_free(result);

    EXPECT_EQ(m_mock->callCount("capability_module", "requestModule"), 0);
    EXPECT_EQ(takeString(lp_token_get("lp_target")), "mock-token-lp_target");
}

// ─────────────────────────────────────────────────────────────────────────────
// The trust root keeps working
// ─────────────────────────────────────────────────────────────────────────────
//
// capability_module's known-caller gate reads lp_token_keys() under the
// "token_registry" host service, and REFUSES an unknown origin. If per-identity
// stores blinded that gate, every first call in the system would be refused and
// the bootstrap would deadlock — so this is the constraint worth proving rather
// than asserting.
//
// The argument in full, of which the case below is the mechanical half:
//
//  1. lp_token_keys() reads TokenManager::instance() and is UNCHANGED. No
//     isolation path writes to it, removes from it, or redirects it.
//  2. When capability_module runs in its own image (its own process, or a
//     cdylib with its own copy of this library), it has its own instance() and
//     a host image's registry cannot reach it at all. Nothing to prove.
//  3. When it runs IN the host image, instance() is the shared ring. Isolation
//     only ADDS private stores; it never removes an entry. The single thing that
//     moves is a consumer-side CACHE write by an isolated identity, which now
//     lands in that identity's store — and those writes are keyed by the TARGET
//     module, while the gate consults ORIGIN names. Origin names are in
//     instance() because the HOST wrote `name -> root token` for every module it
//     loaded, and this change does not touch host writes.
//  4. The bootstrap survives because a private store is seeded with "core" and
//     "capability_module", so the identity's first requestModule authenticates
//     exactly as it did before (asserted in the seeding cases above, and at the
//     client in EachIsolatedIdentityMintsAndCachesItsOwnToken).

class TokenStoreTrustRootTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        m_mock = new LogosMockSetup();
        ASSERT_EQ(lp_grant_host_services(R"(["token_registry"])"), LP_OK);
    }
    void TearDown() override
    {
        lp_grant_host_services(nullptr);   // re-close the gate for later cases
        delete m_mock;
    }
    LogosMockSetup* m_mock = nullptr;
};

TEST_F(TokenStoreTrustRootTest, TheKnownCallerGateStillSeesEveryHostWrittenName)
{
    // What a host writes for every module it loads — and what the gate reads.
    TokenManager::instance().saveToken("loaded_mod_a", "root-a");
    TokenManager::instance().saveToken("loaded_mod_b", "root-b");

    // An isolated identity that has since cached a token of its own.
    ASSERT_TRUE(TokenManager::isolateIdentity(id("trust_iso")));
    TokenManager::forIdentity(id("trust_iso")).saveToken("private_target", "minted-privately");

    const std::string keys = takeString(lp_token_keys());
    ASSERT_FALSE(keys.empty()) << "the grant was not in effect";
    const nlohmann::json parsed = nlohmann::json::parse(keys, nullptr, false);
    ASSERT_TRUE(parsed.is_array());

    std::vector<std::string> names;
    for (const nlohmann::json& e : parsed) names.push_back(e.get<std::string>());
    auto has = [&names](const char* n) {
        return std::find(names.begin(), names.end(), n) != names.end();
    };

    // The gate's inputs are intact: both host-written names are still there.
    EXPECT_TRUE(has("loaded_mod_a"));
    EXPECT_TRUE(has("loaded_mod_b"));

    // And an isolated identity's private cache is NOT folded into the trust
    // root's view — isolation neither blinds the gate nor widens it.
    EXPECT_FALSE(has("private_target"));
}

// The version this surface shipped in. lp_token_isolate_identity and friends are
// purely additive, so MINOR moves and MAJOR does not — an older host stays
// compatible with this library and simply never isolates anything.
TEST(TokenStoreIdentityVersion, TheAdditiveSurfaceMovedMinorNotMajor)
{
    EXPECT_EQ(lp_protocol_abi_major(), 0);
    EXPECT_GE(LOGOS_PROTOCOL_VERSION_MINOR, 4);
}
