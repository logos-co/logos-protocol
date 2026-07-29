#include <gtest/gtest.h>

#include "json_codec.h"
#include "cbor_codec.h"
#include "qvariant_rpc_value.h"

#include <QJsonValue>
#include <QMetaType>
#include <QVariant>

#include <cstdint>
#include <limits>

// ---------------------------------------------------------------------------
// uint64 across the plain (tcp / tcp_ssl) wire.
//
// RpcValue's variant had no unsigned alternative, so every uint64 above
// int64max was squeezed through int64_t and arrived as -1 — independently in
// both directions:
//
//   outbound  qvariant_rpc_value.cpp  QMetaType::ULongLong -> int64_t(...)
//   inbound   json_mapping.cpp        is_number_unsigned   -> get<int64_t>()
//
// Neither wraps loudly: .get<int64_t>() past int64max returns -1 with no
// exception. Two peers both running this code agreed on -1, so nothing looked
// broken from inside.
//
// This was NOT a wire-format limitation. Both codecs carry uint64 natively
// (CBOR emits major type 0, `1b ff..ff`), and the envelope's own `id` field
// already crossed this wire as a uint64_t. Only RpcValue *payloads* could not
// represent it.
//
// The plain tier had no integer test outside int32 range before this file:
// test_json_codec and test_cbor_codec used 42 and 3.
// ---------------------------------------------------------------------------

using namespace logos::plain;

namespace {

constexpr uint64_t kUint64Max = std::numeric_limits<uint64_t>::max();
constexpr uint64_t kInt64Max  = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());

AnyMessage roundtrip(IWireCodec& codec, const AnyMessage& msg)
{
    auto bytes = codec.encode(msg);
    return codec.decode(messageTypeOf(msg), bytes.data(), bytes.size());
}

EventMessage eventCarrying(std::vector<RpcValue> data)
{
    EventMessage e;
    e.object = "test_fullapi";
    e.eventName = "uintEvent";
    e.data = std::move(data);
    return e;
}

} // namespace

// --- The representation rule ----------------------------------------------
// The unsigned alternative is used ONLY where int64_t loses information.
// Anything broader would change the representation of every non-negative
// integer already crossing this wire — and std::variant equality compares the
// alternative index, so it would also break comparisons against int64-built
// values, to fix nothing.

TEST(PlainUint64, MakeIntegerUsesSignedAlternativeWhenItFits)
{
    EXPECT_TRUE(RpcValue::makeInteger(0).isInt());
    EXPECT_TRUE(RpcValue::makeInteger(42).isInt());
    EXPECT_TRUE(RpcValue::makeInteger(kInt64Max).isInt());
    EXPECT_EQ(RpcValue::makeInteger(kInt64Max).asInt(),
              std::numeric_limits<int64_t>::max());

    // Unchanged representation means unchanged equality.
    EXPECT_EQ(RpcValue::makeInteger(42), RpcValue{int64_t(42)});
}

TEST(PlainUint64, MakeIntegerUsesUnsignedAlternativeOnlyPastInt64Max)
{
    EXPECT_TRUE(RpcValue::makeInteger(kInt64Max + 1).isUInt());
    EXPECT_TRUE(RpcValue::makeInteger(kUint64Max).isUInt());
    EXPECT_EQ(RpcValue::makeInteger(kUint64Max).asUInt(), kUint64Max);
}

TEST(PlainUint64, IsIntegralCoversBothAlternatives)
{
    EXPECT_TRUE(RpcValue::makeInteger(42).isIntegral());
    EXPECT_TRUE(RpcValue::makeInteger(kUint64Max).isIntegral());
    EXPECT_FALSE(RpcValue{3.5}.isIntegral());
    EXPECT_FALSE(RpcValue{std::string("7")}.isIntegral());
}

// --- JSON codec ------------------------------------------------------------

TEST(PlainUint64, JsonCodecRoundTripsUint64Max)
{
    JsonCodec codec;
    const AnyMessage out = roundtrip(
        codec, eventCarrying({RpcValue::makeInteger(kUint64Max)}));

    const auto* evt = std::get_if<EventMessage>(&out);
    ASSERT_NE(evt, nullptr);
    ASSERT_EQ(evt->data.size(), 1u);
    ASSERT_TRUE(evt->data[0].isUInt()) << "decoded into the wrong alternative";
    EXPECT_EQ(evt->data[0].asUInt(), kUint64Max);
}

TEST(PlainUint64, JsonCodecRoundTripsUint64NestedInContainers)
{
    RpcMap m;
    m.emplace("n", RpcValue::makeInteger(kUint64Max));
    RpcList l;
    l.items.push_back(RpcValue::makeInteger(kInt64Max + 1));

    JsonCodec codec;
    const AnyMessage out = roundtrip(
        codec, eventCarrying({RpcValue{std::move(m)}, RpcValue{std::move(l)}}));

    const auto* evt = std::get_if<EventMessage>(&out);
    ASSERT_NE(evt, nullptr);
    ASSERT_EQ(evt->data.size(), 2u);
    EXPECT_EQ(evt->data[0].asMap().at("n").asUInt(), kUint64Max);
    EXPECT_EQ(evt->data[1].asList().items.at(0).asUInt(), kInt64Max + 1);
}

