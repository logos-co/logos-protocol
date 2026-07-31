// Teardown race in RpcServerTcp::stop() / RpcServerSsl::stop() — the acceptor
// half of the one test_rpc_connection_teardown.cpp covers for the socket.
//
// stop() used to close m_acceptor on whatever thread called it (in practice the
// host thread, via ~PlainTransportHost), while doAccept() re-armed
// m_acceptor.async_accept() from inside its own completion handler — i.e. on the
// io worker thread. Neither side was serialized on anything. Asio acceptors are
// documented "Shared objects: Unsafe" exactly like sockets, and close() runs the
// same cleanup_descriptor_data() that nulls impl.reactor_data_ while
// reactive_socket_service_base::start_op() holds it by reference: the
// `if (!descriptor_data)` guard passes, the mutex is taken, and the following
// dereference reads through the null the close just stored → SIGSEGV at +0x98 on
// the io thread.
//
// The loop below is the shape that produces it: a server whose accept loop is
// live (real clients connecting) is stopped from the thread that is not the io
// worker, with the stop swept across the microseconds that worker spends
// initiating the next accept.
//
// It is deliberately single-threaded. Running several of these loops in
// parallel against the one shared io worker sounds like more pressure but is
// strictly worse: the crash needs a close to overlap an accept initiation *on
// the same acceptor*, and with N loops competing the worker is usually busy in
// some other server's accept path when a close lands. Measured, the parallel
// variant stopped reproducing at all.
//
// This is a probabilistic detector, not a deterministic one — the window is a
// few instructions wide. Interleaved A/B on macOS/arm64, 10 runs per arm, the
// two builds differing only in rpc_server.{h,cpp}: unpatched 3/10 runs died
// with the signature above, patched 0/10. CI runs this binary four times per
// PR (two platforms x the check phase plus the explicit run), so a regression
// has a good chance of being caught, but a single green run is not proof.
// Post-fix the close is dispatched onto the server's strand and cannot overlap
// an accept initiation at all.
//
// The same loop doubles as the leak/hang guard for that change: deferring the
// close must not strand a descriptor (the fd count is asserted flat across the
// cycles) and must not block teardown (the loop is timed).
//
// Sizing is bounded by TIME_WAIT, not by how long the race takes to hit. Every
// cycle leaves its connections there for 2*MSL, and a few thousand of those
// saturate the ephemeral port range — at which point connects start failing and
// slowing down, the sweep desyncs, and the test would flake for reasons that
// have nothing to do with the race. 1000 servers x 4 clients is about a third
// of the range on the tighter of the two platforms and stays fast.

#include <gtest/gtest.h>

#include "incoming_call_handler.h"
#include "io_context_pool.h"
#include "json_codec.h"
#include "rpc_connection.h"
#include "rpc_message.h"
#include "rpc_server.h"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#  include <fcntl.h>
#  include <sys/resource.h>
#  include <unistd.h>
#endif

using namespace logos::plain;

namespace {

// Number of open file descriptors held by this process, or -1 when the
// platform doesn't let us ask. Used only as a monotonic leak signal.
int openFdCount()
{
#if defined(__unix__) || defined(__APPLE__)
    struct rlimit rl{};
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return -1;
    // Cap the probe: a soft limit of RLIM_INFINITY would otherwise loop forever.
    const long cap = (rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur > 4096)
                         ? 4096
                         : static_cast<long>(rl.rlim_cur);
    int n = 0;
    for (long fd = 0; fd < cap; ++fd)
        if (fcntl(static_cast<int>(fd), F_GETFD) != -1) ++n;
    return n;
#else
    return -1;
#endif
}

// Clients are dropped mid-conversation all through the loop, so a server-side
// write can land on a socket whose peer is already gone. Ignore SIGPIPE for the
// duration and restore the previous disposition so no other test inherits it.
class SigPipeGuard {
public:
    SigPipeGuard()  : m_prev(std::signal(SIGPIPE, SIG_IGN)) {}
    ~SigPipeGuard() { std::signal(SIGPIPE, m_prev); }
private:
    void (*m_prev)(int);
};

// Minimal provider: answers Call with the method name it was given. Enough to
// prove an accepted connection is wired end to end.
class EchoHandler : public IncomingCallHandler {
public:
    void onCall(const CallMessage& req, CallReply reply) override
    {
        ResultMessage r;
        r.id = req.id;
        r.ok = true;
        r.value = RpcValue{req.method};
        reply(std::move(r));
    }
    void onMethods(const MethodsMessage& req, MethodsReply reply) override
    {
        MethodsResultMessage r;
        r.id = req.id;
        r.ok = true;
        reply(std::move(r));
    }
    void onSubscribe(const SubscribeMessage&, EventSink, const void*) override {}
    void onUnsubscribe(const UnsubscribeMessage&, const void*) override {}
    void onConnectionClosed(const void*) override {}
    void onToken(const TokenMessage&) override {}
};

// Connect a plain TCP socket to 127.0.0.1:port. Synchronous: the kernel
// completes it out of the listen backlog, so it returns without the io thread
// having accepted anything yet.
boost::asio::ip::tcp::socket connectTo(boost::asio::io_context& ioc,
                                       uint16_t port,
                                       boost::system::error_code& ec)
{
    boost::asio::ip::tcp::socket sock(ioc);
    boost::asio::ip::tcp::endpoint ep(
        boost::asio::ip::make_address("127.0.0.1"), port);
    sock.connect(ep, ec);
    return sock;
}

// Busy-wait for `ns` nanoseconds. sleep_for() cannot sweep this window: the
// shortest sleep the scheduler actually delivers is on the order of a
// millisecond, which parks the close long after the io worker has finished with
// the accept and gone back to waiting on the reactor — i.e. it lands everywhere
// except in the window. (With a sleep-based sweep this test passed 10/10 runs
// against the unpatched server — it never reproduced at all. Spinning is what
// made it a detector.)
void spinFor(long ns)
{
    if (ns <= 0) return;
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::nanoseconds(ns);
    while (std::chrono::steady_clock::now() < deadline) { /* spin */ }
}

}  // namespace

