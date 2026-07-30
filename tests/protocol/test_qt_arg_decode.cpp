// logos::qtArgDecode — the RUNTIME entry point of the Qt argument decoder,
// used by a provider that knows a parameter only as a QMetaType (the
// QMetaObject dispatch in logos-qt-sdk).
//
// The behavioural end-to-end coverage lives beside its caller in
// logos-qt-sdk's test_provider_arg_validation.cpp. What is pinned HERE is the
// contract this file owns, which a refactor on the protocol side could break
// without any Qt-SDK test noticing:
//
//   1. Ok must yield a QVariant of EXACTLY the requested QMetaType. The
//      QMetaObject dispatch switches on the argument's own typeId to build its
//      Q_ARG, so a value of the right VALUE but the wrong TYPE silently falls
//      through to the QString default and the invoke fails.
//   2. Unchecked must be reported — not guessed at — for every type the codec
//      has no rule for, so the caller can keep its own conversion.
//   3. The rule itself is the codec's; nothing here re-derives it.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include "logos_qt_arg_decode.h"

namespace {

logos::QtArgVerdict decode(const QVariant& in, QMetaType t, QVariant& out)
{
    std::string err;
    return logos::qtArgDecode(in, t, "arg0", out, err);
}

logos::QtArgVerdict decode(const QVariant& in, QMetaType t)
{
    QVariant out;
    return decode(in, t, out);
}

} // namespace

// ── 1. Ok yields exactly the declared type ──────────────────────────────────

TEST(QtArgDecode, OkYieldsExactlyTheDeclaredMetaType)
{
    struct Case { QVariant in; QMetaType type; };
    const Case cases[] = {
        { QVariant(42),                       QMetaType::fromType<int>() },
        { QVariant(42),                       QMetaType::fromType<qlonglong>() },
        { QVariant(42),                       QMetaType::fromType<qulonglong>() },
        { QVariant(42),                       QMetaType::fromType<unsigned int>() },
        { QVariant(true),                     QMetaType::fromType<bool>() },
        { QVariant(2.5),                      QMetaType::fromType<double>() },
        { QVariant(2.5),                      QMetaType::fromType<float>() },
        { QVariant("hi"),                     QMetaType::fromType<QString>() },
        { QVariant("hi"),                     QMetaType::fromType<QByteArray>() },
        { QVariant(QStringList{"a"}),         QMetaType::fromType<QStringList>() },
        { QVariant(QVariantList{QVariant(1)}), QMetaType::fromType<QVariantList>() },
        { QVariant(QVariantMap{}),            QMetaType::fromType<QVariantMap>() },
    };
    for (const Case& c : cases) {
        QVariant out;
        ASSERT_EQ(decode(c.in, c.type, out), logos::QtArgVerdict::Ok)
            << "declared " << c.type.name();
        EXPECT_EQ(out.metaType(), c.type)
            << "declared " << c.type.name() << ", got " << out.typeName();
    }
}

// ── 2. Unchecked, reported rather than guessed ──────────────────────────────

TEST(QtArgDecode, TypesWithNoLidlCounterpartReportUnchecked)
{
    // `any`: declares nothing, so there is nothing to check it against.
    EXPECT_EQ(decode(QVariant(5), QMetaType::fromType<QVariant>()),
              logos::QtArgVerdict::Unchecked);
    // QUrl and friends: the codec has no rule, and inventing one here would
    // refuse calls that work today.
    EXPECT_EQ(decode(QVariant("http://example.com"), QMetaType::fromType<QUrl>()),
              logos::QtArgVerdict::Unchecked);
    EXPECT_EQ(decode(QVariant("x"), QMetaType::fromType<QChar>()),
              logos::QtArgVerdict::Unchecked);
    EXPECT_EQ(decode(QVariant(1), QMetaType(QMetaType::UnknownType)),
              logos::QtArgVerdict::Unchecked);
}

// ── 3. Rejections, with the codec's own diagnostic ──────────────────────────

