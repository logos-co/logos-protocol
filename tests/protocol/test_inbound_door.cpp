// THE INBOUND DOOR ACROSS THE C ABI — lp_token_save_inbound, protocol 0.8.
//
// WHAT IT IS FOR. The generated Qt glue's informModuleToken used to write the
// SAME value through two doors:
//
//     LogosProviderBase::informModuleToken(moduleName, token);   // HOST image
//     logos_module_accept_token(moduleName, token);              // THIS image
//
// The value is a CALLER's token — capability_module saying "moduleName may call
// you" — and the second door forwarded to lp_token_save, the OUTBOUND family.
// So a cdylib filed every caller's token as a credential to PRESENT BACK to
// that caller. That is the direction collision logos-protocol's store split
// removed from the host image, reconstituted one image deeper, and it was
// measured end to end on shipped artifacts: after capability_module minted
// <A -> B> and pushed it to B, B's own LogosAPIClient found the value under "A",
// logged "Found token", SKIPPED requestModule, presented it to A, was rejected
// ("auth token not recognized"), and re-exchanged. Every call of every two-way
// pair paid a rejection plus a full extra round trip, permanently, and the
// caller saw only success.
//
// HOW THESE ARE DETECTORS. Three neutered builds of lp_token_save_inbound were
// run against this file; the counts below are measured, not predicted, and the
// restored control is 7/7 green.
//
//   A. point it at TokenManager::saveToken — i.e. spell it the way the glue
//      used to. 6 of 7 RED. This is the bug, exactly:
//        AnInboundPushIsNotAnOutboundCredential
//            "Which is: 0x955010 / Which is: (nullptr)" — lp_token_get answered
//        AnInboundPushDoesNotClobberAnExistingOutboundCache
//            "T-peer-presents-to-me" vs "T-i-present-to-peer" — the cache was
//            overwritten by the inbound push, which is the direction collision
//        (and the four carve-out / argument cases, which lose the inbound half
//         entirely)
//
//   B. delete the hostServiceGranted(ServiceTokenRegistry) carve-out. 2 RED:
//        ATokenRegistryStillGetsItsOutboundRoster   (roster empty, token "")
//        RevokingTheGrantStopsTheCarveOut
//      This is the OPPOSITE failure and it is fleet-fatal: capability_module
//      reads lp_token_keys() for its known-caller gate and lp_token_get() for
//      the credential it presents when pushing, so an inbound-only door empties
//      its roster and every requestModule is refused with "rejecting request
//      from unknown module identity" — fail-closed, and total, at the first
//      cross-module call.
//
//   C. make the carve-out unconditional. 4 RED, including
//        AnInboundPushIsNotAnOutboundCredential
//        AnUngrantedImageGetsNoOutboundEntry
//      Together B and C pin the carve-out to the GRANT rather than to nothing
//      or to everyone.

#include <gtest/gtest.h>

#include "logos_protocol.h"
#include "token_manager.h"

#include <QCoreApplication>
#include <QString>

#include <algorithm>
#include <string>

#include <nlohmann/json.hpp>

namespace {

QCoreApplication* ensureInboundApp()
{
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance())
        new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

std::string takeString(char* owned)
{
    if (!owned) return {};
    const std::string out = owned;
    lp_string_free(owned);
    return out;
}

bool rosterContains(const std::string& dump, const std::string& key)
{
    const nlohmann::json j = nlohmann::json::parse(dump, nullptr, /*allow_exceptions=*/false);
    if (!j.is_array()) return false;
    return std::any_of(j.begin(), j.end(), [&](const nlohmann::json& e) {
        return e.is_string() && e.get<std::string>() == key;
    });
}

class InboundDoor : public ::testing::Test {
protected:
    void SetUp() override
    {
        ensureInboundApp();
        // The grant and the image store are both process-global, so every case
        // starts ungranted and empty rather than from whatever ran before it.
        ASSERT_EQ(lp_grant_host_services(nullptr), LP_OK);
        TokenManager::instance().clearAllTokens();
    }
    void TearDown() override
    {
        lp_grant_host_services(nullptr);
        TokenManager::instance().clearAllTokens();
    }
};

} // namespace

// ── the bug the door exists to close ────────────────────────────────────────

