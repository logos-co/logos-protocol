#include <gtest/gtest.h>
#include <QtTest/QSignalSpy>
#include "token_manager.h"

class TokenManagerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        TokenManager::instance().clearAllTokens();
    }
};

TEST_F(TokenManagerTest, SaveAndGetToken)
{
    TokenManager::instance().saveToken("mod_a", "token_a");
    EXPECT_EQ(TokenManager::instance().getToken("mod_a"), "token_a");
}

TEST_F(TokenManagerTest, GetMissingTokenReturnsEmpty)
{
    EXPECT_TRUE(TokenManager::instance().getToken("nonexistent").isEmpty());
}

TEST_F(TokenManagerTest, HasToken)
{
    EXPECT_FALSE(TokenManager::instance().hasToken("key"));
    TokenManager::instance().saveToken("key", "val");
    EXPECT_TRUE(TokenManager::instance().hasToken("key"));
}

TEST_F(TokenManagerTest, RemoveToken)
{
    TokenManager::instance().saveToken("key", "val");
    EXPECT_TRUE(TokenManager::instance().removeToken("key"));
    EXPECT_FALSE(TokenManager::instance().hasToken("key"));
}

TEST_F(TokenManagerTest, RemoveNonexistentTokenReturnsFalse)
{
    EXPECT_FALSE(TokenManager::instance().removeToken("missing"));
}

TEST_F(TokenManagerTest, ClearAllTokens)
{
    TokenManager::instance().saveToken("a", "1");
    TokenManager::instance().saveToken("b", "2");
    TokenManager::instance().clearAllTokens();
    EXPECT_EQ(TokenManager::instance().tokenCount(), 0);
}

TEST_F(TokenManagerTest, TokenCount)
{
    EXPECT_EQ(TokenManager::instance().tokenCount(), 0);
    TokenManager::instance().saveToken("x", "1");
    EXPECT_EQ(TokenManager::instance().tokenCount(), 1);
    TokenManager::instance().saveToken("y", "2");
    EXPECT_EQ(TokenManager::instance().tokenCount(), 2);
}

TEST_F(TokenManagerTest, GetTokenKeys)
{
    TokenManager::instance().saveToken("alpha", "1");
    TokenManager::instance().saveToken("beta", "2");
    QList<QString> keys = TokenManager::instance().getTokenKeys();
    EXPECT_EQ(keys.size(), 2);
    EXPECT_TRUE(keys.contains("alpha"));
    EXPECT_TRUE(keys.contains("beta"));
}

TEST_F(TokenManagerTest, OverwriteToken)
{
    TokenManager::instance().saveToken("key", "old");
    TokenManager::instance().saveToken("key", "new");
    EXPECT_EQ(TokenManager::instance().getToken("key"), "new");
    EXPECT_EQ(TokenManager::instance().tokenCount(), 1);
}

TEST_F(TokenManagerTest, SignalTokenSaved)
{
    QSignalSpy spy(&TokenManager::instance(), &TokenManager::tokenSaved);
    TokenManager::instance().saveToken("k", "v");
    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toString(), "k");
}

TEST_F(TokenManagerTest, SignalTokenRemoved)
{
    TokenManager::instance().saveToken("k", "v");
    QSignalSpy spy(&TokenManager::instance(), &TokenManager::tokenRemoved);
    TokenManager::instance().removeToken("k");
    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toString(), "k");
}

TEST_F(TokenManagerTest, SignalAllTokensCleared)
{
    TokenManager::instance().saveToken("k", "v");
    QSignalSpy spy(&TokenManager::instance(), &TokenManager::allTokensCleared);
    TokenManager::instance().clearAllTokens();
    EXPECT_EQ(spy.count(), 1);
}

// --- redactToken: log-safe token fingerprinting (F-007) ---
//
// Tokens gate all cross-module RPC and are accepted by value, so a raw token
// recovered from a log line is directly replayable. redactToken() is the
// guard that keeps the cleartext secret out of logs; these tests pin the
// security-relevant properties: the raw value never appears, the output is
// stable for correlation, and distinct tokens map to distinct fingerprints.

// ── the store mechanics of the DIRECTION split ──────────────────────────────
//
// The behavioural consequences live in test_token_direction.cpp; these are the
// three pieces of bookkeeping that hold the two names and the one value
// together, and that nothing else exercises.

