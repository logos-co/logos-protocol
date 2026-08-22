// The INBOUND token store: what a module accepted FROM a caller, keyed by that
// caller, and nothing else.
//
// WHY THIS FILE EXISTS. ModuleProxy authorizes an inbound call by scanning two
// stores, and until now only one of them was written by production code:
//
//   * m_tokens      — caller-keyed and inbound-only BY CONSTRUCTION (the only
//                     writer is saveToken(from_module_name, token)), but with
//                     ZERO production writers anywhere in the workspace. In a
//                     real process it was permanently empty.
//   * TokenManager  — direction-MIXED. LogosAPIClient writes the token it will
//                     PRESENT to a callee under the CALLEE's name
//                     (logos_api_client.cpp:176); lp_module_accept_token writes
//                     a token RECEIVED from a caller under the CALLER's name
//                     (logos_protocol.cpp:635). One flat QHash<QString,QString>
//                     with no direction tag, last write wins.
//
// So every inbound authorization decision rested on a store that also holds
// outbound tokens, and the store that could not hold an outbound token was
// empty. Two consequences, and each has a test below:
//
//   1. A REVERSE LOOKUP THERE CANNOT NAME A CALLER. A value in TokenManager may
//      be one we hold to CALL x, not one x holds to call US. Naming x as the
//      caller from that store would be affirmatively wrong — worse than
//      declining to answer. Item 5 (logos::currentCaller()) needs an oracle; it
//      has to be m_tokens, and m_tokens has to be complete first.
//   2. AN ISOLATED PROVIDER IDENTITY REJECTED EVERY INBOUND CALL. Inbound
//      tokens are written by LogosProviderBase::informModuleToken into
//      LogosAPI::getTokenManager() — which is TokenManager::forIdentity(name),
//      NOT instance() (logos-plugin-qt logos_api.cpp:37-38). isAuthorized
//      scanned instance() unconditionally. Identical objects for a name nobody
//      isolated, so nothing was visibly broken; the first host to isolate a
//      PROVIDER identity would have had every inbound call rejected with no
//      diagnostic. The generated Qt glue even asserts the opposite in a comment
//      ("ModuleProxy validates INBOUND calls against the host's TokenManager").
//
// HOW THIS WAS VALIDATED — three builds, each missing one mechanism, each run.
// Not a build flag and not a switch in this tree: a throwaway local edit, made
// and thrown away, exactly as the note at the top of tests/protocol/
// CMakeLists.txt prescribes.
//
//   (a) NEITHER mechanism — the store parameter present but never consulted,
//       informModuleToken not recording. 4 passed, 3 FAILED:
//         AnInformedTokenLandsInTheProxysOwnStore              (got false, want true)
//         AnIsolatedProviderIdentityStillAuthorizesInboundCalls(got false, want true)
//         AnAmbientTokenDoesNotAuthorizeAnIsolatedProxy        (got true,  want false)
//   (b) THE INBOUND RECORD ONLY — isAuthorized still scanning
//       TokenManager::instance(). 6 passed, 1 FAILED:
//         AnAmbientTokenDoesNotAuthorizeAnIsolatedProxy        (got true,  want false)
//       That single survivor is what makes it the detector for the STORE SCAN
//       rather than for the record: every other case is satisfied by either
//       mechanism alone, so only this one distinguishes them.
//   (c) THE ANCHOR READ reverted to TokenManager::instance() with everything
//       else in place. 1 FAILED:
//         TheTrustAnchorIsReadFromTheProxysOwnStore  — accepted the ambient
//         anchor (want false) AND refused the proxy's own (want true), i.e. it
//         detects the seam in both directions.
//
// ARefusedPushGrantsNothing is a detector too, and it caught a real mistake
// rather than a hypothetical one: the first draft recorded BEFORE forwarding to
// the provider and this test went red on run (b).
// AnUntrustedPushGrantsNothing, TheDefaultStoreIsStillTheAmbientRing and
// AnEmptyTokenIsRefusedEvenAgainstAnEmptyStoredValue are PINS — they are green
// on every build above, including (a). Do not read them as evidence of anything
// this change added.
//
// WHAT IS DELIBERATELY NOT HERE. There is no assertion that a token found ONLY
// in TokenManager is refused. It is still accepted, exactly as before — the
// host anchors and every bootstrap seed live there and nothing else authorizes
// them. This change adds a second, direction-pure record; it does not take the
// mixed store out of the authorization scan, and doing so would be a behaviour
// break rather than a bug fix.

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

QCoreApplication* ensureInboundApp() {
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance())
        new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

