#include <gtest/gtest.h>
#include "logos_protocol.h"

#include <cstring>

// The protocol version is the single number governing Logos load/call
// compatibility (same MAJOR ⇔ compatible). These tests pin the C ABI
// getters to the header macros so SDKs that forward the linked version
// (instead of minting their own) always agree with logos_protocol.h.

TEST(ProtocolVersion, VersionStringMatchesMacros)
{
    const char* v = lp_protocol_version();
    ASSERT_NE(v, nullptr);
    EXPECT_STREQ(v, LOGOS_PROTOCOL_VERSION_STRING);

    char composed[32];
    std::snprintf(composed, sizeof(composed), "%d.%d.%d",
                  LOGOS_PROTOCOL_VERSION_MAJOR,
                  LOGOS_PROTOCOL_VERSION_MINOR,
                  LOGOS_PROTOCOL_VERSION_PATCH);
    EXPECT_STREQ(v, composed);
}

TEST(ProtocolVersion, AbiMajorMatchesMacro)
{
    EXPECT_EQ(lp_protocol_abi_major(), LOGOS_PROTOCOL_VERSION_MAJOR);
}

TEST(ProtocolVersion, VersionStringIsStatic)
{
    // Documented as a static string: two calls return the same pointer and
    // callers must NOT free it.
    EXPECT_EQ(lp_protocol_version(), lp_protocol_version());
}

TEST(ProtocolVersion, StringFreeAcceptsNull)
{
    lp_string_free(nullptr);  // must be a no-op, not a crash
}

// A symbol added at a MINOR gets a feature macro as well as the bump. Guarding
// on `MINOR >= n` is what could not tell 0.9's two cuts apart, so the macro is
// the thing consumers are told to test — and it has to actually be there.
TEST(ProtocolVersion, TargetPresenceFeatureMacroIsDefined)
{
#if !defined(LOGOS_PROTOCOL_HAS_TARGET_PRESENCE)
    FAIL() << "LOGOS_PROTOCOL_HAS_TARGET_PRESENCE must be defined from 0.10 on";
#else
    SUCCEED();
#endif
}

// The tri-state values are ABI, not an enum: Rust, JS and the generated C++
// wrappers all hard-code them. Renumbering one silently inverts a caller's
// answer, and UNKNOWN==0 is what makes a zeroed/failed read mean "try the call"
// rather than "absent".
TEST(ProtocolVersion, PresenceCodesArePinned)
{
    EXPECT_EQ(LP_PRESENCE_UNKNOWN, 0);
    EXPECT_EQ(LP_PRESENCE_PRESENT, 1);
    EXPECT_EQ(LP_PRESENCE_ABSENT, 2);
}

// A null client is UNKNOWN, never ABSENT. The distinction is the whole safety
// property: a caller folding UNKNOWN to "skip the call" would silently stop
// talking to modules that are running.
TEST(ProtocolVersion, NullClientPresenceIsUnknown)
{
    EXPECT_EQ(lp_target_presence(nullptr), LP_PRESENCE_UNKNOWN);
}
