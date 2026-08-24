// THE LAYOUT OF TokenManager IS A CROSS-PACKAGE ABI, AND THIS FILE MEASURES IT.
//
// WHY A TEST AND NOT JUST A COMMENT. A TokenManager is allocated by ONE image
// and mutated by ANOTHER. A module plugin statically links its own copy of this
// library — its own TokenManager::saveToken, its own TokenManager::getToken —
// and runs them on the object the HOST image constructed, reached through
// LogosAPI::getTokenManager(). logos-plugin-qt's
// LogosProviderBase::informModuleToken and logos-qt-sdk's LpBridge::syncFromApi
// are both plugin CODE operating on a host OBJECT. The host ships as one
// package and each module ships as its own .lgx, installed independently, so
// the two sides are routinely built months apart.
//
// So the member offsets a plugin uses are the offsets ITS header had. An
// earlier revision of token_manager.h split the store into three members on the
// argument that "no consumer ever allocates a TokenManager and none needs
// sizeof()". That argument is about ALLOCATION; the hazard is MUTATION. Adding
// two members moved m_mutex from +24 to +56 and the version-mix matrix measured
// the result on shipped artifacts:
//
//   * OLD host + NEW module: TokenManager::saveInboundToken compare-exchanges
//     at this+56 on a 32-byte object — past the end, into adjacent BSS. In the
//     measured host that word was boost::asio's openssl_init guard, whose value
//     after static init is 1, which is exactly Qt's dummyLocked() sentinel; the
//     fast path can therefore never win and QBasicMutex::lockInternal()
//     futex-waits forever. The module's host process deadlocked on the FIRST
//     inbound token push and never served another call, while the daemon still
//     reported it "loaded", "crashed": 0.
//   * NEW host + OLD module: the old code CASes this+24, which post-split is
//     m_inbound's QHash d-pointer. Survivable only while that hash is empty; a
//     poke setting it non-null reproduced the same permanent hang.
//   * The ui seam, both ways: LpBridge::syncFromApi -> getToken hung before the
//     plugin ever reached READY.
//
// Nothing in band could see any of it. evaluateProtocolGate compares MAJOR only
// and MAJOR is 0 on both sides; logos_module_get_protocol_version() is exported
// by every module and called by nobody; and both layouts answered "0.7.0".
//
// HOW EACH TEST HERE IS A DETECTOR. Every assertion below is RED on the
// three-member tree (logos-protocol e514c53) and green on this one:
//
//   TheObjectIsTheSizeEveryShippedModuleWasCompiledAgainst
//       sizeof 64 vs 32 — the direct measurement of the break.
//   TheTokenMapIsAtTheOffsetEveryShippedModuleReadsItFrom
//       measured on a live object rather than asserted from the header, so a
//       reorder that preserves sizeof is caught too.
//   BothDirectionsLiveInTheSameMember
//       the positive form of "no member was added": an inbound write must move
//       the SAME word an outbound write moves.
//
// The remaining tests are about the encoding that makes the freeze possible —
// direction as a KEY NAMESPACE — and specifically about the one thing a key
// namespace can get wrong that a separate member cannot: a wire-supplied name
// spelled to land in the other half.

#include <gtest/gtest.h>

#include "token_manager.h"

#include <QCoreApplication>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

namespace {

QCoreApplication* ensureAbiApp()
{
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance())
        new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

// The layout master shipped, spelled out. This is not a guess at what the class
// contains: it is the contract every module .lgx in the field was compiled
// against, and the thing token_manager.cpp's static_assert pins.
struct AbiReference : QObject {
    QHash<QString, QString> tokens;
    mutable QMutex mutex;
};

// Which pointer-sized words of `obj` went from all-zero to non-zero across
// `mutate`. Reading the object representation through unsigned char* is the
// only way to ask this question from outside the class, and it is the right
// question: the header's own claim about where its members sit is exactly what
// is under test, so an offsetof() on the header under test would be circular.
std::vector<std::ptrdiff_t> wordsThatBecameNonZero(const void* obj, std::size_t size,
                                                   const std::function<void()>& mutate)
{
    const auto* bytes = static_cast<const unsigned char*>(obj);
    const std::size_t words = size / sizeof(void*);

    std::vector<std::uintptr_t> before(words);
    std::memcpy(before.data(), bytes, words * sizeof(void*));

    mutate();

    std::vector<std::uintptr_t> after(words);
    std::memcpy(after.data(), bytes, words * sizeof(void*));

    std::vector<std::ptrdiff_t> moved;
    for (std::size_t i = 0; i < words; ++i)
        if (before[i] == 0 && after[i] != 0)
            moved.push_back(static_cast<std::ptrdiff_t>(i * sizeof(void*)));
    return moved;
}

// A store nobody else in this binary has touched. instance() accumulates tokens
// from every other suite, and an already-populated QHash has a non-null d
// pointer, which is precisely the 0 -> non-zero transition the probe looks for.
TokenManager& freshStore(const char* name)
{
    const QString identity = QString::fromLatin1(name);
    EXPECT_TRUE(TokenManager::isolateIdentity(identity))
        << "the probe needs a private store; this name must not have been vended shared";
    TokenManager& store = TokenManager::forIdentity(identity);
    EXPECT_NE(&store, &TokenManager::instance());
    return store;
}

// The literal an attacker would have to spell to forge an inbound key. Written
// out here rather than obtained from TokenManager (which keeps it private, and
// rightly so) — this file is the OUTSIDE, which is where the forgery would come
// from. U+0001 START OF HEADING.
const QString kForgedInboundPrefix = QString(QChar(u'\u0001'))
                                   + QStringLiteral("in")
                                   + QString(QChar(u'\u0001'));

class TokenManagerAbi : public ::testing::Test {
protected:
    void SetUp() override { ensureAbiApp(); }
};

} // namespace