// A provider that records the tokens pushed at it and answers one method.
//
// It writes into a store handed to it at construction, which is what makes the
// isolation case reproducible in-process: this is the stand-in for
// LogosProviderBase::informModuleToken, whose real body is
// `m_logosAPI->getTokenManager()->saveToken(moduleName, token)` and whose real
// store is TokenManager::forIdentity(<this module's name>).
class RecordingProvider : public LogosProviderObject {
public:
    explicit RecordingProvider(TokenManager* store) : m_store(store) {}

    QVariant callMethod(const QString& method, const QVariantList&) override {
        if (method == QLatin1String("work")) return QStringLiteral("worked");
        return QVariant();
    }
    bool informModuleToken(const QString& moduleName, const QString& token) override {
        if (m_store) m_store->saveToken(moduleName, token);
        ++informs;
        return acceptPushes;
    }
    QJsonArray getMethods() override {
        QJsonObject work;
        work["name"] = QStringLiteral("work");
        work["type"] = QStringLiteral("method");
        return QJsonArray{ work };
    }
    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("recording_module"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }

    int  informs = 0;
    bool acceptPushes = true;

private:
    TokenManager* m_store;
};

// True when a business dispatch went through. An unauthorized call never
// reaches the provider: it comes back as the structured rejection sentinel.
bool callSucceeds(ModuleProxy& proxy, const QString& token) {
    const QVariant r = proxy.callRemoteMethod(token, QStringLiteral("work"), {});
    return !logos::isUnauthorizedSentinel(r) && r.toString() == QStringLiteral("worked");
}

// A store that is genuinely NOT instance(). forIdentity() is additive by
// design — it hands back instance() for every name until that name has been
// isolated — so a test that wants a second store has to isolate first, and must
// do so before anything else asks for the name (isolateIdentity refuses once a
// shared store has been vended under it).
TokenManager& isolatedStore(const QString& identity) {
    EXPECT_TRUE(TokenManager::isolateIdentity(identity));
    TokenManager& store = TokenManager::forIdentity(identity);
    EXPECT_NE(&store, &TokenManager::instance());
    return store;
}

// The host anchor: informModuleToken accepts ONLY a caller holding "core" or
// "capability_module" out of TokenManager::instance(). Seed it once per process
// — instance() is a singleton and these tests share it.
QString seedTrustAnchor() {
    const QString anchor = QStringLiteral("inbound-test-host-anchor");
    TokenManager::instance().saveToken(QStringLiteral("core"), anchor);
    return anchor;
}

} // namespace

// ── 1. the inbound record is actually written ────────────────────────────────
//
// FAILS BEFORE THE FIX: informModuleToken forwarded to the provider and wrote
// nothing of its own, so m_tokens stayed empty in every real process. The
// assertion is indirect but exact — the proxy's own store is private, so the
// only way to observe it is that authorization now works from a store the test
// can empty independently.
TEST(InboundTokenStore, AnInformedTokenLandsInTheProxysOwnStore)
{
    ensureInboundApp();
    const QString anchor = seedTrustAnchor();

    // The provider writes into a store that is NOT the one isAuthorized scans,
    // so nothing the provider does can make this call authorize. Only the
    // proxy's own record can. It has to be ISOLATED to be a different object:
    // forIdentity() on a name nobody isolated returns instance() itself, which
    // would make this test pass on the unfixed tree for the wrong reason.
    TokenManager& elsewhere = isolatedStore(
        QStringLiteral("inbound_store_test_provider_side"));
    RecordingProvider provider(&elsewhere);
    ModuleProxy proxy(&provider);

    const QString granted = QStringLiteral("inbound-token-alpha");
    ASSERT_TRUE(proxy.informModuleToken(anchor, QStringLiteral("caller_alpha"), granted));
    EXPECT_EQ(provider.informs, 1);

    EXPECT_TRUE(callSucceeds(proxy, granted));
}

