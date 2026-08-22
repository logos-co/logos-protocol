// WHO IS CALLING — the host half.
//
// ModuleProxy has always known the answer and always thrown it away. Deciding
// that an inbound call is authorized IS finding which issued token matched, and
// an issued token in m_tokens is filed under the name of the caller it was
// issued to. This file pins what that answer is allowed to say, and — more of
// the work than it looks — what it must REFUSE to say.
//
// WHAT IS NOT HERE, so nobody reads a green run as more than it is. This is the
// production of the caller DOCUMENT and its lifetime on the dispatching thread.
// It is not the delivery of that document into a module cdylib: that needs the
// host-side invokable (logos-plugin-qt), the pull in the generated glue, and
// logos_module_set_call_caller defined by each language backend, none of which
// exist in this repo. A module built today still reads Unknown, correctly,
// because nothing pushes yet.
//
// THE THREE REFUSALS, which are the reason the type has an Unknown arm at all:
//
//   * A hit in TokenManager under a NON-anchor key names nobody. That store is
//     direction-MIXED — LogosAPIClient files the token it will PRESENT to a
//     callee under the CALLEE's name (logos_api_client.cpp:176) — so a reverse
//     lookup there can name a module we CALL as the module CALLING us. Naming
//     it would be affirmatively wrong, which is worse than declining.
//   * A hit on an anchor key names the HOST and carries no module name.
//     TokenManager::bootstrapKeys() is "core" and "capability_module" holding
//     one host secret under two keys, so any name from that arm is a coin flip.
//   * Two callers holding one token value name neither. Impossible with UUIDs;
//     if it ever happens we do not get to pick.
//
// HOW THIS WAS VALIDATED — five builds, each missing one mechanism, each run.
// Throwaway local edits, made and thrown away, as the note at the top of
// tests/protocol/CMakeLists.txt prescribes; not a build flag and not a switch in
// this tree. Each run is also recorded above the test it made red.
//
//   (a) NO SCOPE — the CallerScope construction removed from callRemoteMethod.
//       2 passed, 9 FAILED. Every dispatch sees "", including the cases that
//       want Unknown, because an unopened scope is not the same fact as a
//       scope that could not name anyone.
//   (b) NO RESOLUTION — the scope opened, but authorize() leaving the document
//       at the Unknown it initialises. 6 passed, 5 FAILED. The four
//       Unknown-expecting cases survive, which is what makes (a) and (b)
//       distinguishable rather than two spellings of one detector.
//   (c) THE MIXED STORE NAMING PEOPLE — a second fold.offer() in the m_store
//       loop. 10 passed, 1 FAILED (the direction-purity case).
//   (d) NO ANCHOR ARM. 9 passed, 2 FAILED — and the second failure is the
//       interesting one: the tie case reports
//       {"kind":"module","name":"impostor_module"} for a token that is
//       demonstrably the host's.
//   (e) CLEAR INSTEAD OF RESTORE in ~CallerScope. 10 passed, 1 FAILED (the
//       nesting case).
//
// TheProducedDocumentsMatchTheDeclaredWireShape is a PIN, green on all five. Do
// not read it as evidence of anything but the JSON not having been reworded.

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
#include <QVariantList>

#include <string>

namespace {

QCoreApplication* ensureCallerApp() {
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance())
        new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

// The documents, spelled once here so a test asserts against the wire shape in
// logos_module_impl.h rather than against logos_caller_scope.cpp's opinion of
// it. A change to either that does not change the other fails these.
const char* kUnknown = R"({"kind":"unknown"})";
const char* kHost    = R"({"kind":"host"})";
std::string moduleDoc(const char* name) {
    return std::string(R"({"kind":"module","name":")") + name + R"("})";
}

// Reads the ambient caller AT DISPATCH TIME, which is the only time it exists.
// Optionally makes a second, nested inbound call from inside the first — the
// shape a handler that calls another module produces, where QtRO can deliver an
// inbound call on the same thread inside the nested event loop.
class CallerProbeProvider : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList&) override {
        ++calls;
        seen = logos::currentInboundCallerJson();
        if (!nestedToken.isEmpty() && nestedProxy) {
            const QString token = nestedToken;
            nestedToken.clear();                       // once, not forever
            nestedProxy->callRemoteMethod(token, QStringLiteral("work"), {});
            afterNested = logos::currentInboundCallerJson();
        }
        if (method == QLatin1String("work")) return QStringLiteral("ok");
        return QVariant();
    }
    // Deliberately writes to NO store. m_tokens is then the only place the
    // token can be, which is what makes "the inbound record is the oracle" an
    // assertion rather than a coincidence.
    bool informModuleToken(const QString&, const QString&) override { return true; }
    QJsonArray getMethods() override {
        QJsonObject work;
        work["name"] = QStringLiteral("work");
        work["type"] = QStringLiteral("method");
        return QJsonArray{ work };
    }
    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("probe_module"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }

    int         calls = 0;
    std::string seen;        // the caller the dispatch saw
    std::string afterNested; // ... and what it saw again after a nested call

    ModuleProxy* nestedProxy = nullptr;
    QString      nestedToken;
};

// A store that is genuinely NOT instance(), so the m_store side of the scan has
// exactly the keys this test put there. forIdentity() is additive by design and
// hands back instance() until a name is isolated, so isolate first.
TokenManager& privateStore(const QString& identity) {
    EXPECT_TRUE(TokenManager::isolateIdentity(identity));
    TokenManager& s = TokenManager::forIdentity(identity);
    EXPECT_NE(&s, &TokenManager::instance());
    return s;
}

// The host anchor, written straight into the proxy's own store. Not seeded via
// instance(): a private store copies the bootstrap keys when it is CREATED, so
// a later write to instance() would never reach it.
QString seedAnchor(TokenManager& store, const char* value) {
    const QString anchor = QString::fromLatin1(value);
    store.saveToken(QStringLiteral("core"), anchor);
    return anchor;
}

bool dispatched(const QVariant& r) {
    return !logos::isUnauthorizedSentinel(r) && r.toString() == QStringLiteral("ok");
}

} // namespace

// ── 1. a caller's own inbound token names that caller ────────────────────────
//
// The whole point, stated once. The token was recorded under "chat_module" by
// informModuleToken, so presenting it identifies chat_module.
//
// RED BEFORE: with the CallerScope construction removed from callRemoteMethod,
// `seen` is "" and this fails on the first EXPECT_EQ. With the scope kept but
// authorize() resolving nothing (the `if (callerJson)` block reduced to the
// Unknown it initialises), `seen` is {"kind":"unknown"} and it fails the same
// assertion. Two mechanisms, one detector each.
TEST(CallCaller, AnInboundTokenNamesTheCallerItWasIssuedTo)
{
    ensureCallerApp();
    TokenManager& store = privateStore(QStringLiteral("caller_names_issuer"));
    const QString anchor = seedAnchor(store, "caller-test-anchor-1");

    CallerProbeProvider provider;
    ModuleProxy proxy(&provider, nullptr, &store);

    const QString granted = QStringLiteral("caller-token-chat");
    ASSERT_TRUE(proxy.informModuleToken(anchor, QStringLiteral("chat_module"), granted));

    ASSERT_TRUE(dispatched(proxy.callRemoteMethod(granted, QStringLiteral("work"), {})));
    EXPECT_EQ(provider.seen, moduleDoc("chat_module"));
}

// ── 2. the host anchor names the host, and no module ─────────────────────────
//
// "core" and "capability_module" are one secret under two keys
// (TokenManager::bootstrapKeys), so the arm carries no name BY CONSTRUCTION.
// This is a detector for the anchor arm existing at all: without the anchorHits
// branch the call still authorizes (the anchor is in m_store) and resolves to
// Unknown — measured, {"kind":"unknown"} against the {"kind":"host"} wanted.
TEST(CallCaller, TheHostAnchorNamesTheHostAndCarriesNoName)
{
    ensureCallerApp();
    TokenManager& store = privateStore(QStringLiteral("caller_anchor_names_host"));
    const QString anchor = seedAnchor(store, "caller-test-anchor-2");

    CallerProbeProvider provider;
    ModuleProxy proxy(&provider, nullptr, &store);

    ASSERT_TRUE(dispatched(proxy.callRemoteMethod(anchor, QStringLiteral("work"), {})));
    EXPECT_EQ(provider.seen, kHost);
    // Stated as its own assertion because "no name" is the property that must
    // not be helpfully improved later: a reader must never be able to extract
    // "core" from this document.
    EXPECT_EQ(provider.seen.find("name"), std::string::npos);
}