// ── the freeze ──────────────────────────────────────────────────────────────

TEST_F(TokenManagerAbi, TheObjectIsTheSizeEveryShippedModuleWasCompiledAgainst)
{
    EXPECT_EQ(sizeof(TokenManager), sizeof(AbiReference))
        << "TokenManager grew a member. It is allocated by the host image and "
           "mutated by module images built against other revisions of this "
           "header, so a member added here moves m_mutex and deadlocks them — "
           "measured, on shipped artifacts. Encode new state in the key "
           "namespace instead.";

    // Stated separately so the failure says WHICH of the two facts broke: the
    // absolute shape (QObject + two words) and the reference's agreement with
    // it are different claims.
    EXPECT_EQ(sizeof(AbiReference), sizeof(QObject) + 2 * sizeof(void*));
}

TEST_F(TokenManagerAbi, TheTokenMapIsAtTheOffsetEveryShippedModuleReadsItFrom)
{
    TokenManager& store = freshStore("abi_probe_outbound");

    const auto moved = wordsThatBecameNonZero(
        &store, sizeof(TokenManager),
        [&] { store.saveToken(QStringLiteral("peer"), QStringLiteral("tok")); });

    ASSERT_EQ(moved.size(), 1u)
        << "exactly one word should have gone from null to non-null: the token "
           "map's d pointer";
    EXPECT_EQ(moved.front(), static_cast<std::ptrdiff_t>(sizeof(QObject)))
        << "the token map moved off the offset every shipped module reads it "
           "from; the mutex moved with it";

    // With sizeof pinned above and the map measured here, the mutex has exactly
    // one place left to be — which is why this file does not need to reach into
    // a private member to pin it.
    EXPECT_EQ(sizeof(TokenManager), sizeof(QObject) + 2 * sizeof(void*));
}

TEST_F(TokenManagerAbi, BothDirectionsLiveInTheSameMember)
{
    TokenManager& store = freshStore("abi_probe_inbound");

    const auto moved = wordsThatBecameNonZero(
        &store, sizeof(TokenManager),
        [&] { store.saveInboundToken(QStringLiteral("caller"), QStringLiteral("tok")); });

    ASSERT_EQ(moved.size(), 1u)
        << "an inbound write must touch exactly one word — the same map the "
           "outbound write touches. Two means a second member exists.";
    EXPECT_EQ(moved.front(), static_cast<std::ptrdiff_t>(sizeof(QObject)))
        << "the inbound half is stored somewhere other than the one token map, "
           "which is an added member however it is spelled";
}

// ── the encoding, and the one way a key namespace can be attacked ───────────

TEST_F(TokenManagerAbi, TheOutboundDoorRefusesAKeyThatForgesTheInboundNamespace)
{
    TokenManager& store = freshStore("abi_probe_forge_out");

    // The name reaches saveToken from the wire: capability_module names the peer
    // in informModuleToken, and lp_token_save passes through whatever the C ABI
    // was handed. Without the refusal this is a one-line way to write an
    // OUTBOUND value into an INBOUND slot — the pre-split collision, forged by
    // hand rather than caused by a shared key.
    store.saveToken(kForgedInboundPrefix + QStringLiteral("victim"),
                    QStringLiteral("forged"));

    EXPECT_EQ(store.inbound().token(QStringLiteral("victim")), QString())
        << "a forged key authorized a caller through the outbound door";
    EXPECT_FALSE(store.inbound().contains(QStringLiteral("victim")));
    EXPECT_EQ(store.inbound().count(), 0);

    // And it was not quietly filed as an ordinary outbound entry either: a
    // refusal that stores the value anywhere leaves it in the roster.
    EXPECT_EQ(store.tokenCount(), 0);
    EXPECT_TRUE(store.getTokenKeys().isEmpty());
}

