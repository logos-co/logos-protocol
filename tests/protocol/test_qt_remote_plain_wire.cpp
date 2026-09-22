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

} // namespace