// ── 2. the proxy's own record follows the provider's verdict ─────────────────
//
// A DETECTOR, and it earned its place: the first draft recorded BEFORE
// forwarding — on the theory that a module might call back into us from inside
// the push — and this test failed. There is no such window (the push reaches
// module code only as far as a store write), so the record goes after.
//
// READ THE SCOPE EXACTLY. This is not "a refused push leaves the token
// unusable". It cannot be: the generated Qt glue saves to the host stack BEFORE
// forwarding across the C ABI and returns hostOk && implOk, so a cdylib-side
// failure returns false with the host store already holding the token. The
// claim is only that THE PROXY adds no grant of its own to a push the provider
// rejected — which is why the provider here writes somewhere isAuthorized does
// not scan.
TEST(InboundTokenStore, ARefusedPushGrantsNothing)
{
    ensureInboundApp();
    const QString anchor = seedTrustAnchor();

    TokenManager& elsewhere = isolatedStore(
        QStringLiteral("inbound_store_test_refuse_side"));
    RecordingProvider provider(&elsewhere);
    provider.acceptPushes = false;
    ModuleProxy proxy(&provider);

    const QString refused = QStringLiteral("inbound-token-refused");
    EXPECT_FALSE(proxy.informModuleToken(anchor, QStringLiteral("caller_beta"), refused));
    EXPECT_EQ(provider.informs, 1);

    EXPECT_FALSE(callSucceeds(proxy, refused));
}

// ── 3. an untrusted push still grants nothing ────────────────────────────────
//
// A pin, not a detector: the trusted-channel gate already ran before the new
// write. It is here because the new write sits next to that gate, and the
// obvious way to get the ordering wrong is to record before checking.
TEST(InboundTokenStore, AnUntrustedPushGrantsNothing)
{
    ensureInboundApp();
    seedTrustAnchor();

    TokenManager& elsewhere = isolatedStore(
        QStringLiteral("inbound_store_test_untrusted_side"));
    RecordingProvider provider(&elsewhere);
    ModuleProxy proxy(&provider);

    const QString smuggled = QStringLiteral("inbound-token-smuggled");
    EXPECT_FALSE(proxy.informModuleToken(QStringLiteral("not-the-anchor"),
                                         QStringLiteral("caller_gamma"), smuggled));
    EXPECT_EQ(provider.informs, 0);

    EXPECT_FALSE(callSucceeds(proxy, smuggled));
}

// ── 4. an isolated provider identity still authorizes ────────────────────────
//
// THE LATENT BUG, reproduced. isAuthorized hardcoded TokenManager::instance()
// while the provider's inbound write goes to forIdentity(<own name>). For a
// name nobody isolated those are pointer-identical and nothing is visible; the
// moment a host isolates a PROVIDER identity, the two diverge and every inbound
// call is rejected.
//
// FAILS BEFORE THE FIX in both halves: without the injected store the scan
// misses the isolated store, and without the proxy's own record there is
// nothing else to match.
TEST(InboundTokenStore, AnIsolatedProviderIdentityStillAuthorizesInboundCalls)
{
    ensureInboundApp();
    const QString anchor = seedTrustAnchor();

    const QString identity = QStringLiteral("inbound_store_isolated_provider");
    ASSERT_TRUE(TokenManager::isolateIdentity(identity));
    TokenManager& isolated = TokenManager::forIdentity(identity);
    ASSERT_NE(&isolated, &TokenManager::instance());

    RecordingProvider provider(&isolated);
    ModuleProxy proxy(&provider, /*parent=*/nullptr, &isolated);

    // The trust anchor reaches an isolated store through the bootstrap seed
    // ("core" is one of TokenManager::bootstrapKeys()), which is what lets the
    // push authorize at all.
    ASSERT_EQ(isolated.getToken(QStringLiteral("core")), anchor);

    const QString granted = QStringLiteral("inbound-token-isolated");
    ASSERT_TRUE(proxy.informModuleToken(anchor, QStringLiteral("caller_delta"), granted));

    EXPECT_TRUE(callSucceeds(proxy, granted));
}

// ── 5. the injected store is the one that is scanned ─────────────────────────
//
// The other direction of the same wiring, and the reason the parameter exists
// rather than a lazy forIdentity(providerName()) lookup: a token that is ONLY
// in the ambient ring must not authorize a proxy bound to a private store.
// Without this, test 4 would pass on an implementation that scans BOTH stores,
// which would silently re-open the ambient ring the isolation work closed.
TEST(InboundTokenStore, AnAmbientTokenDoesNotAuthorizeAnIsolatedProxy)
{
    ensureInboundApp();
    seedTrustAnchor();

    const QString identity = QStringLiteral("inbound_store_isolated_scan_only");
    ASSERT_TRUE(TokenManager::isolateIdentity(identity));
    TokenManager& isolated = TokenManager::forIdentity(identity);
    ASSERT_NE(&isolated, &TokenManager::instance());

    // Planted in the AMBIENT ring only, under a key that is not a bootstrap key
    // (a bootstrap key would legitimately be copied into the private store).
    const QString ambientOnly = QStringLiteral("inbound-token-ambient-only");
    TokenManager::instance().saveToken(QStringLiteral("some_other_module"), ambientOnly);
    ASSERT_TRUE(isolated.getToken(QStringLiteral("some_other_module")).isEmpty());

    RecordingProvider provider(&isolated);
    ModuleProxy proxy(&provider, /*parent=*/nullptr, &isolated);

    EXPECT_FALSE(callSucceeds(proxy, ambientOnly));

    // CONTROL. The same value DOES authorize a proxy left on the default store,
    // so the assertion above is about which store is scanned and not about the
    // token having failed to be planted.
    RecordingProvider ambientProvider(&TokenManager::instance());
    ModuleProxy ambientProxy(&ambientProvider);
    EXPECT_TRUE(callSucceeds(ambientProxy, ambientOnly));
}

