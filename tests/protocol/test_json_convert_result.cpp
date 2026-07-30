#include <gtest/gtest.h>

#include "logos_json_convert.h"
#include "logos_types.h"

#include <QByteArray>
#include <QMetaType>
#include <QVariant>
#include <QVariantMap>

// `result` is the one LIDL type whose canonical converter pair used to be
// ONE-WAY: qvariantToNlohmann owned LogosResult -> {success,value,error},
// but nothing owned the way back, so every consumer that needed it either
// re-derived it or silently lost the result (a `qvariant_cast<LogosResult>`
// of the decoded QVariantMap default-constructs to success=false).
//
// These tests pin the inverse and, in particular, the two states a
// std::string-typed intermediate cannot represent: an ABSENT error, and a
// `value` whose shape must survive (bytes / 64-bit integers / containers).

using logos::qvariantToNlohmann;
using logos::nlohmannToQVariant;
using logos::jsonToLogosResult;

namespace {
LogosResult makeResult(bool success, QVariant value, QVariant error)
{
    LogosResult r;
    r.success = success;
    r.value = std::move(value);
    r.error = std::move(error);
    return r;
}
}  // namespace

TEST(JsonConvertResult, RoundTripsSuccessWithAbsentError)
{
    qRegisterMetaType<LogosResult>("LogosResult");

    QVariantMap payload;
    payload.insert("id", QStringLiteral("abc"));
    payload.insert("count", QVariant(qlonglong(42)));

    const LogosResult original = makeResult(true, QVariant(payload), QVariant());
    const nlohmann::json j = qvariantToNlohmann(QVariant::fromValue(original));

    ASSERT_TRUE(j.is_object());
    EXPECT_TRUE(j["success"].get<bool>());
    EXPECT_TRUE(j["error"].is_null());

    const LogosResult back = jsonToLogosResult(j);
    EXPECT_TRUE(back.success);
    // The absence of an error must survive: an empty QString here would make
    // `error.isValid()` true and change what every caller sees.
    EXPECT_FALSE(back.error.isValid());
    ASSERT_TRUE(back.value.canConvert<QVariantMap>());
    const QVariantMap m = back.value.toMap();
    EXPECT_EQ(m.value("id").toString(), QStringLiteral("abc"));
    EXPECT_EQ(m.value("count").toLongLong(), 42);
}

TEST(JsonConvertResult, RoundTripsFailureWithErrorString)
{
    qRegisterMetaType<LogosResult>("LogosResult");

    const LogosResult original =
        makeResult(false, QVariant(), QVariant(QStringLiteral("boom")));
    const nlohmann::json j = qvariantToNlohmann(QVariant::fromValue(original));

    const LogosResult back = jsonToLogosResult(j);
    EXPECT_FALSE(back.success);
    EXPECT_EQ(back.error.toString(), QStringLiteral("boom"));
    EXPECT_FALSE(back.value.isValid());
}

TEST(JsonConvertResult, ValueKeepsBytesAndUint64)
{
    qRegisterMetaType<LogosResult>("LogosResult");

    QVariantMap payload;
    payload.insert("blob", QVariant(QByteArray("\x00\x80\xff", 3)));
    payload.insert("big", QVariant(qulonglong(18446744073709551615ULL)));

    const LogosResult original = makeResult(true, QVariant(payload), QVariant());
    const LogosResult back = jsonToLogosResult(qvariantToNlohmann(QVariant::fromValue(original)));

    const QVariantMap m = back.value.toMap();
    // Bytes stay bytes (tagged form decoded back to a QByteArray), not a
    // base64 string and not a one-key map.
    ASSERT_EQ(m.value("blob").userType(), int(QMetaType::QByteArray));
    EXPECT_EQ(m.value("blob").toByteArray().size(), 3);
    EXPECT_EQ(m.value("big").toULongLong(), 18446744073709551615ULL);
}

TEST(JsonConvertResult, NonObjectDecodesToFailure)
{
    const LogosResult back = jsonToLogosResult(nlohmann::json("not-a-result"));
    EXPECT_FALSE(back.success);
    EXPECT_FALSE(back.value.isValid());
    EXPECT_FALSE(back.error.isValid());
}
