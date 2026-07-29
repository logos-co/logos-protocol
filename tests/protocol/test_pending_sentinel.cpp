#include <gtest/gtest.h>

#include <QJsonObject>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include "logos_async_dispatch.h"

// ---------------------------------------------------------------------------
// The pending-call sentinel is in-band: a "multi" provider returns a QVariantMap
// carrying the call id under __logos_pending_call__, because the dispatch slot
// returns a single QVariant and there is no out-of-band place to say "deferred"
// without an ABI break.
//
// In-band means user data can imitate it. The four detection sites used to test
// `m.contains(pendingCallKey())` and nothing else, so ANY map carrying that key
// was taken for a deferred call: the consumer waited in a nested event loop for
// a completion that never arrives and the call hung for the full timeout. An
// `any` slot is enough to reach it.
//
// These pin the shape check. They do NOT claim the sentinel is unforgeable — a
// forgery of the canonical shape is still indistinguishable, because that is the
// real thing. See known.json.
// ---------------------------------------------------------------------------

namespace {

QVariant canonical(const QString& id)
{
    QVariantMap m;
    m[logos::pendingCallKey()] = id;
    return QVariant(m);
}

} // namespace

TEST(PendingSentinel, CanonicalShapeIsRecognisedAndYieldsTheCallId)
{
    QString id;
    ASSERT_TRUE(logos::isPendingCallSentinel(canonical("lc-7"), &id));
    EXPECT_EQ(id, QStringLiteral("lc-7"));
}

TEST(PendingSentinel, CallIdOutParamIsOptional)
{
    EXPECT_TRUE(logos::isPendingCallSentinel(canonical("lc-0")));
}

// The case the conformance matrix measured: user data that merely CONTAINS the
// key. Previously hijacked the call; must now pass through as an ordinary map.
TEST(PendingSentinel, ExtraKeysMeanItIsOrdinaryUserData)
{
    QVariantMap m;
    m[logos::pendingCallKey()] = QStringLiteral("lc-1");
    m[QStringLiteral("x")] = 2;

    EXPECT_FALSE(logos::isPendingCallSentinel(QVariant(m)));
}

// echoAny({"__logos_pending_call__": 1, "x": 2}) — the exact matrix payload.
TEST(PendingSentinel, MatrixPayloadIsNotASentinel)
{
    QVariantMap m;
    m[logos::pendingCallKey()] = 1;
    m[QStringLiteral("x")] = 2;

    EXPECT_FALSE(logos::isPendingCallSentinel(QVariant(m)));
}

// A call id is a string. A one-key map whose value is a number is user data.
TEST(PendingSentinel, NonStringValueIsNotASentinel)
{
    QVariantMap m;
    m[logos::pendingCallKey()] = 1;

    EXPECT_FALSE(logos::isPendingCallSentinel(QVariant(m)));
}

TEST(PendingSentinel, EmptyCallIdIsNotASentinel)
{
    EXPECT_FALSE(logos::isPendingCallSentinel(canonical(QString())));
}

TEST(PendingSentinel, WrongKeyIsNotASentinel)
{
    QVariantMap m;
    m[QStringLiteral("pending")] = QStringLiteral("lc-1");

    EXPECT_FALSE(logos::isPendingCallSentinel(QVariant(m)));
}

TEST(PendingSentinel, NonMapValuesAreNotSentinels)
{
    EXPECT_FALSE(logos::isPendingCallSentinel(QVariant()));
    EXPECT_FALSE(logos::isPendingCallSentinel(QVariant(QStringLiteral("lc-1"))));
    EXPECT_FALSE(logos::isPendingCallSentinel(QVariant(42)));
    EXPECT_FALSE(logos::isPendingCallSentinel(QVariant(QVariantList{1, 2})));
    EXPECT_FALSE(logos::isPendingCallSentinel(QVariant(QVariantMap{})));
}

// The QJsonObject arm mirrors isUnauthorizedSentinel's: some json_convert paths
// historically produced QJsonObject rather than QVariantMap.
TEST(PendingSentinel, QJsonObjectArmBehavesTheSame)
{
    QJsonObject good;
    good[logos::pendingCallKey()] = QStringLiteral("lc-2");
    QString id;
    ASSERT_TRUE(logos::isPendingCallSentinel(QVariant(good), &id));
    EXPECT_EQ(id, QStringLiteral("lc-2"));

    QJsonObject extra;
    extra[logos::pendingCallKey()] = QStringLiteral("lc-2");
    extra[QStringLiteral("x")] = 2;
    EXPECT_FALSE(logos::isPendingCallSentinel(QVariant(extra)));

    QJsonObject numeric;
    numeric[logos::pendingCallKey()] = 1;
    EXPECT_FALSE(logos::isPendingCallSentinel(QVariant(numeric)));
}

// What the guard does NOT do, pinned so nobody reads the green cells above as
// "the sentinel is safe". A canonical-shape forgery is the real thing.
TEST(PendingSentinel, CanonicalShapeForgeryIsStillIndistinguishable)
{
    QVariantMap forged;
    forged[logos::pendingCallKey()] = QStringLiteral("lc-0");

    EXPECT_TRUE(logos::isPendingCallSentinel(QVariant(forged)))
        << "if this ever fails the in-band design changed — update known.json";
}