// ── 6. the default is still the ambient ring ─────────────────────────────────
//
// The back-compatibility claim, stated as a test rather than as a comment: a
// two-argument ModuleProxy — which is every construction site in the fleet —
// scans exactly what it scanned before.
TEST(InboundTokenStore, TheDefaultStoreIsStillTheAmbientRing)
{
    ensureInboundApp();
    seedTrustAnchor();

    const QString planted = QStringLiteral("inbound-token-default-store");
    TokenManager::instance().saveToken(QStringLiteral("planted_module"), planted);

    RecordingProvider provider(&TokenManager::instance());
    ModuleProxy proxy(&provider);

    EXPECT_TRUE(callSucceeds(proxy, planted));
}

// ── 7. the empty token is still refused ──────────────────────────────────────
//
// A pin on the fail-closed guard at the top of isAuthorized, which is
// load-bearing beyond its own line: constantTimeEquals("", "") is TRUE, so
// without the guard an empty token would match any empty value that ever
// reached either store. Deleting it looks like a cleanup.
TEST(InboundTokenStore, AnEmptyTokenIsRefusedEvenAgainstAnEmptyStoredValue)
{
    ensureInboundApp();
    seedTrustAnchor();

    RecordingProvider provider(&TokenManager::instance());
    ModuleProxy proxy(&provider);

    // saveToken refuses an empty value, so plant it the only way a store can
    // hold one: through the proxy's own inbound record, which refuses it too.
    EXPECT_FALSE(proxy.saveToken(QStringLiteral("empty_caller"), QString()));

    EXPECT_FALSE(callSucceeds(proxy, QString()));
}

// ── 8. the trust anchor comes from the same store ────────────────────────────
//
// informModuleToken's gate and isAuthorized's scan have to agree on WHICH store
// defines trust, or a proxy ends up with two notions of who it trusts. This
// pins the gate to the injected store: an anchor held only in the ambient ring
// is not this proxy's anchor.
//
// It also states the precondition the logos-plugin-qt wiring has to satisfy.
// LogosAPIProvider::seedHandshakeTrustAnchor writes "core"/"capability_module"
// into TokenManager::instance() by name; hand this proxy an isolated store
// without moving that write and every push during the handshake window is
// refused. That is why the parameter is opt-in and defaulted.
TEST(InboundTokenStore, TheTrustAnchorIsReadFromTheProxysOwnStore)
{
    ensureInboundApp();
    const QString ambientAnchor = seedTrustAnchor();

    const QString identity = QStringLiteral("inbound_store_isolated_anchor");
    ASSERT_TRUE(TokenManager::isolateIdentity(identity));
    TokenManager& isolated = TokenManager::forIdentity(identity);
    ASSERT_NE(&isolated, &TokenManager::instance());

    // Overwrite the seeded copy, so the two stores disagree about "core".
    const QString privateAnchor = QStringLiteral("inbound-test-private-anchor");
    isolated.saveToken(QStringLiteral("core"), privateAnchor);
    ASSERT_NE(isolated.getToken(QStringLiteral("core")),
              TokenManager::instance().getToken(QStringLiteral("core")));

    RecordingProvider provider(&isolated);
    ModuleProxy proxy(&provider, /*parent=*/nullptr, &isolated);

    EXPECT_FALSE(proxy.informModuleToken(ambientAnchor,
                                         QStringLiteral("caller_epsilon"),
                                         QStringLiteral("tok-from-ambient-anchor")));
    EXPECT_EQ(provider.informs, 0);

    EXPECT_TRUE(proxy.informModuleToken(privateAnchor,
                                        QStringLiteral("caller_zeta"),
                                        QStringLiteral("tok-from-private-anchor")));
    EXPECT_EQ(provider.informs, 1);
}

