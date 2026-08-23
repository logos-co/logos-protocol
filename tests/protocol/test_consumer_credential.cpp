// AN ISOLATED IDENTITY'S OWN CREDENTIAL — the two properties that pull in
// opposite directions, and why neither is evidence without the other.
//
// A host that loads several callers into ONE process gives each of them a
// private token store (TokenManager::isolateIdentity). Until this change that
// store was CREATED by copying instance()'s tokens for bootstrapKeys() —
// "core" and "capability_module" — which are the HOST's credential. Two things
// followed, and only the first of them was latent:
//
//   * ELEVATION AT NAMING. Presenting the host's anchor hits an anchor key in
//     the callee's m_store, so ModuleProxy::authorize answers
//     logos::callerHostAnchorJson(): a sandboxed in-process view is reported as
//     the host. Latent only because both anchor targets are legacy Q_INVOKABLE
//     plugins with no generated glue to read a caller.
//   * ELEVATION AT AUTHORITY, which is live. informModuleToken's
//     trusted-channel gate compares the presenting token against the SAME two
//     keys, so any holder of the copy can push arbitrary (name, token) pairs
//     into another module's token map — three public calls, no glue.
//
// THE COPY EXISTED FOR A REAL REASON and deleting it alone is not a fix. The
// credential under "capability_module" is what authenticates
// `capability_module.requestModule`; without SOME value there,
// ModuleProxy::authorize refuses at its empty-token check and the identity can
// never obtain a token for anything. Isolation becomes a LOCKOUT, which is
// worse than the elevation it replaces.
//
// So the fix is not "remove the seed" but "seed the RIGHT value": the identity's
// OWN host-issued credential, minted by the host and registered with
// capability_module before it is adopted. Every other image in the system
// already does exactly this (LogosAPIProvider::seedHandshakeTrustAnchor for a
// module image; ui-host for a view process). The in-process private store was
// the only store seeded with somebody else's credential.
//
// THE TWO TESTS THAT MATTER, and each fails without the other's half:
//
//   NO ANCHOR   AnIsolatedIdentityHoldsNoValueOfTheHosts
//               AnAdmittedConsumerIsNamedAsItselfAndNotAsTheHost
//               AnIsolatedIdentityWithNoCredentialIsRefusedEverywhere
//   NO LOCKOUT  AnAdmittedConsumerCompletesTheHandshakeAndReachesAnOrdinaryModule
//               AnAdmittedProviderIdentityStillAcceptsAPushFromCapability
//
// HOW THIS WAS VALIDATED — three builds, each run in full (506 tests), each
// missing something the real tree has. Throwaway local edits, made and thrown
// away, exactly as the note at the top of tests/protocol/CMakeLists.txt
// prescribes; not a build flag and not a switch in this tree.
//
//   (M0) TODAY'S BEHAVIOUR — forIdentity() seeding a new private store from
//        instance()'s bootstrap keys, AND adoptCredentialFor() reduced to
//        `return true` without writing. That pair is exactly master: the copy
//        exists, and no host anywhere hands an identity a credential of its own
//        (all five registration sites mint a UUID, register it, and drop it).
//        16 of 506 FAILED. The NO ANCHOR set is red, and the sharpest single
//        reading is
//          AnAdmittedConsumerIsNamedAsItselfAndNotAsTheHost
//              — {"kind":"host"} where {"kind":"module","name":...} is wanted.
//              That is the elevation in one line: a sandboxed in-process view
//              reported as basecamp.
//        So is the outcome-shaped statement of the same thing,
//          AnIsolatedIdentityWithNoCredentialIsRefusedEverywhere
//              — an identity nobody credentialed calls capability_module
//              successfully, because it inherited the right to.
//
//   (M1) THE COPY, WITH ADOPTION WORKING — only forIdentity()'s seeding
//        restored. 7 of 506 FAILED, and WHICH seven is the interesting part:
//        AnAdmittedConsumerIsNamedAsItselfAndNotAsTheHost stays GREEN, because
//        adoption overwrites the copy for any identity that is admitted. The
//        copy is only observable on an identity nobody adopted — which is why
//        APrivateStoreIsBornEmpty and
//        AnIsolatedIdentityWithNoCredentialIsRefusedEverywhere have to exist
//        as separate cases rather than being folded into the admitted ones.
//
//   (M2) THE COPY REMOVED AND ADOPTION NEUTERED — the "just delete the seed"
//        change, on its own. 10 of 506 FAILED: every NO ANCHOR case goes GREEN
//        and every NO LOCKOUT case goes RED —
//          AnAdmittedConsumerCompletesTheHandshakeAndReachesAnOrdinaryModule
//              — requestModule refused; the consumer has nothing to present
//          AnAdmittedProviderIdentityStillAcceptsAPushFromCapability
//              — informModuleToken refused; the identity can never be told
//                about any caller
//          InboundTokenStore.AnIsolatedProviderIdentityStillAuthorizesInboundCalls
//          TokenStoreClientTest.EachIsolatedIdentityMintsAndCachesItsOwnToken
//        That is the live outage this file exists to make unshippable by
//        accident: passing NO ANCHOR alone is not progress, it is a different
//        and worse bug.
//
// AnAdmittedConsumerIsNamedAsItselfAndNotAsTheHost is red on M0 AND on M2 and
// green only on the real tree, which makes it the one case that detects each
// half's absence on its own. AHostThatNeverIsolatesSeesNoChangeAtAll is a PIN —
// green on all three — and must not be read as evidence of anything this change
// added; it is the compatibility claim, and its job is to stay green.