// ── 3. the direction-mixed store names NOBODY ────────────────────────────────
//
// THE REFUSAL THAT MATTERS MOST, and the one an "obvious simplification" would
// delete. "some_callee" here is exactly the shape LogosAPIClient writes: a token
// this module holds in order to CALL some_callee, filed under some_callee's
// name. Reverse-looking-up that store would report some_callee as our CALLER.
//
// RED BEFORE: with a second fold.offer() added to the m_store loop — the
// one-line "why not name it from there too" — this reports
// {"kind":"module","name":"some_callee"} and fails.
TEST(CallCaller, ATokenFoundOnlyInTheDirectionMixedStoreNamesNobody)
{
    ensureCallerApp();
    TokenManager& store = privateStore(QStringLiteral("caller_mixed_store_no_name"));
    seedAnchor(store, "caller-test-anchor-3");

    // An OUTBOUND token: ours to present to some_callee, filed under its name.
    const QString outbound = QStringLiteral("caller-token-outbound");
    store.saveToken(QStringLiteral("some_callee"), outbound);

    CallerProbeProvider provider;
    ModuleProxy proxy(&provider, nullptr, &store);

    // It still AUTHORIZES — that is pre-existing behaviour and this change does
    // not touch it. It simply cannot NAME anyone.
    ASSERT_TRUE(dispatched(proxy.callRemoteMethod(outbound, QStringLiteral("work"), {})));
    EXPECT_EQ(provider.seen, kUnknown);
}

// ── 4. an operator token names nobody ────────────────────────────────────────
//
// A host-installed TokenValidator returns bool and nothing else, so there is no
// name to be had. Pinned rather than left implicit because this is the MAIN
// plain-transport case: operator-issued named tokens authorize here, and every
// one of them reads Unknown until TokenValidator is widened.
TEST(CallCaller, AValidatorAcceptedTokenNamesNobody)
{
    ensureCallerApp();
    TokenManager& store = privateStore(QStringLiteral("caller_validator_no_name"));
    seedAnchor(store, "caller-test-anchor-4");

    CallerProbeProvider provider;
    ModuleProxy proxy(&provider, nullptr, &store);
    proxy.setTokenValidator([](const QString& t, const QString&) {
        return t == QStringLiteral("operator-issued-token");
    });

    ASSERT_TRUE(dispatched(proxy.callRemoteMethod(
        QStringLiteral("operator-issued-token"), QStringLiteral("work"), {})));
    EXPECT_EQ(provider.seen, kUnknown);
}

// ── 5. two callers holding one token value name neither ──────────────────────
//
// Unreachable with UUID tokens. It is here because the fold's answer has to be
// wrong-proof rather than merely usually-right: if two keys ever hold one value
// the scan cannot tell which caller presented it, and picking the last one the
// QHash happened to yield would be a guess dressed as an identity.
TEST(CallCaller, TwoCallersSharingATokenValueNameNeither)
{
    ensureCallerApp();
    TokenManager& store = privateStore(QStringLiteral("caller_ambiguous_names_none"));
    const QString anchor = seedAnchor(store, "caller-test-anchor-5");

    CallerProbeProvider provider;
    ModuleProxy proxy(&provider, nullptr, &store);

    const QString shared = QStringLiteral("caller-token-shared");
    ASSERT_TRUE(proxy.informModuleToken(anchor, QStringLiteral("caller_one"), shared));
    ASSERT_TRUE(proxy.informModuleToken(anchor, QStringLiteral("caller_two"), shared));

    ASSERT_TRUE(dispatched(proxy.callRemoteMethod(shared, QStringLiteral("work"), {})));
    EXPECT_EQ(provider.seen, kUnknown);
}

