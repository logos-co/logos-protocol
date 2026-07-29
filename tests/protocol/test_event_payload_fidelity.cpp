#include <gtest/gtest.h>

#include <QMetaType>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <string>

#include "logos_provider_interface.h"

// ---------------------------------------------------------------------------
// Event payload fidelity across the universal -> Qt bridge.
//
// setEventListenerStdBridge adapts the universal event callback (event name +
// a JSON *string* payload) to the Qt-side EventCallback, which takes a
// QVariantList. It is the event-path counterpart of callMethodStdBridge.
//
// The two are NOT symmetric today, and that asymmetry is what these tests pin:
//
//   callMethodStdBridge      -> logos::nlohmannToQVariant       (canonical)
//   setEventListenerStdBridge-> QJsonDocument::fromJson
//                               + QJsonValue::toVariant         (Qt's parser)
//
// Qt 6 backs QJsonValue with QCborValue, so integers up to int64 DO survive the
// Qt parser. What does not survive is a uint64 above int64max: it has no
// integral representation there and falls back to double. Hence the failure is
// narrow and easy to miss — most integers are fine.
//
// Note on who is affected: module providers do NOT go through this bridge.
// A Qt provider stores its callback verbatim (logos-qt-sdk
// QtProviderObject::setEventListener) and a cdylib provider's generated
// emitTrampoline already uses logos::nlohmannArgsToQVariantList, which handles
// is_number_unsigned. The live caller is the logoscore daemon's CoreServiceImpl
// (core_service_dispatch.cpp), which forwards every watched module event
// through here — which is why a uint64 event degrades identically no matter
// what language the emitting module was written in.
//
// The bridge had no payload assertions at all before this file:
// test_universal_provider_dispatch references it only to satisfy the pure
// virtual. That is how the defect survived the codec convergence.
// ---------------------------------------------------------------------------

namespace {

// Minimal universal provider: it does nothing but hand us the std-side event
// callback the bridge installs, so a test can fire an event with an exact JSON
// payload and observe what the Qt side receives.
class EventEmittingProvider : public LogosProviderObject {
public:
    QVariant callMethod(const QString& m, const QVariantList& a) override {
        return callMethodStdBridge(m, a);
    }
    QJsonArray getMethods() override { return getMethodsStdBridge(); }
    void setEventListener(EventCallback cb) override {
        setEventListenerStdBridge(std::move(cb));
    }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("event_sample"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }

    void setEventListenerStd(UniversalEventCallback cb) override {
        stdCallback = std::move(cb);
    }

    // Emit exactly this JSON text as the payload — no re-serialization on the
    // way in, so the test controls the bytes the bridge parses.
    void emitRaw(const std::string& eventName, const std::string& payloadJson) {
        ASSERT_TRUE(static_cast<bool>(stdCallback));
        stdCallback(eventName, payloadJson);
    }

    UniversalEventCallback stdCallback;
};

// Installs a Qt-side listener and records what it receives.
struct Captured {
    QString name;
    QVariantList args;
    int count = 0;
};

Captured captureEvent(const std::string& eventName, const std::string& payloadJson)
{
    EventEmittingProvider provider;
    Captured cap;
    provider.setEventListener([&cap](const QString& n, const QVariantList& a) {
        cap.name = n;
        cap.args = a;
        ++cap.count;
    });
    provider.emitRaw(eventName, payloadJson);
    return cap;
}

} // namespace

// --- The M6 case ----------------------------------------------------------
// A uint64 above int64max is exact on the method path since the canonical codec
// landed. It must be exact on the event path too: same value, same process, one
// hop later.
TEST(EventPayloadFidelity, Uint64AboveInt64MaxSurvives)
{
    const Captured cap = captureEvent("uintEvent", "[18446744073709551615]");

    ASSERT_EQ(cap.count, 1);
    ASSERT_EQ(cap.args.size(), 1);
    EXPECT_EQ(cap.args[0].typeId(), QMetaType::ULongLong)
        << "expected qulonglong, got " << cap.args[0].typeName();
    EXPECT_EQ(cap.args[0].toULongLong(), 18446744073709551615ULL);
}

// 2^53+1 is the smallest integer a double cannot represent. It is well inside
// int64 range, so this fails on any double round-trip while staying clear of
// the signed/unsigned question — it separates "degraded to double" from
// "unsigned not represented".
TEST(EventPayloadFidelity, IntegerPast2Pow53IsNotRounded)
{
    const Captured cap = captureEvent("intEvent", "[9007199254740993]");

    ASSERT_EQ(cap.args.size(), 1);
    EXPECT_EQ(cap.args[0].toLongLong(), 9007199254740993LL);
}

TEST(EventPayloadFidelity, NegativeInt64MinSurvives)
{
    const Captured cap = captureEvent("intEvent", "[-9223372036854775808]");

    ASSERT_EQ(cap.args.size(), 1);
    EXPECT_EQ(cap.args[0].toLongLong(), std::numeric_limits<int64_t>::min());
}

