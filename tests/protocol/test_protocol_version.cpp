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

TEST(ProtocolVersion, StringCopyUsesProtocolOwnedStorage)
{
    char source[] = "allocator-boundary";
    char* copy = lp_string_copy(source);
    ASSERT_NE(copy, nullptr);
    EXPECT_STREQ(copy, source);
    EXPECT_NE(copy, source);
    lp_string_free(copy);
    EXPECT_EQ(lp_string_copy(nullptr), nullptr);
}