// Stop a server whose accept loop is live, from a thread that is not the io
// worker, many times over. Any overlap between stop()'s acceptor close and the
// accept handler's re-arm takes the process down.
TEST(RpcServerTeardownTest, StopWhileAcceptsAreInFlight)
{
    constexpr int kCycles  = 1000;
    constexpr int kClients = 4;

    SigPipeGuard noSigPipe;
    auto& ioc = IoContextPool::shared().ioContext();
    auto codec = std::make_shared<JsonCodec>();
    EchoHandler handler;

    const int fdsBefore = openFdCount();
    const auto t0 = std::chrono::steady_clock::now();

    int connected = 0;
    int bindFailures = 0;

    for (int i = 0; i < kCycles; ++i) {
        auto server = std::make_shared<RpcServerTcp>(
            ioc, "127.0.0.1", /*port=*/0, codec, &handler);
        if (!server->start()) { ++bindFailures; continue; }
        const uint16_t port = server->boundPort();
        if (port == 0) { ++bindFailures; continue; }

        // Fill the backlog. Each accepted socket makes the io worker construct a
        // connection and re-arm async_accept — that re-arm is what the stop()
        // below races.
        std::vector<boost::asio::ip::tcp::socket> clients;
        clients.reserve(kClients);
        for (int k = 0; k < kClients; ++k) {
            boost::system::error_code ec;
            auto sock = connectTo(ioc, port, ec);
            if (ec) continue;  // port pressure, not the thing under test
            ++connected;
            clients.push_back(std::move(sock));
        }

        // Sweep the offset between "the io worker picked an accept up" and "this
        // thread closes the acceptor". The connects above hand the worker a burst
        // of completions; it is somewhere inside that burst — accepting,
        // constructing a connection, re-arming — for the next tens of
        // microseconds. Walk the close across that span in sub-microsecond steps.
        spinFor(static_cast<long>(i % 160) * 250);

        server->stop();
        // stop() is idempotent; calling it again must not double-close.
        server->stop();
        server.reset();
    }

    const auto elapsed = std::chrono::steady_clock::now() - t0;

    // Thresholds, not equalities: a busy port table can legitimately refuse the
    // odd bind or connect, and that is not what this test is about. They only
    // have to be tight enough to catch "the loop never actually ran".
    EXPECT_LT(bindFailures, kCycles / 20)
        << bindFailures << " of " << kCycles << " servers failed to bind 127.0.0.1:0";
    EXPECT_GT(connected, kCycles)
        << "only " << connected << " clients connected across " << kCycles
        << " cycles — too few accepts to have raced anything";

    // Teardown must not hang: the close is dispatched, never waited on.
    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 120)
        << "teardown loop took far longer than the work it does — a close that "
           "blocks or waits on the io thread would show up here";

    // Give the strand a moment to drain the last dispatched close before
    // counting, then assert the descriptors actually came back.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const int fdsAfter = openFdCount();
    if (fdsBefore >= 0 && fdsAfter >= 0) {
        EXPECT_LE(fdsAfter, fdsBefore + 8)
            << kCycles << " start/stop cycles leaked descriptors: "
            << fdsBefore << " -> " << fdsAfter;
    }
}

// The first accept is armed on the strand rather than inline in start(), so it
// may not be armed yet when start() returns. That must be invisible: listen()
// has already run, so a client connecting immediately is held in the backlog and
// served as soon as the strand gets there.
TEST(RpcServerTeardownTest, ServesAClientThatConnectsTheInstantStartReturns)
{
    auto& ioc = IoContextPool::shared().ioContext();
    auto codec = std::make_shared<JsonCodec>();
    EchoHandler handler;

    auto server = std::make_shared<RpcServerTcp>(
        ioc, "127.0.0.1", /*port=*/0, codec, &handler);
    ASSERT_TRUE(server->start());
    ASSERT_NE(server->boundPort(), 0)
        << "start() must publish the bound port before it returns";

    boost::system::error_code ec;
    auto sock = connectTo(ioc, server->boundPort(), ec);
    ASSERT_FALSE(ec) << "connect failed: " << ec.message();

    auto client = std::make_shared<RpcConnection<TcpStream>>(
        std::move(sock), codec, nullptr);
    client->start();

    CallMessage call;
    call.id = client->nextId();
    call.object = "probe";
    call.method = "ping";
    auto fut = client->sendCall(call);

    ASSERT_EQ(fut.wait_for(std::chrono::seconds(10)), std::future_status::ready)
        << "the server never answered — the accept loop was not armed";
    const ResultMessage res = fut.get();
    EXPECT_TRUE(res.ok) << res.err;
    ASSERT_TRUE(std::holds_alternative<std::string>(res.value.value));
    EXPECT_EQ(std::get<std::string>(res.value.value), "ping");

    client->stop();
    server->stop();
}