#include <gtest/gtest.h>

#include "logos_caller_scope.h"
#include "logos_provider_interface.h"
#include "logos_rpc_status.h"
#include "module_proxy.h"
#include "token_manager.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <string>

namespace {

QCoreApplication* ensureCredApp() {
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance())
        new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

const char* kHostDoc = R"({"kind":"host"})";
std::string moduleDoc(const QString& name) {
    return std::string(R"({"kind":"module","name":")") + name.toStdString() + R"("})";
}

// A stand-in for a real module behind a ModuleProxy: it answers one method,
// records who the dispatch said was calling, and mirrors
// LogosProviderBase::informModuleToken by writing the pushed token into the
// store it was handed (which for the Qt stack is
// TokenManager::forIdentity(<this module's name>)).
class ProbeProvider : public LogosProviderObject {
public:
    ProbeProvider(QString name, TokenManager* store)
        : m_name(std::move(name)), m_store(store) {}

    QVariant callMethod(const QString& method, const QVariantList&) override {
        ++calls;
        seen = logos::currentInboundCallerJson();
        if (method == QLatin1String("work")) return QStringLiteral("ok");
        if (method == QLatin1String("requestModule")) return QStringLiteral("ok");
        return QVariant();
    }
    bool informModuleToken(const QString& moduleName, const QString& token) override {
        if (m_store) m_store->saveToken(moduleName, token);
        ++informs;
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
    QString providerName() const override { return m_name; }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }

    int         calls   = 0;
    int         informs = 0;
    std::string seen;

private:
    QString       m_name;
    TokenManager* m_store = nullptr;
};

bool dispatched(const QVariant& r) {
    return !logos::isUnauthorizedSentinel(r) && r.toString() == QStringLiteral("ok");
}

// Every test names its own identities: the store registry is process-global and
// isolation is deliberately irreversible, so distinct names are what keeps the
// cases independent inside one binary.
QString cid(const char* suffix) {
    return QStringLiteral("cc_") + QString::fromLatin1(suffix);
}

// The host's own credential, written where a real host writes it: into
// instance(), under both bootstrap keys, before anything is isolated. This is
// the value an isolated store USED to be born holding.
QString seedHostAnchor(const char* value) {
    const QString anchor = QString::fromLatin1(value);
    TokenManager::instance().adoptCredential(anchor);
    return anchor;
}

// THE MECHANISM UNDER TEST, spelled out at the protocol layer so this file does
// not depend on logos-plugin-qt. logos::admitConsumer() is the Qt-side wrapper
// around exactly these three steps in exactly this order.
//
// REGISTER BEFORE ADOPT. The identity must never hold a credential
// capability_module has not yet accepted; doing it the other way round leaves a
// window in which the consumer's first call presents a token the trust root has
// never heard of, and the whole point of the synchronous registration both hosts
// already perform is to have no such window.
bool admit(const QString& identity, const QString& credential,
           ModuleProxy& capabilityProxy, const QString& hostAnchor)
{
    if (!TokenManager::isolateIdentity(identity)) return false;
    if (!capabilityProxy.informModuleToken(hostAnchor, identity, credential)) return false;
    return TokenManager::adoptCredentialFor(identity, credential);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// NO ANCHOR
// ─────────────────────────────────────────────────────────────────────────────

// The one-line statement of the bug. A private store must not come into
// existence holding a value the host holds.
//
// RED ON (M1): both EXPECT_EQ fail with the host anchor, and
// identitiesSharingHostAnchor() lists the identity.
TEST(ConsumerCredential, AnIsolatedIdentityHoldsNoValueOfTheHosts)
{
    ensureCredApp();
    const QString anchor = seedHostAnchor("cc-host-anchor-born-empty");

    const QString identity = cid("born_empty");
    ASSERT_TRUE(TokenManager::isolateIdentity(identity));
    TokenManager& store = TokenManager::forIdentity(identity);
    ASSERT_NE(&store, &TokenManager::instance());

    EXPECT_TRUE(store.getToken(QStringLiteral("core")).isEmpty());
    EXPECT_TRUE(store.getToken(QStringLiteral("capability_module")).isEmpty());
    EXPECT_EQ(store.tokenCount(), 0);

    // The control that makes the assertion above mean something: the anchor IS
    // reachable, it is sitting in instance() under both keys, and the private
    // store simply did not take it.
    ASSERT_EQ(TokenManager::instance().getToken(QStringLiteral("capability_module")), anchor);

    EXPECT_FALSE(TokenManager::identitiesSharingHostAnchor().contains(identity));
}

// The diagnostic a host's CI asserts on. It has to be able to SEE the bad state,
// or "it is empty" is not evidence of anything.
TEST(ConsumerCredential, TheHostAnchorDiagnosticSeesAHandCopiedAnchor)
{
    ensureCredApp();
    const QString anchor = seedHostAnchor("cc-host-anchor-diagnostic");

    const QString clean = cid("diag_clean");
    ASSERT_TRUE(TokenManager::isolateIdentity(clean));
    ASSERT_TRUE(TokenManager::adoptCredentialFor(clean, QStringLiteral("cc-own-cred-clean")));
    EXPECT_FALSE(TokenManager::identitiesSharingHostAnchor().contains(clean));

    // A host that copies the anchor by hand — the unsanctioned route the
    // library can no longer take for it — is still visible here.
    const QString dirty = cid("diag_dirty");
    ASSERT_TRUE(TokenManager::isolateIdentity(dirty));
    TokenManager::forIdentity(dirty).saveToken(QStringLiteral("capability_module"), anchor);
    EXPECT_TRUE(TokenManager::identitiesSharingHostAnchor().contains(dirty));
}

// The sanctioned API must not be a one-line way back to the bug.
TEST(ConsumerCredential, AdoptingTheHostsOwnAnchorIsRefused)
{
    ensureCredApp();
    const QString anchor = seedHostAnchor("cc-host-anchor-refused");

    const QString identity = cid("refuse_anchor");
    ASSERT_TRUE(TokenManager::isolateIdentity(identity));

    EXPECT_FALSE(TokenManager::adoptCredentialFor(identity, anchor));
    EXPECT_EQ(TokenManager::forIdentity(identity).tokenCount(), 0)
        << "a refusal must write nothing at all";

    // ... and the identity is still adoptable with a credential of its own.
    EXPECT_TRUE(TokenManager::adoptCredentialFor(identity, QStringLiteral("cc-own-cred")));
}

// THE NAMING HALF, at the layer that decides it. A consumer presenting its own
// credential is a MODULE named after itself, not the host.
//
// RED ON (M1): {"kind":"host"} — the isolated view wearing basecamp's authority.
TEST(ConsumerCredential, AnAdmittedConsumerIsNamedAsItselfAndNotAsTheHost)
{
    ensureCredApp();
    const QString anchor = seedHostAnchor("cc-host-anchor-naming");

    // capability_module's own proxy, authorizing against the HOST store, which
    // is where the host anchor lives. This is the real topology: capability is
    // a legacy in-process plugin on the host's identity.
    ProbeProvider capability(QStringLiteral("capability_module"), &TokenManager::instance());
    ModuleProxy capProxy(&capability, nullptr, &TokenManager::instance());

    const QString identity   = cid("named_view");
    const QString credential = QStringLiteral("cc-cred-named-view");
    ASSERT_TRUE(admit(identity, credential, capProxy, anchor));

    // The consumer presents whatever ITS OWN store says it presents to
    // capability_module — read here rather than passed in, because "what the
    // store hands the client" is exactly the thing under test.
    const QString presented =
        TokenManager::forIdentity(identity).getToken(QStringLiteral("capability_module"));
    EXPECT_EQ(presented, credential);

    ASSERT_TRUE(dispatched(capProxy.callRemoteMethod(presented, QStringLiteral("work"), {})));
    EXPECT_EQ(capability.seen, moduleDoc(identity));
    EXPECT_NE(capability.seen, kHostDoc);

    // The control, in the same test, so a green run cannot be a green run of
    // nothing: the host itself still authorizes AND still reads as the host.
    ASSERT_TRUE(dispatched(capProxy.callRemoteMethod(anchor, QStringLiteral("work"), {})));
    EXPECT_EQ(capability.seen, kHostDoc);
}

// An identity nobody credentialed must be able to do NOTHING. On the copying
// build it can do everything the host can, which is the escalation stated as an
// outcome rather than as a store contents.
//
// RED ON (M1): the call authorizes.
TEST(ConsumerCredential, AnIsolatedIdentityWithNoCredentialIsRefusedEverywhere)
{
    ensureCredApp();
    const QString anchor = seedHostAnchor("cc-host-anchor-uncredentialed");

    ProbeProvider capability(QStringLiteral("capability_module"), &TokenManager::instance());
    ModuleProxy capProxy(&capability, nullptr, &TokenManager::instance());

    const QString identity = cid("uncredentialed");
    ASSERT_TRUE(TokenManager::isolateIdentity(identity));
    TokenManager& store = TokenManager::forIdentity(identity);

    // Whatever the store hands it for capability_module is what it can present.
    const QString presented = store.getToken(QStringLiteral("capability_module"));
    EXPECT_TRUE(presented.isEmpty());
    EXPECT_TRUE(logos::isUnauthorizedSentinel(
        capProxy.callRemoteMethod(presented, QStringLiteral("work"), {})));
    EXPECT_EQ(capability.calls, 0);

    // The control: the host's own anchor still works, so this test is about the
    // identity and not about the proxy being broken.
    ASSERT_TRUE(dispatched(capProxy.callRemoteMethod(anchor, QStringLiteral("work"), {})));
}

// ─────────────────────────────────────────────────────────────────────────────
// NO LOCKOUT
// ─────────────────────────────────────────────────────────────────────────────

// THE WHOLE HANDSHAKE, end to end, in one process: the consumer authenticates
// requestModule at capability_module with its own credential, capability pushes
// the minted token to the target, and the consumer reaches the target with it.
// Every hop is a real ModuleProxy::authorize.
//
// RED ON (M2): the first callRemoteMethod returns the unauthorized sentinel —
// the consumer has no credential to present, so nothing downstream happens.
TEST(ConsumerCredential, AnAdmittedConsumerCompletesTheHandshakeAndReachesAnOrdinaryModule)
{
    ensureCredApp();
    const QString anchor = seedHostAnchor("cc-host-anchor-handshake");

    ProbeProvider capability(QStringLiteral("capability_module"), &TokenManager::instance());
    ModuleProxy capProxy(&capability, nullptr, &TokenManager::instance());

    // An ordinary target module, on its own identity and its own store, exactly
    // as an out-of-process module's own image would be.
    const QString targetName = cid("ordinary_target");
    ASSERT_TRUE(TokenManager::isolateIdentity(targetName));
    TokenManager& targetStore = TokenManager::forIdentity(targetName);
    const QString targetCredential = QStringLiteral("cc-cred-ordinary-target");
    ASSERT_TRUE(TokenManager::adoptCredentialFor(targetName, targetCredential));
    ProbeProvider target(targetName, &targetStore);
    ModuleProxy targetProxy(&target, nullptr, &targetStore);

    const QString identity   = cid("handshake_view");
    const QString credential = QStringLiteral("cc-cred-handshake-view");
    ASSERT_TRUE(admit(identity, credential, capProxy, anchor));

    // 1. The consumer authenticates requestModule with its own credential.
    const QString presented =
        TokenManager::forIdentity(identity).getToken(QStringLiteral("capability_module"));
    ASSERT_FALSE(presented.isEmpty()) << "the consumer has nothing to present";
    ASSERT_TRUE(dispatched(capProxy.callRemoteMethod(
        presented, QStringLiteral("requestModule"), QVariantList{identity, targetName})))
        << "requestModule refused: an admitted consumer is locked out";

    // 2. capability_module mints a per-(caller,target) token and pushes it to
    //    the target over the trusted channel, authenticating with the token the
    //    target itself accepts — getToken(targetName) in capability's store,
    //    which informModuleToken wrote when the target was admitted.
    const QString minted = QStringLiteral("cc-minted-view-to-target");
    ASSERT_TRUE(targetProxy.informModuleToken(targetCredential, identity, minted));

    // 3. The consumer reaches the target with it, and is NAMED there.
    ASSERT_TRUE(dispatched(targetProxy.callRemoteMethod(minted, QStringLiteral("work"), {})));
    EXPECT_EQ(target.seen, moduleDoc(identity));
}

// The inbound direction of the same rule. An isolated PROVIDER identity has to
// be tellable about its own callers, and the token capability_module presents
// when it pushes is `tokenManager->getToken(moduleName)` — that identity's own
// credential. This is the case the bootstrap copy was really protecting, and it
// survives the copy's removal because the credential is in the store.
//
// RED ON (M2): informModuleToken is refused — the store holds nothing for the
// trusted-channel gate to match.
TEST(ConsumerCredential, AnAdmittedProviderIdentityStillAcceptsAPushFromCapability)
{
    ensureCredApp();
    seedHostAnchor("cc-host-anchor-inbound");

    const QString identity   = cid("inbound_provider");
    const QString credential = QStringLiteral("cc-cred-inbound-provider");
    ASSERT_TRUE(TokenManager::isolateIdentity(identity));
    TokenManager& store = TokenManager::forIdentity(identity);
    ASSERT_TRUE(TokenManager::adoptCredentialFor(identity, credential));

    ProbeProvider provider(identity, &store);
    ModuleProxy proxy(&provider, nullptr, &store);

    const QString granted = QStringLiteral("cc-granted-inbound");
    ASSERT_TRUE(proxy.informModuleToken(credential, cid("some_caller"), granted))
        << "the provider identity cannot be told about any caller: it is deaf";
    EXPECT_TRUE(dispatched(proxy.callRemoteMethod(granted, QStringLiteral("work"), {})));
    EXPECT_EQ(provider.seen, moduleDoc(cid("some_caller")));

    // The host's anchor is NOT a key to this door any more, which is the other
    // half of "no anchor": an isolated provider trusts its own credential and
    // nothing else.
    EXPECT_FALSE(proxy.informModuleToken(
        TokenManager::instance().getToken(QStringLiteral("capability_module")),
        cid("impostor"), QStringLiteral("cc-impostor-token")));
}

// ─────────────────────────────────────────────────────────────────────────────
// Rotation — the ordering hazard the fix introduces, handled rather than left
// ─────────────────────────────────────────────────────────────────────────────

// A reload re-mints and re-registers, and ModuleProxy::saveToken overwrites
// m_tokens[name], so the PREVIOUS credential stops working at the target the
// moment the new one is registered. Under the old copying scheme that never
// mattered — the store presented a never-rotating anchor. Now it does: a store
// left holding the stale credential is a locked-out reload that looks live.
// resetIdentity therefore clears the credential too, and the caller adopts the
// new one.
TEST(ConsumerCredential, AReloadRotatesTheCredentialAndTheOldOneStopsWorking)
{
    ensureCredApp();
    const QString anchor = seedHostAnchor("cc-host-anchor-reload");

    ProbeProvider capability(QStringLiteral("capability_module"), &TokenManager::instance());
    ModuleProxy capProxy(&capability, nullptr, &TokenManager::instance());

    const QString identity = cid("reloaded_view");
    const QString first    = QStringLiteral("cc-cred-reload-first");
    ASSERT_TRUE(admit(identity, first, capProxy, anchor));
    ASSERT_TRUE(dispatched(capProxy.callRemoteMethod(first, QStringLiteral("work"), {})));

    // A per-target token minted for the PREVIOUS incarnation.
    TokenManager& store = TokenManager::forIdentity(identity);
    store.saveToken(cid("some_target"), QStringLiteral("cc-stale-per-target"));

    // The reload: mint, register, reset, adopt.
    const QString second = QStringLiteral("cc-cred-reload-second");
    ASSERT_TRUE(capProxy.informModuleToken(anchor, identity, second));
    ASSERT_TRUE(TokenManager::resetIdentity(identity));
    EXPECT_EQ(store.tokenCount(), 0) << "reset must drop the credential too";
    ASSERT_TRUE(TokenManager::adoptCredentialFor(identity, second));

    EXPECT_EQ(store.getToken(QStringLiteral("capability_module")), second);
    EXPECT_TRUE(store.getToken(cid("some_target")).isEmpty())
        << "a token minted for the previous incarnation must not survive";

    // The store object is the same one a client mid-flight holds by raw pointer.
    EXPECT_EQ(&TokenManager::forIdentity(identity), &store);

    ASSERT_TRUE(dispatched(capProxy.callRemoteMethod(second, QStringLiteral("work"), {})));
    EXPECT_TRUE(logos::isUnauthorizedSentinel(
        capProxy.callRemoteMethod(first, QStringLiteral("work"), {})))
        << "the superseded credential must stop working at the target";
}

// ─────────────────────────────────────────────────────────────────────────────
// The host that has NOT adopted the mechanism
// ─────────────────────────────────────────────────────────────────────────────

// The compatibility claim, tested rather than asserted in a comment: a host that
// never isolates anything is completely unaffected. forIdentity() returns
// instance() pointer-identically, the ambient ring still holds every token the
// host wrote, and calls authorize exactly as they did.
TEST(ConsumerCredential, AHostThatNeverIsolatesSeesNoChangeAtAll)
{
    ensureCredApp();
    const QString anchor = seedHostAnchor("cc-host-anchor-untouched");
    TokenManager::instance().saveToken(cid("legacy_module"), QStringLiteral("cc-legacy-root"));

    EXPECT_EQ(&TokenManager::forIdentity(cid("never_isolated_a")), &TokenManager::instance());
    EXPECT_EQ(&TokenManager::forIdentity(cid("never_isolated_b")), &TokenManager::instance());

    ProbeProvider provider(cid("legacy_module"), &TokenManager::instance());
    ModuleProxy proxy(&provider, nullptr, &TokenManager::instance());

    EXPECT_TRUE(dispatched(proxy.callRemoteMethod(anchor, QStringLiteral("work"), {})));
    EXPECT_EQ(provider.seen, kHostDoc);
    EXPECT_EQ(TokenManager::instance().getToken(cid("legacy_module")),
              QStringLiteral("cc-legacy-root"));
}