TEST_F(InboundDoor, AnInboundPushIsNotAnOutboundCredential)
{
    ASSERT_EQ(lp_token_save_inbound("caller_a", "T-issued-to-a"), LP_OK);

    // The claim, in the words of the failure it prevents: this module must not
    // be able to present, to caller_a, the very token caller_a was issued to
    // call THIS module.
    EXPECT_EQ(lp_token_get("caller_a"), nullptr)
        << "an inbound grant became an outbound credential — the collision, one "
           "image below ModuleProxy::authorize";
    EXPECT_FALSE(TokenManager::instance().hasToken(QStringLiteral("caller_a")));

    // ...and it did land where it belongs.
    EXPECT_EQ(TokenManager::instance().inbound().token(QStringLiteral("caller_a")),
              QStringLiteral("T-issued-to-a"));
}

TEST_F(InboundDoor, AnInboundPushDoesNotClobberAnExistingOutboundCache)
{
    // The other half of the same key collision, and the one that produced the
    // measured rejection-plus-re-exchange on every call: a cached per-target
    // token for peer P, then an inbound push naming P.
    ASSERT_EQ(lp_token_save("peer", "T-i-present-to-peer"), LP_OK);
    ASSERT_EQ(lp_token_save_inbound("peer", "T-peer-presents-to-me"), LP_OK);

    EXPECT_EQ(takeString(lp_token_get("peer")), std::string("T-i-present-to-peer"))
        << "the inbound push overwrote the outbound cache for the same peer";
    EXPECT_EQ(TokenManager::instance().inbound().token(QStringLiteral("peer")),
              QStringLiteral("T-peer-presents-to-me"));
}

TEST_F(InboundDoor, TheOutboundDoorIsStillTheAnchorSeedingDoor)
{
    // logos_module_accept_token keeps its meaning, and the glue's onInit keeps
    // using it: the module's own host-issued credential, under both bootstrap
    // keys. Nothing about adding an inbound door may change this.
    ASSERT_EQ(lp_token_save("core", "my-anchor"), LP_OK);
    ASSERT_EQ(lp_token_save("capability_module", "my-anchor"), LP_OK);

    EXPECT_EQ(TokenManager::instance().credential(), QStringLiteral("my-anchor"));
    EXPECT_EQ(takeString(lp_token_get("capability_module")), std::string("my-anchor"));
}

// ── the carve-out, in both directions ───────────────────────────────────────

TEST_F(InboundDoor, ATokenRegistryStillGetsItsOutboundRoster)
{
    ASSERT_EQ(lp_grant_host_services(R"(["token_registry"])"), LP_OK);

    // This is the shape of what capability_module receives: core telling it
    // "module_x is loaded, here is its token". To the registry that message is
    // OUTBOUND — the credential it will present when it pushes to module_x —
    // and it is also the roster entry its known-caller gate reads.
    ASSERT_EQ(lp_token_save_inbound("module_x", "T-x"), LP_OK);

    const std::string roster = takeString(lp_token_keys());
    ASSERT_FALSE(roster.empty()) << "the granted roster must be readable";
    EXPECT_TRUE(rosterContains(roster, "module_x"))
        << "capability_module's known-caller gate reads exactly this; empty here "
           "means every requestModule in the fleet is refused with 'rejecting "
           "request from unknown module identity'";

    EXPECT_EQ(takeString(lp_token_get("module_x")), std::string("T-x"))
        << "capability_module authenticates its push to module_x with this value";

    // The inbound half is written too: the registry is also a provider, and a
    // module presenting its own anchor to capability_module is an inbound call.
    EXPECT_EQ(TokenManager::instance().inbound().token(QStringLiteral("module_x")),
              QStringLiteral("T-x"));
}

TEST_F(InboundDoor, AnUngrantedImageGetsNoOutboundEntry)
{
    // Same call, no grant. An ordinary module is not a registry, and for it the
    // message means only "this caller may call you".
    ASSERT_EQ(lp_token_save_inbound("module_x", "T-x"), LP_OK);

    EXPECT_EQ(lp_token_get("module_x"), nullptr)
        << "the carve-out fired for an image that was never granted the registry "
           "role — that is the original bug with an extra step";
    EXPECT_EQ(lp_token_keys(), nullptr) << "and the roster stays closed";
    EXPECT_EQ(TokenManager::instance().inbound().token(QStringLiteral("module_x")),
              QStringLiteral("T-x"));
}

