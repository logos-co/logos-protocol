#include <gtest/gtest.h>
#include "logos_protocol.h"
#include "logos_runtime_delegate.h"
#include "logos_transport_config.h"

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

// Plugins built before qt_remote_plain existed carry these numbers in the
// LogosTransportConfig objects they share with the host in-process.
TEST(ProtocolVersion, TransportProtocolsKeepTheirNumbers)
{
    EXPECT_EQ(static_cast<int>(LogosProtocol::LocalSocket), 0);
    EXPECT_EQ(static_cast<int>(LogosProtocol::Tcp), 1);
    EXPECT_EQ(static_cast<int>(LogosProtocol::TcpSsl), 2);
    EXPECT_EQ(static_cast<int>(LogosProtocol::QtRemotePlain), 3);
    EXPECT_EQ(static_cast<int>(LogosProtocol::Inproc), 4);
}

// MINORs 10 and 11 were reused (see logos_protocol.h), so each addition of the
// 0.12 wave has a feature macro, and arrives with it.
TEST(ProtocolVersion, TheQtRemotePlainWaveHasFeatureMacros)
{
#if defined(LOGOS_PROTOCOL_HAS_QT_REMOTE_PLAIN) && defined(LOGOS_PROTOCOL_HAS_CURRENT_CALLER_JSON) \
    && defined(LOGOS_PROTOCOL_HAS_PROVIDER_TOKEN_VALIDATOR) \
    && defined(LOGOS_PROTOCOL_HAS_WILDCARD_SUBSCRIBE) \
    && defined(LOGOS_PROTOCOL_HAS_STAGED_PUBLICATION) && defined(LOGOS_PROTOCOL_HAS_STRING_COPY)
    const void* symbols[] = {reinterpret_cast<const void*>(&lp_current_caller_json),
                             reinterpret_cast<const void*>(&lp_provider_set_token_validator),
                             reinterpret_cast<const void*>(&lp_provider_prepare),
                             reinterpret_cast<const void*>(&lp_string_copy)};
    for (const void* symbol : symbols) EXPECT_NE(symbol, nullptr);
#else
    FAIL() << "an addition of the 0.12 wave has no feature macro";
#endif
}

// Every addition of 0.13 arrives with a feature macro, in both runtimes.
TEST(ProtocolVersion, TheInProcessWaveHasFeatureMacros)
{
#if defined(LOGOS_PROTOCOL_HAS_INPROC) && defined(LOGOS_PROTOCOL_HAS_CALLER_RESOLVER) \
    && defined(LOGOS_PROTOCOL_HAS_RUNTIME_DELEGATE)
    const void* symbols[] = {reinterpret_cast<const void*>(&lp_provider_set_caller_resolver),
                             reinterpret_cast<const void*>(&lp_runtime_delegate_create),
                             reinterpret_cast<const void*>(&lp_runtime_delegate_release)};
    for (const void* symbol : symbols) EXPECT_NE(symbol, nullptr);
#else
    FAIL() << "an addition of the 0.13 wave has no feature macro";
#endif
}