// ── 6. the anchor wins a tie ─────────────────────────────────────────────────
//
// A value that is BOTH the host anchor and some caller's inbound token. The
// anchor's own ambiguity forbids naming, so the anchor arm has to win; the
// alternative is asserting a module identity for a value that demonstrably
// belongs to the host too. Pins the ORDER of the two branches, which is
// otherwise an invisible choice.
//
// RED BEFORE: with the anchor arm removed this reports
// {"kind":"module","name":"impostor_module"} — a module identity asserted for
// the host's own token, which is the failure mode worth having a test for
// rather than the missing-arm one next door.
TEST(CallCaller, TheHostAnchorWinsATieAgainstAnInboundKey)
{
    ensureCallerApp();
    TokenManager& store = privateStore(QStringLiteral("caller_anchor_wins_tie"));
    const QString anchor = seedAnchor(store, "caller-test-anchor-6");

    CallerProbeProvider provider;
    ModuleProxy proxy(&provider, nullptr, &store);

    ASSERT_TRUE(proxy.informModuleToken(anchor, QStringLiteral("impostor_module"), anchor));

    ASSERT_TRUE(dispatched(proxy.callRemoteMethod(anchor, QStringLiteral("work"), {})));
    EXPECT_EQ(provider.seen, kHost);
}

// ── 7. an overlong key authorizes and names nobody ───────────────────────────
//
// kCallerKeyMax is the width at which recovering a name stops being free, not a
// limit on module names. The two halves of that sentence are both assertions:
// the call must still go through, and the answer must be Unknown rather than a
// truncated name — a 64-byte prefix of a longer name is a DIFFERENT module's
// name as far as any consumer is concerned.
TEST(CallCaller, AnOverlongCallerKeyStillAuthorizesAndNamesNobody)
{
    ensureCallerApp();
    TokenManager& store = privateStore(QStringLiteral("caller_overlong_key"));
    const QString anchor = seedAnchor(store, "caller-test-anchor-7");

    CallerProbeProvider provider;
    ModuleProxy proxy(&provider, nullptr, &store);

    const QString longName = QString(65, QLatin1Char('m'));   // > kCallerKeyMax
    const QString granted  = QStringLiteral("caller-token-longkey");
    ASSERT_TRUE(proxy.informModuleToken(anchor, longName, granted));

    ASSERT_TRUE(dispatched(proxy.callRemoteMethod(granted, QStringLiteral("work"), {})));
    EXPECT_EQ(provider.seen, kUnknown);
    EXPECT_EQ(provider.seen.find("mmmm"), std::string::npos);
}

// ── 8. outside a dispatch there is no caller ─────────────────────────────────
//
// The sharp edge, asserted rather than only documented. Empty is a third state,
// distinct from {"kind":"unknown"}: "no dispatch on this thread" versus "a
// dispatch whose caller could not be named". A background thread, a timer and
// onContextReady all land here.
//
// The second half is the one that would rot: the scope must POP. A leaked value
// is a caller identity attributed to whatever runs next on this thread, which
// is the worst failure this design can have.
TEST(CallCaller, ThereIsNoCallerOutsideADispatchAndTheScopePops)
{
    ensureCallerApp();
    EXPECT_TRUE(logos::currentInboundCallerJson().empty());

    TokenManager& store = privateStore(QStringLiteral("caller_scope_pops"));
    const QString anchor = seedAnchor(store, "caller-test-anchor-8");

    CallerProbeProvider provider;
    ModuleProxy proxy(&provider, nullptr, &store);

    const QString granted = QStringLiteral("caller-token-pop");
    ASSERT_TRUE(proxy.informModuleToken(anchor, QStringLiteral("popper_module"), granted));
    ASSERT_TRUE(dispatched(proxy.callRemoteMethod(granted, QStringLiteral("work"), {})));
    ASSERT_EQ(provider.seen, moduleDoc("popper_module"));

    EXPECT_TRUE(logos::currentInboundCallerJson().empty());
}

