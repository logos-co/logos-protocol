// RpcServerTcp::stop() and the calls it was serving.
//
// stop() failed every connection at once: a call still running lost its reply,
// which writeFrame() dropped on the stopped connection, and a reply still being
// written was cut off mid-frame by the close. It is the shape 3c635eb removed
// from the local server, where `daemon stop` at zero grace lost its reply. Each
// test here fails with the old stop() and passes once stop() lets started calls
// answer and queued frames leave, for at most a second, before it closes.

#include <gtest/gtest.h>

#include "incoming_call_handler.h"
#include "io_context_pool.h"
#include "json_codec.h"
#include "rpc_connection.h"
#include "rpc_framing.h"
#include "rpc_message.h"
#include "rpc_server.h"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace logos::plain;

namespace {

// Answers every call from a thread of its own, after `delay`, as a provider
// whose methods take a while does. `started` resolves once a call has begun.
class SlowHandler : public IncomingCallHandler {
public:
    explicit SlowHandler(std::chrono::milliseconds delay, std::string value)
        : m_delay(delay), m_value(std::move(value)) {}
    ~SlowHandler() override
    {
        for (auto& t : m_threads) t.join();
    }

    void onCall(const CallMessage& req, CallReply reply) override
    {
        if (!m_startedSet.exchange(true)) m_started.set_value();
        m_threads.emplace_back([this, id = req.id, reply = std::move(reply)] {
            std::this_thread::sleep_for(m_delay);
            ResultMessage r;
            r.id = id;
            r.ok = true;
            r.value = RpcValue{m_value};
            reply(std::move(r));
        });
    }
    void onMethods(const MethodsMessage&, MethodsReply) override {}
    void onSubscribe(const SubscribeMessage&, EventSink, const void*) override {}
    void onUnsubscribe(const UnsubscribeMessage&, const void*) override {}
    void onConnectionClosed(const void*) override {}
    void onToken(const TokenMessage&) override {}

    std::future<void> started() { return m_started.get_future(); }

private:
    std::chrono::milliseconds m_delay;
    std::string m_value;
    std::promise<void> m_started;
    std::atomic<bool> m_startedSet{false};
    std::vector<std::thread> m_threads;
};

// `receiveBuffer` > 0 shrinks the socket's receive buffer, so the kernel holds
// less of what the server writes and the rest waits in the server's queue.
boost::asio::ip::tcp::socket connectTo(uint16_t port, int receiveBuffer = 0)
{
    boost::asio::ip::tcp::socket sock(IoContextPool::shared().ioContext());
    sock.open(boost::asio::ip::tcp::v4());
    if (receiveBuffer > 0)
        sock.set_option(boost::asio::socket_base::receive_buffer_size(receiveBuffer));
    sock.connect({boost::asio::ip::make_address("127.0.0.1"), port});
    return sock;
}

}  // namespace

// A call running when stop() begins still answers, and stop() waits for it.
TEST(RpcServerStopDrainTest, ACallStillRunningWhenStopBeginsIsAnswered)
{
    auto codec = std::make_shared<JsonCodec>();
    SlowHandler handler(std::chrono::milliseconds(200), "done");
    auto started = handler.started();
    auto server = std::make_shared<RpcServerTcp>(
        IoContextPool::shared().ioContext(), "127.0.0.1", 0, codec, &handler);
    ASSERT_TRUE(server->start());

    auto client = std::make_shared<RpcConnection<TcpStream>>(
        connectTo(server->boundPort()), codec, nullptr);
    client->start();
    CallMessage call;
    call.id = client->nextId();
    call.object = "probe";
    call.method = "slow";
    auto answer = client->sendCall(call);
    ASSERT_EQ(started.wait_for(std::chrono::seconds(10)), std::future_status::ready);

    const auto t0 = std::chrono::steady_clock::now();
    server->stop();
    const auto stopped = std::chrono::steady_clock::now() - t0;

    ASSERT_EQ(answer.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    const ResultMessage res = answer.get();
    EXPECT_TRUE(res.ok) << "the running call lost its reply: " << res.err;
    EXPECT_LT(stopped, std::chrono::seconds(2)) << "stop() is bounded";
    client->stop();
}

// A reply still being written when stop() begins arrives whole. The peer reads
// nothing more until stop() has begun, then reads as fast as it can: a paced
// reader would be at the mercy of the timer resolution (15 ms on Windows).
TEST(RpcServerStopDrainTest, AReplyStillBeingWrittenArrivesWhole)
{
    // Past what the kernel buffers on either side, and under kMaxFrameLength.
    constexpr std::size_t kValueBytes = 15u << 20;
    auto codec = std::make_shared<JsonCodec>();
    SlowHandler handler(std::chrono::milliseconds(0), std::string(kValueBytes, 'x'));
    auto server = std::make_shared<RpcServerTcp>(
        IoContextPool::shared().ioContext(), "127.0.0.1", 0, codec, &handler);
    ASSERT_TRUE(server->start());

    auto sock = connectTo(server->boundPort(), 64 << 10);
    CallMessage call;
    call.id = 1;
    call.object = "probe";
    call.method = "big";
    boost::asio::write(sock, boost::asio::buffer(encodeFrame(*codec, AnyMessage{call})));

    // The reply has started: its first bytes are here, the rest is queued.
    std::vector<uint8_t> buffer(64u << 10);
    FrameReader reader;
    std::size_t received = sock.read_some(boost::asio::buffer(buffer));
    reader.append(buffer.data(), received);

    std::thread stopper([server] { server->stop(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // The frame must complete before the socket closes.
    MessageType tag{};
    std::vector<uint8_t> payload;
    bool whole = reader.next(tag, payload);
    boost::system::error_code ec;
    while (!whole) {
        const std::size_t n = sock.read_some(boost::asio::buffer(buffer), ec);
        if (ec) break;
        received += n;
        reader.append(buffer.data(), n);
        whole = reader.next(tag, payload);
    }
    stopper.join();

    ASSERT_TRUE(whole) << "the socket closed after " << received
                       << " bytes, in the middle of the reply (" << ec.message() << ")";
    const AnyMessage decoded = codec->decode(tag, payload.data(), payload.size());
    ASSERT_TRUE(std::holds_alternative<ResultMessage>(decoded));
    const auto& res = std::get<ResultMessage>(decoded);
    EXPECT_TRUE(res.ok);
    ASSERT_TRUE(std::holds_alternative<std::string>(res.value.value));
    EXPECT_EQ(std::get<std::string>(res.value.value).size(), kValueBytes);
}