TEST_F(TokenManagerAbi, TheInboundDoorRefusesACallerNameThatCarriesTheNamespace)
{
    TokenManager& store = freshStore("abi_probe_forge_in");

    EXPECT_FALSE(store.saveInboundToken(kForgedInboundPrefix + QStringLiteral("x"),
                                        QStringLiteral("t")))
        << "a caller name is supplied by capability_module over RPC; it must "
           "not be able to address any key but its own";
    EXPECT_EQ(store.inbound().count(), 0);
    EXPECT_EQ(store.tokenCount(), 0);
}

TEST_F(TokenManagerAbi, TheOutboundRosterNeverPublishesInboundKeys)
{
    TokenManager& store = freshStore("abi_probe_roster");

    store.saveToken(QStringLiteral("callee"), QStringLiteral("out-tok"));
    ASSERT_TRUE(store.saveInboundToken(QStringLiteral("caller"), QStringLiteral("in-tok")));

    // getTokenKeys() is the roster lp_token_keys() publishes to a granted token
    // registry, and capability_module treats every entry as "a module I may
    // call". Leaking the inbound half into it would publish everyone who may
    // call US as if we held their credential.
    EXPECT_EQ(store.getTokenKeys(), QList<QString>{ QStringLiteral("callee") });
    EXPECT_EQ(store.tokenCount(), 1);
    EXPECT_EQ(store.getTokenKeysStd().size(), 1u);
    EXPECT_EQ(store.getTokenKeysStd().front(), std::string("callee"));

    // ...and the inbound view is the mirror image of that claim.
    EXPECT_EQ(store.inbound().keys(), QStringList{ QStringLiteral("caller") });
    EXPECT_EQ(store.inbound().count(), 1);

    // Neither half answers for the other, at the door as well as in the roster.
    EXPECT_EQ(store.getToken(QStringLiteral("caller")), QString());
    EXPECT_FALSE(store.hasToken(QStringLiteral("caller")));
    EXPECT_EQ(store.inbound().token(QStringLiteral("callee")), QString());
}

TEST_F(TokenManagerAbi, AnInboundEntryIsNotReadableOrRemovableThroughTheOutboundDoor)
{
    TokenManager& store = freshStore("abi_probe_reserved_read");

    ASSERT_TRUE(store.saveInboundToken(QStringLiteral("caller"), QStringLiteral("in-tok")));

    const QString forged = kForgedInboundPrefix + QStringLiteral("caller");
    EXPECT_EQ(store.getToken(forged), QString())
        << "getToken(inboundKey(x)) would hand a module a token it ISSUED, to "
           "present as if it were its own";
    EXPECT_FALSE(store.hasToken(forged));
    EXPECT_FALSE(store.removeToken(forged));

    // The refusal is a refusal, not a deletion.
    EXPECT_EQ(store.inbound().token(QStringLiteral("caller")), QStringLiteral("in-tok"));
}

// ── the credential, which is DERIVED and therefore mixed-package correct ────

TEST_F(TokenManagerAbi, TheCredentialIsWhateverIsUnderTheBootstrapKeys)
{
    TokenManager& store = freshStore("abi_probe_credential");

    EXPECT_EQ(store.credential(), QString());

    // This is what EVERY anchor writer in the fleet does — module_initializer,
    // seedHandshakeTrustAnchor, the generated glue's
    // logos_module_accept_token("core"), LpBridge::syncFromApi, ui-host's
    // main(). Reading the credential back out of the map rather than out of a
    // cached member is what makes it answer correctly on a store some OTHER
    // image wrote, which is the whole mixed-package case.
    for (const QString& key : TokenManager::bootstrapKeys())
        store.saveToken(key, QStringLiteral("anchor-v1"));
    EXPECT_EQ(store.credential(), QStringLiteral("anchor-v1"));

    // Dropping one of the two role labels must not strand the credential on a
    // value the store no longer holds. LogosAPIClient removes a rejected
    // per-target token on the re-exchange path, so this is reachable from
    // ordinary traffic rather than only from teardown.
    ASSERT_TRUE(store.removeToken(TokenManager::bootstrapKeys().first()));
    EXPECT_EQ(store.credential(), QStringLiteral("anchor-v1"));

    for (const QString& key : TokenManager::bootstrapKeys())
        store.removeToken(key);
    EXPECT_EQ(store.credential(), QString());
}

TEST_F(TokenManagerAbi, ClearingTakesBothNamespacesAndTheCredential)
{
    TokenManager& store = freshStore("abi_probe_clear");

    store.saveToken(QStringLiteral("callee"), QStringLiteral("out"));
    ASSERT_TRUE(store.saveInboundToken(QStringLiteral("caller"), QStringLiteral("in")));
    store.adoptCredential(QStringLiteral("anchor"));
    ASSERT_EQ(store.credential(), QStringLiteral("anchor"));

    store.clearAllTokens();

    EXPECT_EQ(store.tokenCount(), 0);
    EXPECT_EQ(store.inbound().count(), 0);
    EXPECT_EQ(store.credential(), QString())
        << "resetIdentity() clears a store on reload precisely so a stale "
           "credential cannot make a locked-out reload look like a live one";
}