// ── 9. a nested inbound call restores the outer caller ───────────────────────
//
// THE DETECTOR FOR SAVE-AND-RESTORE, and the reason CallerScope is not a
// set/clear pair. A handler that calls another module spins a nested event
// loop, and QtRO can deliver a second inbound call on the same thread inside
// it. With a clear-on-exit scope the inner dispatch's exit wipes the outer
// one's caller, and the outer handler's identity evaporates mid-frame with
// nothing to see.
//
// RED BEFORE: with CallerScope's destructor changed to `slot().clear()` —
// which is what "clear it when the dispatch ends" looks like when written
// naturally — afterNested is "" against the outer module document wanted.
TEST(CallCaller, ANestedInboundCallRestoresTheOuterCaller)
{
    ensureCallerApp();
    TokenManager& store = privateStore(QStringLiteral("caller_nested_restore"));
    const QString anchor = seedAnchor(store, "caller-test-anchor-9");

    CallerProbeProvider provider;
    ModuleProxy proxy(&provider, nullptr, &store);

    const QString outerToken = QStringLiteral("caller-token-outer");
    const QString innerToken = QStringLiteral("caller-token-inner");
    ASSERT_TRUE(proxy.informModuleToken(anchor, QStringLiteral("outer_module"), outerToken));
    ASSERT_TRUE(proxy.informModuleToken(anchor, QStringLiteral("inner_module"), innerToken));

    provider.nestedProxy = &proxy;
    provider.nestedToken = innerToken;

    ASSERT_TRUE(dispatched(proxy.callRemoteMethod(outerToken, QStringLiteral("work"), {})));
    ASSERT_EQ(provider.calls, 2);

    // `seen` was overwritten by the INNER dispatch, which is itself the proof
    // that the inner frame got its own caller rather than inheriting the outer.
    EXPECT_EQ(provider.seen, moduleDoc("inner_module"));
    // And the outer frame got its own back.
    EXPECT_EQ(provider.afterNested, moduleDoc("outer_module"));
    // Both frames are gone.
    EXPECT_TRUE(logos::currentInboundCallerJson().empty());
}

// ── 10. a rejected call opens no scope at all ────────────────────────────────
//
// An unauthorized call has no caller because it has no dispatch. Pins that the
// rejection path is still the rejection path — the scope is constructed after
// the gate, not before it — so nothing can read a caller for a call that never
// ran.
TEST(CallCaller, AnUnauthorizedCallOpensNoScope)
{
    ensureCallerApp();
    TokenManager& store = privateStore(QStringLiteral("caller_rejected_no_scope"));
    seedAnchor(store, "caller-test-anchor-10");

    CallerProbeProvider provider;
    ModuleProxy proxy(&provider, nullptr, &store);

    const QVariant r = proxy.callRemoteMethod(QStringLiteral("never-issued"),
                                              QStringLiteral("work"), {});
    EXPECT_TRUE(logos::isUnauthorizedSentinel(r));
    EXPECT_EQ(provider.calls, 0);
    EXPECT_TRUE(logos::currentInboundCallerJson().empty());
}

// ── 11. the documents are the documents ──────────────────────────────────────
//
// The producers, asserted directly against the shapes logos_module_impl.h
// specifies. Cheap, and it is what stops a "tidy up the JSON" commit from
// changing a wire format that two language backends parse.
//
// The empty-name case is rule 4 applied at the PRODUCER: a known arm missing a
// required field is Unknown, so callerModuleJson("") must not mint
// {"kind":"module","name":""} for a reader to have to reject later.
TEST(CallCaller, TheProducedDocumentsMatchTheDeclaredWireShape)
{
    EXPECT_EQ(logos::callerUnknownJson(), kUnknown);
    EXPECT_EQ(logos::callerHostAnchorJson(), kHost);
    EXPECT_EQ(logos::callerModuleJson("chat_module"), moduleDoc("chat_module"));
    EXPECT_EQ(logos::callerModuleJson(std::string()), kUnknown);
    // Escaping goes through a real JSON writer rather than string concatenation.
    EXPECT_EQ(logos::callerModuleJson("a\"b"), R"({"kind":"module","name":"a\"b"})");
}