TEST_F(TokenManagerTest, TheInboundOverloadsAllReachTheSameMap)
{
    // The const char* overload is not sugar: without it a literal pair is
    // AMBIGUOUS between the QString and std::string forms and the call does not
    // compile at all. This test is therefore as much a compile-time assertion
    // as a runtime one — it was added because building logos-qt-sdk against
    // this header failed on exactly that.
    EXPECT_TRUE(TokenManager::instance().saveInboundToken("peer_lit", "tok-lit"));
    EXPECT_TRUE(TokenManager::instance().saveInboundToken(
        QStringLiteral("peer_q"), QStringLiteral("tok-q")));
    EXPECT_TRUE(TokenManager::instance().saveInboundToken(
        std::string("peer_std"), std::string("tok-std")));

    const TokenManager::InboundView in = TokenManager::instance().inbound();
    EXPECT_EQ(in.token(QStringLiteral("peer_lit")), QStringLiteral("tok-lit"));
    EXPECT_EQ(in.token(QStringLiteral("peer_q")),   QStringLiteral("tok-q"));
    EXPECT_EQ(in.token(QStringLiteral("peer_std")), QStringLiteral("tok-std"));
    EXPECT_EQ(in.count(), 3);

    // And none of them is visible to the outbound surface.
    EXPECT_EQ(TokenManager::instance().tokenCount(), 0);
    EXPECT_FALSE(TokenManager::instance().hasToken("peer_lit"));
}

TEST_F(TokenManagerTest, RemovingABootstrapKeyDoesNotStrandTheCredential)
{
    // One value under two names. Dropping ONE name must leave the credential
    // asserting the value the store still holds under the other; dropping BOTH
    // must leave no credential at all. Reachable from ordinary traffic:
    // LogosAPIClient::removeToken() runs on the re-exchange path.
    TokenManager::instance().adoptCredential(QStringLiteral("the-credential"));
    ASSERT_EQ(TokenManager::instance().credential(), QStringLiteral("the-credential"));

    ASSERT_TRUE(TokenManager::instance().removeToken("core"));
    EXPECT_EQ(TokenManager::instance().credential(), QStringLiteral("the-credential"))
        << "dropping one of the two bootstrap names cleared a credential the "
           "store still holds under the other";

    ASSERT_TRUE(TokenManager::instance().removeToken("capability_module"));
    EXPECT_TRUE(TokenManager::instance().credential().isEmpty())
        << "the credential outlived every bootstrap key it was installed under";
}

TEST_F(TokenManagerTest, ClearAllTokensClearsAllThree)
{
    // resetIdentity() is documented to clear the credential too — a reload
    // re-mints and re-registers, so a surviving credential is a locked-out
    // reload that looks live. The inbound record goes for the same reason: it
    // names the callers of the PREVIOUS incarnation.
    TokenManager::instance().saveToken("callee", "out-tok");
    TokenManager::instance().saveInboundToken("caller", "in-tok");
    TokenManager::instance().adoptCredential(QStringLiteral("cred"));

    TokenManager::instance().clearAllTokens();

    EXPECT_EQ(TokenManager::instance().tokenCount(), 0);
    EXPECT_EQ(TokenManager::instance().inbound().count(), 0);
    EXPECT_TRUE(TokenManager::instance().credential().isEmpty());
}

TEST(RedactTokenTest, NeverContainsRawTokenValue)
{
    const QString secret = "3f2a9c00-dead-beef-cafe-0123456789ab";
    const QString redacted = redactToken(secret);
    EXPECT_FALSE(redacted.contains(secret));
    // A replayer must not be able to recover the secret from any substring:
    // the redaction is a one-way hash, so the cleartext is absent entirely.
    EXPECT_EQ(redacted.indexOf(secret), -1);
}

TEST(RedactTokenTest, EmptyTokenRendersAsNone)
{
    EXPECT_EQ(redactToken(QString()), QStringLiteral("<none>"));
    EXPECT_EQ(redactToken(""), QStringLiteral("<none>"));
}

TEST(RedactTokenTest, IsDeterministicForCorrelation)
{
    // Same token must always fingerprint identically so operators can still
    // correlate log lines referring to the same credential.
    const QString token = "some-capability-token";
    EXPECT_EQ(redactToken(token), redactToken(token));
}

TEST(RedactTokenTest, DistinctTokensProduceDistinctFingerprints)
{
    EXPECT_NE(redactToken("token-one"), redactToken("token-two"));
}

TEST(RedactTokenTest, HasStableFingerprintShape)
{
    // Fixed prefix + 8 hex chars of SHA-256 + ellipsis, e.g. "redacted:1a2b3c4d…".
    const QString redacted = redactToken("abc");
    EXPECT_TRUE(redacted.startsWith(QStringLiteral("redacted:")));
    EXPECT_TRUE(redacted.endsWith(QStringLiteral("…")));
    // SHA-256("abc") = ba7816bf8f01cfea... → first 8 hex chars are "ba7816bf".
    EXPECT_EQ(redacted, QStringLiteral("redacted:ba7816bf…"));
}
