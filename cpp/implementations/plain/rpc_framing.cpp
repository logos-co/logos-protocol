#include "rpc_framing.h"

#include <cstring>

namespace logos::plain {

namespace {

void writeBe32(std::vector<uint8_t>& out, uint32_t n)
{
    out.push_back(static_cast<uint8_t>((n >> 24) & 0xff));
    out.push_back(static_cast<uint8_t>((n >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((n >>  8) & 0xff));
    out.push_back(static_cast<uint8_t>( n        & 0xff));
}

uint32_t readBe32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
         | (uint32_t(p[2]) <<  8) |  uint32_t(p[3]);
}

} // anonymous namespace

std::vector<uint8_t>
encodeFrame(MessageType tag, const std::vector<uint8_t>& payload, uint32_t limit)
{
    // Total frame body length = 1 byte (tag) + payload bytes. The 4-byte
    // length prefix is not included in the length value it describes.
    const uint64_t bodyLen = 1u + payload.size();
    if (bodyLen > limit)
        throw FramingError("frame too large");

    std::vector<uint8_t> out;
    out.reserve(4 + bodyLen);
    writeBe32(out, static_cast<uint32_t>(bodyLen));
    out.push_back(static_cast<uint8_t>(tag));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<uint8_t>
encodeFrame(IWireCodec& codec, const AnyMessage& msg, uint32_t limit)
{
    return encodeFrame(messageTypeOf(msg), codec.encode(msg), limit);
}

void FrameReader::append(const uint8_t* data, std::size_t len)
{
    m_buf.insert(m_buf.end(), data, data + len);
}

bool FrameReader::next(MessageType& tag, std::vector<uint8_t>& payload)
{
    // Need at least 4 bytes for the length prefix.
    if (m_buf.size() < 4) return false;

    const uint32_t bodyLen = readBe32(m_buf.data());
    if (bodyLen == 0)                         throw FramingError("zero-length frame");
    if (bodyLen > m_limit)                    throw FramingError("frame length exceeds cap");

    const std::size_t total = 4u + bodyLen;
    if (m_buf.size() < total) return false;

    // Tag + payload.
    tag = static_cast<MessageType>(m_buf[4]);
    payload.assign(m_buf.begin() + 5, m_buf.begin() + total);

    // Drop the consumed bytes. erase from front is O(n) in buffer size;
    // acceptable while frames are small and rare. Can switch to a ring
    // buffer if profiling ever flags this.
    m_buf.erase(m_buf.begin(), m_buf.begin() + total);
    return true;
}

} // namespace logos::plain