TEST_F(InboundDoor, RevokingTheGrantStopsTheCarveOut)
{
    ASSERT_EQ(lp_grant_host_services(R"(["token_registry"])"), LP_OK);
    ASSERT_EQ(lp_token_save_inbound("early", "T-early"), LP_OK);
    ASSERT_EQ(lp_grant_host_services(nullptr), LP_OK);
    ASSERT_EQ(lp_token_save_inbound("late", "T-late"), LP_OK);

    // The grant is read at the moment of the write, not cached at load: an image
    // that loses the role stops filing new pushes outbound.
    EXPECT_EQ(takeString(lp_token_get("early")), std::string("T-early"));
    EXPECT_EQ(lp_token_get("late"), nullptr);
}

// ── argument handling ───────────────────────────────────────────────────────

TEST_F(InboundDoor, RefusesNullEmptyAndForgedNames)
{
    EXPECT_EQ(lp_token_save_inbound(nullptr, "t"), LP_ERR_INVALID_ARG);
    EXPECT_EQ(lp_token_save_inbound("caller", nullptr), LP_ERR_INVALID_ARG);
    EXPECT_EQ(lp_token_save_inbound("", "t"), LP_ERR_INVALID_ARG);

    // An empty token would read as PRESENT to inbound().contains() while
    // authorizing nothing, which is the worst of both.
    EXPECT_EQ(lp_token_save_inbound("caller", ""), LP_ERR_INVALID_ARG);
    EXPECT_FALSE(TokenManager::instance().inbound().contains(QStringLiteral("caller")));

    // The caller name arrives over RPC, named by capability_module. A name
    // carrying the reserved direction-namespace character must not be able to
    // address any key but its own.
    EXPECT_EQ(lp_token_save_inbound("\001in\001victim", "forged"), LP_ERR_INVALID_ARG);
    EXPECT_EQ(TokenManager::instance().inbound().token(QStringLiteral("victim")), QString());
}

TEST_F(InboundDoor, TheOutboundDoorReportsARefusedForgedNameToo)
{
    // SYMMETRY, and it is a report the caller can act on rather than a log line
    // only an operator can read. lp_token_save_inbound already answers
    // LP_ERR_INVALID_ARG for a name carrying the reserved direction namespace;
    // lp_token_save answered LP_OK for the identical refusal, because
    // TokenManager::saveToken returns void and the C door had nothing to
    // forward. A module tripping the guard therefore saw rc=0 and went on
    // believing it held a credential it does not hold — the failure mode this
    // whole split exists to make loud.
    //
    // RED BEFORE (measured, at this commit):
    //   Expected equality of these values:
    //     lp_token_save("\001in\001victim", "forged")
    //       Which is: 0
    //     LP_ERR_INVALID_ARG
    //       Which is: -1
    EXPECT_EQ(lp_token_save("\001in\001victim", "forged"), LP_ERR_INVALID_ARG);
    EXPECT_EQ(TokenManager::instance().inbound().token(QStringLiteral("victim")),
              QString())
        << "an outbound write addressed the inbound namespace";
    // A PIN ON THE SECOND GUARD, and it is stated in the only accessor that
    // could see the failure. hasToken(), tokenCount() and getTokenKeys() all
    // FILTER reserved keys, so each answers "nothing there" whether or not the
    // write landed; inbound().count() is the one that counts the namespace.
    //
    // Removing the C-door guard alone leaves this GREEN (measured): the write
    // is refused a second time inside TokenManager::saveToken, so the only
    // defect was the return code — which is exactly the finding. This fires
    // when BOTH guards go, which is the state the door was one line away from.
    EXPECT_EQ(TokenManager::instance().inbound().count(), 0)
        << "the refused key landed in the reserved namespace, invisible to every "
           "outbound accessor";

    // The per-identity twin is the same door with a store selector in front of
    // it, so it owes the same answer. Kept in the same case because a fix that
    // reaches one and not the other is the shape this finding already is.
    EXPECT_EQ(lp_token_save_for("some_identity", "\001in\001victim", "forged"),
              LP_ERR_INVALID_ARG);

    // ...and the refusal is about the NAMESPACE, not about the door: an
    // ordinary name still succeeds, so a fix that simply fails everything
    // cannot pass this.
    EXPECT_EQ(lp_token_save("peer", "T-peer"), LP_OK);
    EXPECT_EQ(takeString(lp_token_get("peer")), std::string("T-peer"));
    EXPECT_EQ(lp_token_save_for("some_identity", "peer2", "T-peer2"), LP_OK);
}