// A large integer nested in a container, not just as a top-level element —
// containers were where the method-path equivalent (M1) hid.
TEST(EventPayloadFidelity, LargeIntegerNestedInContainersSurvives)
{
    const Captured cap = captureEvent(
        "nestedEvent", R"([{"n": 18446744073709551615}, [9007199254740993]])");

    ASSERT_EQ(cap.args.size(), 2);
    EXPECT_EQ(cap.args[0].toMap().value("n").toULongLong(), 18446744073709551615ULL);
    EXPECT_EQ(cap.args[1].toList().at(0).toLongLong(), 9007199254740993LL);
}

// --- Bytes ----------------------------------------------------------------
// Canonical tagged bytes must decode to a QByteArray, exactly as they do on the
// method path. Today they survive end-to-end only because the untouched
// {"_bytes": ...} object round-trips as a QVariantMap and a downstream consumer
// decodes the tag — which is not the same thing as the bridge decoding it.
TEST(EventPayloadFidelity, TaggedBytesDecodeToByteArray)
{
    const Captured cap = captureEvent("bytesEvent", R"([{"_bytes": "YQBiAGM"}])");

    ASSERT_EQ(cap.args.size(), 1);
    EXPECT_EQ(cap.args[0].typeId(), QMetaType::QByteArray)
        << "expected QByteArray, got " << cap.args[0].typeName();
    EXPECT_EQ(cap.args[0].toByteArray(), QByteArray("a\0b\0c", 5));
}

TEST(EventPayloadFidelity, TaggedBytesNestedInContainerDecode)
{
    const Captured cap = captureEvent("bytesEvent", R"([[{"_bytes": "YQBiAGM"}]])");

    ASSERT_EQ(cap.args.size(), 1);
    const QVariantList inner = cap.args[0].toList();
    ASSERT_EQ(inner.size(), 1);
    EXPECT_EQ(inner.at(0).typeId(), QMetaType::QByteArray);
}

// --- Shapes that already work: guard against a fix regressing them ---------
TEST(EventPayloadFidelity, MultipleParametersKeepOrderAndTypes)
{
    const Captured cap = captureEvent("tripleEvent", R"([42, "hi", true])");

    ASSERT_EQ(cap.args.size(), 3);
    EXPECT_EQ(cap.args[0].toLongLong(), 42);
    EXPECT_EQ(cap.args[1].toString(), QStringLiteral("hi"));
    EXPECT_EQ(cap.args[2].toBool(), true);
}

// Converging on the method path's helper also converges its SIGNEDNESS rule:
// nlohmannArgsToQVariantList classifies every non-negative integer as unsigned,
// so a LIDL `int` event argument now arrives as ULongLong rather than LongLong.
// That is what nlohmannToQVariant (methods) and the cdylib emitTrampoline
// already did, so this makes the surfaces agree — but it is an observable
// metatype change, pinned here so it stays a decision rather than a side effect.
// Value-level reads (toLongLong/toULongLong) are unaffected either way.
TEST(EventPayloadFidelity, NonNegativeIntegerCarriesUnsignedMetatype)
{
    const Captured cap = captureEvent("intEvent", "[42]");

    ASSERT_EQ(cap.args.size(), 1);
    EXPECT_EQ(cap.args[0].typeId(), QMetaType::ULongLong);
    EXPECT_EQ(cap.args[0].toLongLong(), 42);
    EXPECT_EQ(cap.args[0].toULongLong(), 42ULL);
}

// A negative integer keeps the signed metatype — the classification is by value,
// not by declared LIDL type, so this is the other half of the rule.
TEST(EventPayloadFidelity, NegativeIntegerCarriesSignedMetatype)
{
    const Captured cap = captureEvent("intEvent", "[-42]");

    ASSERT_EQ(cap.args.size(), 1);
    EXPECT_EQ(cap.args[0].typeId(), QMetaType::LongLong);
    EXPECT_EQ(cap.args[0].toLongLong(), -42);
}

TEST(EventPayloadFidelity, DoubleStaysDouble)
{
    const Captured cap = captureEvent("doubleEvent", "[3.5]");

    ASSERT_EQ(cap.args.size(), 1);
    EXPECT_EQ(cap.args[0].typeId(), QMetaType::Double);
    EXPECT_DOUBLE_EQ(cap.args[0].toDouble(), 3.5);
}

TEST(EventPayloadFidelity, EmptyPayloadYieldsNoArguments)
{
    const Captured cap = captureEvent("bareEvent", "[]");

    ASSERT_EQ(cap.count, 1);
    EXPECT_EQ(cap.args.size(), 0);
}

TEST(EventPayloadFidelity, NullElementSurvivesAsAnElement)
{
    const Captured cap = captureEvent("nullEvent", R"(["a", null, "b"])");

    ASSERT_EQ(cap.args.size(), 3);
    EXPECT_TRUE(cap.args[1].isNull());
}

// A non-array payload is the documented fallback: it is handed over as a single
// string argument rather than dropped. Pinned so a fix keeps the behaviour.
TEST(EventPayloadFidelity, NonArrayPayloadFallsBackToSingleStringArgument)
{
    const Captured cap = captureEvent("rawEvent", "not json at all");

    ASSERT_EQ(cap.args.size(), 1);
    EXPECT_EQ(cap.args[0].toString(), QStringLiteral("not json at all"));
}