TEST(QtArgDecode, MismatchesAreRejectedWithAPathedDiagnostic)
{
    QVariant out;
    std::string err;

    EXPECT_EQ(logos::qtArgDecode(QVariant(-1), QMetaType::fromType<qulonglong>(),
                                 "arg0", out, err),
              logos::QtArgVerdict::Rejected);
    EXPECT_NE(err.find("arg0"), std::string::npos) << err;

    EXPECT_EQ(logos::qtArgDecode(QVariant(3.7), QMetaType::fromType<int>(),
                                 "arg1", out, err),
              logos::QtArgVerdict::Rejected);
    EXPECT_NE(err.find("arg1"), std::string::npos) << err;

    // A failure INSIDE a container names the element.
    EXPECT_EQ(logos::qtArgDecode(QVariant(QVariantList{QVariant("a"), QVariant(1)}),
                                 QMetaType::fromType<QStringList>(), "arg2", out, err),
              logos::QtArgVerdict::Rejected);
    EXPECT_NE(err.find("arg2[1]"), std::string::npos) << err;
}

TEST(QtArgDecode, NarrowIntegerRangeIsCheckedNotTruncated)
{
    // int is int32: 2^32 used to arrive as 0 through QVariant::convert.
    EXPECT_EQ(decode(QVariant(qlonglong(4294967296LL)), QMetaType::fromType<int>()),
              logos::QtArgVerdict::Rejected);
    // ...while the same value is fine for a 64-bit parameter.
    EXPECT_EQ(decode(QVariant(qlonglong(4294967296LL)), QMetaType::fromType<qlonglong>()),
              logos::QtArgVerdict::Ok);
}

// ── 4. The rule is the codec's — including the part a stricter one gets wrong

TEST(QtArgDecode, WholeValuedFloatIsAcceptedForAnIntegerAndFractionalIsNot)
{
    QVariant out;
    ASSERT_EQ(decode(QVariant(3.0), QMetaType::fromType<int>(), out),
              logos::QtArgVerdict::Ok);
    EXPECT_EQ(out.toInt(), 3);

    EXPECT_EQ(decode(QVariant(3.7), QMetaType::fromType<int>()),
              logos::QtArgVerdict::Rejected);
}

TEST(QtArgDecode, ContainersAreShapeCheckedAndHandedOnUnchanged)
{
    QVariant out;
    // The caller's own list comes back: rebuilding it from JSON would retype
    // nested elements for no validation gain (the element type is erased by
    // the Qt spelling anyway).
    ASSERT_EQ(decode(QVariant(QVariantList{QVariant(1), QVariant("a")}),
                     QMetaType::fromType<QVariantList>(), out),
              logos::QtArgVerdict::Ok);
    ASSERT_EQ(out.toList().size(), 2);
    EXPECT_EQ(out.toList().at(0).metaType(), QMetaType::fromType<int>());

    EXPECT_EQ(decode(QVariant("notalist"), QMetaType::fromType<QVariantList>()),
              logos::QtArgVerdict::Rejected);
    EXPECT_EQ(decode(QVariant(5), QMetaType::fromType<QVariantMap>()),
              logos::QtArgVerdict::Rejected);
}

TEST(QtArgDecode, BytesKeepTheCodecsDocumentedLenientForm)
{
    // NOT a rejection, deliberately — see bytesFromJsonLenient.
    QVariant out;
    ASSERT_EQ(decode(QVariant(42), QMetaType::fromType<QByteArray>(), out),
              logos::QtArgVerdict::Ok);
    EXPECT_EQ(out.toByteArray(), QByteArray("42"));

    // Round-trip of genuine binary, including NUL and high bits.
    const QByteArray raw("\x00\x80\xff", 3);
    ASSERT_EQ(decode(QVariant(raw), QMetaType::fromType<QByteArray>(), out),
              logos::QtArgVerdict::Ok);
    EXPECT_EQ(out.toByteArray(), raw);
}

// ── 5. The rejection envelope ───────────────────────────────────────────────

TEST(QtArgDecode, DispatchFailedIsTheCanonicalShape)
{
    const nlohmann::json j = logos::dispatchFailedJson("my_module", "expected integer at arg0");
    EXPECT_EQ(j["code"], "dispatch_failed");
    EXPECT_EQ(j["origin"], "my_module");
    EXPECT_EQ(j["message"], "expected integer at arg0");

    const QVariant v = logos::dispatchFailedVariant("my_module", "expected integer at arg0");
    ASSERT_EQ(v.metaType(), QMetaType::fromType<QVariantMap>());
    const QVariantMap m = v.toMap();
    EXPECT_EQ(m.value("code").toString(), QStringLiteral("dispatch_failed"));
    EXPECT_EQ(m.value("origin").toString(), QStringLiteral("my_module"));

    // The QVariant form must be the same object as the JSON one — a consumer
    // reading the plain wire and one reading QtRO have to see one shape.
    EXPECT_EQ(logos::qvariantToNlohmann(v), j);
}
