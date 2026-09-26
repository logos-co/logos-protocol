#ifndef LOGOS_PLAIN_RPC_FRAMING_H
#define LOGOS_PLAIN_RPC_FRAMING_H

#include "rpc_message.h"
#include "wire_codec.h"

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace logos::plain {

// -----------------------------------------------------------------------------
// Wire framing: one frame is
//
//   [4-byte big-endian length N][1-byte MessageType tag][N-1 payload bytes]
//
// The length covers the tag + payload. 4 bytes give us 4 GiB max per frame
// (way more than we'll ever need); 1-byte tag holds a MessageType enum
// value. Payload bytes come from an IWireCodec.
// -----------------------------------------------------------------------------

class FramingError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Maximum accepted frame length (length-prefix value). 16 MiB is plenty for
// anything we'll RPC today, and guards against runaway allocation on
// malformed input.
constexpr uint32_t kMaxFrameLength = 16u * 1024u * 1024u;

// Build a complete frame around a codec-produced payload. Throws FramingError
// when the frame would exceed `limit` (a connection's negotiated maximum).
std::vector<uint8_t>
encodeFrame(MessageType tag, const std::vector<uint8_t>& payload,
            uint32_t limit = kMaxFrameLength);

// Convenience: encode message via codec + frame in one step.
std::vector<uint8_t> encodeFrame(IWireCodec& codec, const AnyMessage& msg,
                                 uint32_t limit = kMaxFrameLength);

// Incremental reader: feed bytes via `append`, pull complete frames out via
// `next`. Useful with Asio async reads that deliver arbitrary chunk sizes.
class FrameReader {
public:
    void append(const uint8_t* data, std::size_t len);
    void append(const std::vector<uint8_t>& chunk) {
        append(chunk.data(), chunk.size());
    }

    // Returns true and populates `tag` + `payload` with one frame's worth
    // of data. Returns false if the buffer doesn't yet contain a complete
    // frame. Throws FramingError on unrecoverable corruption.
    bool next(MessageType& tag, std::vector<uint8_t>& payload);

    std::size_t buffered() const { return m_buf.size(); }

    // The largest frame this reader accepts; kMaxFrameLength unless a session
    // negotiated another.
    void setLimit(uint32_t limit) { m_limit = limit; }
    uint32_t limit() const { return m_limit; }

private:
    std::vector<uint8_t> m_buf;
    uint32_t m_limit = kMaxFrameLength;
};

} // namespace logos::plain

#endif // LOGOS_PLAIN_RPC_FRAMING_H