// ── 9. the scan's shape does not depend on the answer ────────────────────────
//
// WHAT THIS ASSERTS, exactly: the number of times constantTimeEquals runs for
// one inbound call is a function of the STORE SIZES only — never of where the
// matching token sits in the iteration, nor of whether anything matched at all.
// logos::tokenComparisonCount() is instrumentation for that and nothing else.
//
// WHAT IT DOES NOT ASSERT, and this matters more than what it does: it makes NO
// claim about wall-clock time, and it is not a constant-time proof. A timing
// test here would be theatre. constantTimeEquals opens with two
// QString::toUtf8() heap allocations and the loop is preceded by a QHash walk,
// both orders of magnitude noisier than the byte fold being defended; -O2 is
// free to lower the fold's mask-merge to a conditional select OR to a branch and
// nothing in a runCommand can see which it chose; separating the signal needs
// dudect-scale sampling on a shared, frequency-scaled CI runner. And the threat
// would be recovering a 122-bit random UUID by timing, which is not the threat
// this code has.
//
// The invocation-count invariant, by contrast, is deterministic, portable, and
// is precisely what an accidental `break` or early `return` destroys — which is
// the realistic regression, especially now that the loop body does more than
// OR a bool. That is the property worth pinning, so it is the one pinned.
//
// RED BEFORE: with `if (authorized) break;` added at the end of the m_tokens
// loop — the natural "we already know the answer" optimisation — the cost
// becomes the match's POSITION in the iteration. Measured:
//
//   n=1   0 of 1 measurements differ.  Undetectable, and that is the reason the
//         sweep exists: with one token the break fires after the only
//         comparison, so the cost is identical to a miss.
//   n=2   1 of 2 differ — 4 against a miss cost of 5.
//   n=50  49 of 50 differ, ranging 52..100 against a miss cost of 101. The one
//         that agrees is whichever token QHash happens to iterate last.
//
// Removing the early exit again returns every one of the 53 measurements to its
// store-size cost. The n=50 spread is also the leak stated plainly: the number
// of comparisons is the presented token's position among the issued ones.
TEST(InboundTokenStore, TheComparisonCountDependsOnStoreSizeOnly)
{
    ensureInboundApp();

    unsigned long long previousTotal = 0;

    for (const int n : { 1, 2, 50 }) {
        const QString identity =
            QStringLiteral("inbound_store_ct_scan_%1").arg(n);
        ASSERT_TRUE(TokenManager::isolateIdentity(identity));
        TokenManager& store = TokenManager::forIdentity(identity);
        ASSERT_NE(&store, &TokenManager::instance());

        // The anchor goes straight into this proxy's own store: a private store
        // copies the bootstrap keys when it is CREATED, so seeding instance()
        // afterwards would never reach it.
        const QString anchor = QStringLiteral("ct-scan-anchor-%1").arg(n);
        store.saveToken(QStringLiteral("core"), anchor);

        RecordingProvider provider(&store);
        ModuleProxy proxy(&provider, /*parent=*/nullptr, &store);

        QStringList issued;
        for (int i = 0; i < n; ++i) {
            const QString token = QStringLiteral("ct-scan-token-%1-%2").arg(n).arg(i);
            ASSERT_TRUE(proxy.informModuleToken(
                anchor, QStringLiteral("ct_caller_%1").arg(i), token));
            issued << token;
        }

        // The reference: a token that matches NOTHING, so the scan runs to the
        // end of both stores with no early exit available to it.
        const unsigned long long beforeMiss = logos::tokenComparisonCount();
        EXPECT_FALSE(callSucceeds(proxy, QStringLiteral("ct-scan-no-such-token")));
        const unsigned long long missCost =
            logos::tokenComparisonCount() - beforeMiss;

        // EVERY issued token, not a sampled first and last. QHash iteration
        // order is unspecified, so "the match at index 0" is not something a
        // test can arrange — measuring all n makes the claim order-independent
        // and covers whichever position each token actually lands in.
        for (const QString& token : issued) {
            const unsigned long long before = logos::tokenComparisonCount();
            EXPECT_TRUE(callSucceeds(proxy, token));
            EXPECT_EQ(logos::tokenComparisonCount() - before, missCost)
                << "n=" << n << " token=" << token.toStdString();
        }

        // Anti-vacuity, both directions. A counter stuck at zero, or one the
        // scan stopped feeding, would satisfy every equality above.
        EXPECT_GE(missCost, static_cast<unsigned long long>(n));
        EXPECT_GT(missCost, previousTotal);
        previousTotal = missCost;
    }
}