TEST(PlainUint64, JsonCodecKeepsNegativeIntegersSigned)
{
    JsonCodec codec;
    const AnyMessage out = roundtrip(
        codec, eventCarrying({RpcValue{std::numeric_limits<int64_t>::min()}}));

    const auto* evt = std::get_if<EventMessage>(&out);
    ASSERT_NE(evt, nullptr);
    ASSERT_TRUE(evt->data[0].isInt());
    EXPECT_EQ(evt->data[0].asInt(), std::numeric_limits<int64_t>::min());
}

// --- CBOR codec ------------------------------------------------------------

TEST(PlainUint64, CborCodecRoundTripsUint64Max)
{
    CborCodec codec;
    const AnyMessage out = roundtrip(
        codec, eventCarrying({RpcValue::makeInteger(kUint64Max)}));

    const auto* evt = std::get_if<EventMessage>(&out);
    ASSERT_NE(evt, nullptr);
    ASSERT_EQ(evt->data.size(), 1u);
    ASSERT_TRUE(evt->data[0].isUInt());
    EXPECT_EQ(evt->data[0].asUInt(), kUint64Max);
}

TEST(PlainUint64, CborCodecRoundTripsUint64NestedInContainers)
{
    RpcMap m;
    m.emplace("n", RpcValue::makeInteger(kUint64Max));

    CborCodec codec;
    const AnyMessage out = roundtrip(codec, eventCarrying({RpcValue{std::move(m)}}));

    const auto* evt = std::get_if<EventMessage>(&out);
    ASSERT_NE(evt, nullptr);
    EXPECT_EQ(evt->data[0].asMap().at("n").asUInt(), kUint64Max);
}

// --- The Qt boundary, both directions --------------------------------------

TEST(PlainUint64, QVariantToRpcValuePreservesUint64)
{
    const QVariant v = QVariant::fromValue<qulonglong>(kUint64Max);
    const RpcValue r = qvariantToRpcValue(v);

    ASSERT_TRUE(r.isUInt()) << "wrapped to int64 again";
    EXPECT_EQ(r.asUInt(), kUint64Max);
}

TEST(PlainUint64, RpcValueToQVariantPreservesUint64)
{
    const QVariant v = rpcValueToQVariant(RpcValue::makeInteger(kUint64Max));

    EXPECT_EQ(v.typeId(), QMetaType::ULongLong) << "got " << v.typeName();
    EXPECT_EQ(v.toULongLong(), kUint64Max);
}

TEST(PlainUint64, QtBoundaryRoundTripIsExact)
{
    const QVariant in = QVariant::fromValue<qulonglong>(kUint64Max);
    const QVariant out = rpcValueToQVariant(qvariantToRpcValue(in));

    EXPECT_EQ(out.toULongLong(), kUint64Max);
}

// A uint that fits int64 keeps crossing as signed, exactly as before. Pinned so
// the narrow rule stays visible rather than assumed.
TEST(PlainUint64, SmallUnsignedStillCrossesAsSigned)
{
    const QVariant in = QVariant::fromValue<qulonglong>(42);
    const RpcValue r = qvariantToRpcValue(in);

    EXPECT_TRUE(r.isInt());
    EXPECT_EQ(rpcValueToQVariant(r).typeId(), QMetaType::LongLong);
}

// --- The off-by-one in the QJsonValue::Double guard ------------------------
// double(int64max) rounds UP to exactly 2^63, so `d <= double(int64max)` used to
// admit 2^63 and then run int64_t(d) out of range — undefined behaviour,
// saturating on arm64 and INT64_MIN on x86-64.

TEST(PlainUint64, DoubleAtTwoPow63DoesNotEnterTheIntegerBranch)
{
    const QVariant v = QVariant(QJsonValue(9223372036854775808.0));   // 2^63
    const RpcValue r = qvariantToRpcValue(v);

    EXPECT_FALSE(r.isInt()) << "2^63 is not representable as int64_t";
    ASSERT_TRUE(r.isDouble());
    EXPECT_DOUBLE_EQ(r.asDouble(), 9223372036854775808.0);
}

TEST(PlainUint64, DoubleAtInt64MinStillTakesTheIntegerBranch)
{
    const QVariant v = QVariant(QJsonValue(-9223372036854775808.0));  // -2^63, exact
    const RpcValue r = qvariantToRpcValue(v);

    ASSERT_TRUE(r.isInt());
    EXPECT_EQ(r.asInt(), std::numeric_limits<int64_t>::min());
}
