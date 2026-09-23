#include "implementations/qt_remote_plain/qtro_wire.h"

#include <gtest/gtest.h>

using namespace logos::plain;
using namespace logos::qt_remote_plain;

namespace {

std::vector<std::uint8_t> bytes(std::initializer_list<std::uint8_t> values)
{
    return {values};
}

TEST(QtRemotePlainWireTest, HandshakeMatchesTheFrozenQtRo20Frame)
{
    EXPECT_EQ(handshakePacket(), bytes({
        0x16, 0x00, 0x00, 0x00, // 22 bytes after the size word
        0x01, 0x00,             // Handshake
        0x10, 0x00, 0x00, 0x00, // QString byte count
        'Q', 0, 't', 0, 'R', 0, 'O', 0,
        ' ', 0, '2', 0, '.', 0, '0', 0,
    }));
}

TEST(QtRemotePlainWireTest, NestedVariantRoundTripsWithoutQt)
{
    RpcMap map;
    map.emplace("z", RpcValue{RpcBytes{{0x00, 0x7f, 0xff}}});
    RpcList list;
    list.items = {RpcValue{true}, RpcValue{std::int64_t{-42}}, RpcValue{"logos"}};
    map.emplace("a", RpcValue{std::move(list)});

    const Variant input = Variant::fromRpc(RpcValue{std::move(map)});
    Writer writer;
    writer.variant(input);
    Reader reader(writer.data());
    const Variant output = reader.variant();

    EXPECT_EQ(output.type, MetaType::VariantMap);
    ASSERT_TRUE(output.value.isMap());
    EXPECT_EQ(output.value.asMap().at("z"), input.value.asMap().at("z"));
    EXPECT_EQ(output.value.asMap().at("a"), input.value.asMap().at("a"));
    EXPECT_EQ(reader.remaining(), 0u);
}

TEST(QtRemotePlainWireTest, DynamicDefinitionRoundTripsWithoutQt)
{
    const auto input = moduleProxyDefinition();
    Writer writer;
    writer.classDefinition(input);
    Reader reader(writer.data());
    EXPECT_EQ(reader.classDefinition(), input);
    EXPECT_EQ(reader.remaining(), 0u);
}

TEST(QtRemotePlainWireTest, FrameParserHandlesPartialAndConcatenatedInput)
{
    const auto first = addObjectPacket("chat", true);
    const auto second = pingPacket(PacketType::Ping, "chat");

    std::vector<std::uint8_t> buffered(first.begin(), first.begin() + 3);
    EXPECT_FALSE(completeFrameSize(buffered).has_value());
    buffered.insert(buffered.end(), first.begin() + 3, first.end());
    buffered.insert(buffered.end(), second.begin(), second.end());
    ASSERT_TRUE(completeFrameSize(buffered).has_value());
    EXPECT_EQ(*completeFrameSize(buffered), first.size());

    const auto decoded = decodeFrame(first);
    EXPECT_EQ(decoded.type, PacketType::AddObject);
    EXPECT_EQ(decoded.name, "chat");
    Reader payload(decoded.payload);
    EXPECT_TRUE(payload.boolean());
    EXPECT_EQ(payload.remaining(), 0u);
}

TEST(QtRemotePlainWireTest, RejectsTruncationAndUnboundedLengths)
{
    EXPECT_THROW(decodeFrame(bytes({1, 0, 0, 0})), CodecError);

    Writer oversized;
    oversized.u32(0xffffffffu);
    Reader reader(oversized.data());
    EXPECT_THROW((void)reader.stringList(), CodecError);

    auto frame = handshakePacket();
    frame[0] = 0xff;
    frame[1] = 0xff;
    frame[2] = 0xff;
    frame[3] = 0x7f;
    EXPECT_THROW((void)completeFrameSize(frame), CodecError);
}

TEST(QtRemotePlainWireTest, InvalidTextIsReplacedRatherThanRejected)
{
    Writer invalidUtf8;
    ASSERT_NO_THROW(invalidUtf8.variant(Variant::fromRpc(RpcValue{std::string("a\xff" "b")})));
    Reader replaced(invalidUtf8.data());
    EXPECT_EQ(replaced.variant().value.asString(), "a\xef\xbf\xbd" "b");

    // A QString holding one high surrogate, as a Qt peer may send it.
    Writer lone;
    lone.u32(10);
    lone.u8(0);
    lone.u32(2);
    lone.u16(0xd83d);
    Reader reader(lone.data());
    const Variant decoded = reader.variant();
    EXPECT_EQ(decoded.value.asString(), "\xef\xbf\xbd");
    Writer again;
    again.variant(decoded);
    EXPECT_EQ(again.data(), lone.data());
}

TEST(QtRemotePlainWireTest, NullJsonPayloadsDecode)
{
    for (const auto type : {MetaType::JsonObject, MetaType::JsonArray, MetaType::JsonDocument}) {
        Writer nullDocument;
        nullDocument.u32(static_cast<std::uint32_t>(type));
        nullDocument.u8(0);
        nullDocument.u32(0xffffffffu);
        Reader reader(nullDocument.data());
        EXPECT_NO_THROW((void)reader.variant()) << static_cast<std::uint32_t>(type);
    }
}

TEST(QtRemotePlainWireTest, AnUninterpretableTrailingValueCrossesOpaque)
{
    // QBitArray is outside the codec: 3 bits in one byte.
    Writer bits;
    bits.u32(13);
    bits.u8(0);
    bits.u32(3);
    bits.u8(0x05);
    Reader reader(bits.data());
    const Variant value = reader.variantUntil(bits.data().size());
    EXPECT_TRUE(value.opaque);
    EXPECT_EQ(reader.remaining(), 0u);
    Writer again;
    again.variant(value);
    EXPECT_EQ(again.data(), bits.data());
}

TEST(QtRemotePlainWireTest, NestingIsBoundedInsteadOfExhaustingTheStack)
{
    Writer deep;
    for (int i = 0; i < 1000; ++i) {
        deep.u32(9); // QVariantList of one element
        deep.u8(0);
        deep.u32(1);
    }
    deep.u32(0);
    deep.u8(1);
    Reader reader(deep.data());
    EXPECT_THROW((void)reader.variant(), CodecError);
    Reader trailing(deep.data());
    EXPECT_TRUE(trailing.variantUntil(deep.data().size()).opaque);
}

TEST(QtRemotePlainWireTest, ADeeplyNestedJsonPayloadIsRefusedNotRecursedInto)
{
    constexpr int kDepth = 100000;
    std::string text(kDepth, '[');
    text.append(kDepth, ']');
    Writer document;
    document.u32(static_cast<std::uint32_t>(MetaType::JsonDocument));
    document.u8(0);
    document.bytes({text.begin(), text.end()});
    Reader reader(document.data());
    EXPECT_THROW((void)reader.variant(), CodecError);
}

// Each level used to copy its children's values, so a deep frame decoded
// into its size times its depth; nested containers now hand theirs up.
TEST(QtRemotePlainWireTest, DeepNestingDecodesOnceAndReencodesExactly)
{
    constexpr int kDepth = 60;
    constexpr int kWidth = 2000;
    Writer deep;
    for (int level = 0; level < kDepth; ++level) {
        deep.u32(static_cast<std::uint32_t>(MetaType::VariantList));
        deep.u8(0);
        deep.u32(kWidth + 1);
        for (int i = 0; i < kWidth; ++i) {
            deep.u32(static_cast<std::uint32_t>(MetaType::Bool));
            deep.u8(0);
            deep.u8(1);
        }
    }
    deep.u32(static_cast<std::uint32_t>(MetaType::Invalid));
    deep.u8(1);

    Reader reader(deep.data());
    const Variant decoded = reader.variant();
    EXPECT_EQ(reader.remaining(), 0u);
    const RpcValue* level = &decoded.value;
    for (int i = 0; i < kDepth; ++i) {
        ASSERT_TRUE(level->isList()) << "level " << i;
        ASSERT_EQ(level->asList().items.size(), static_cast<std::size_t>(kWidth + 1));
        level = &level->asList().items.back();
    }
    EXPECT_TRUE(level->isNull());
    EXPECT_TRUE(decoded.nestedValues.back().detached);

    Writer again;
    again.variant(decoded);
    EXPECT_EQ(again.data(), deep.data());
}

TEST(QtRemotePlainWireTest, ADetachedMapReencodesInItsWireOrder)
{
    // An unsorted hash inside a list, as Qt may send a QVariantHash.
    Writer outer;
    outer.u32(static_cast<std::uint32_t>(MetaType::VariantList));
    outer.u8(0);
    outer.u32(1);
    outer.u32(static_cast<std::uint32_t>(MetaType::VariantHash));
    outer.u8(0);
    outer.u32(2);
    outer.string("zeta");
    outer.variant(Variant::fromRpc(RpcValue{std::int64_t{1}}));
    outer.string("alpha");
    outer.variant(Variant::fromRpc(RpcValue{std::int64_t{2}}));
    Reader reader(outer.data());
    const Variant decoded = reader.variant();
    ASSERT_TRUE(decoded.value.isList());
    EXPECT_EQ(decoded.value.asList().items[0].asMap().at("alpha"), RpcValue{std::int64_t{2}});
    Writer again;
    again.variant(decoded);
    EXPECT_EQ(again.data(), outer.data());
}

TEST(QtRemotePlainWireTest, MismatchedValuesFailWithACodecError)
{
    Variant notAList;
    notAList.type = MetaType::StringList;
    notAList.isNull = false;
    notAList.value = RpcValue{"not a list"};
    Writer writer;
    EXPECT_THROW(writer.variant(notAList), CodecError);
}

} // namespace
