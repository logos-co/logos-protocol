// Teardown race in RpcConnection<Stream>::fail().
//
// fail() used to close the socket on whatever thread called it, while every
// other access to the stream was serialized on the connection's strand
// (start()/writeFrame() post onto it; doRead()/doWrite() complete through
// bind_executor(m_strand, …)). A strand serializes *handlers* — it does
// nothing about a raw call made from outside it, and asio sockets are
// documented as unsafe for concurrent use.
//
// The shape below is the one that crashed in production: a connection with
// frames still queued is stopped from a thread other than the io worker (the
// CLI's ~RpcClient → ~PlainTransportConnection → stop() → fail() path) while
// that worker is inside reactive_socket_service_base::start_op() initiating
// the async_write for a frame writeFrame() had just posted. start_op() holds
// `descriptor_data` as a *reference* to impl.reactor_data_, which close()'s
// cleanup_descriptor_data() nulls between the null check and the shutdown_
// read — SIGSEGV at +0x98 on the io thread.
//
// A connected AF_UNIX pair stands in for the TCP socket: it reaches the same
// reactor (start_op is in reactive_socket_service_base, shared by every
// protocol) while costing no ephemeral ports, no accept backlog and no
// TIME_WAIT, so the loop can run thousands of cycles without the resource
// exhaustion that would make it flaky for reasons unrelated to the race.
//
// This is a probabilistic detector, not a deterministic one: the window is a
// few instructions wide. Pre-fix it takes the test binary down with a SIGSEGV
// often enough to catch a regression across repeated CI runs; post-fix the
// close is dispatched onto the strand and cannot overlap a write initiation
// at all.
//
// The same loop doubles as the leak/hang guard for that change: closing
// asynchronously must not strand a descriptor (the fd count is asserted flat
// across the cycles) and must not block teardown (the loop is timed).

#include <gtest/gtest.h>

#include "io_context_pool.h"
#include "json_codec.h"
#include "rpc_connection.h"
#include "rpc_message.h"

#include <boost/asio/local/connect_pair.hpp>
#include <boost/asio/local/stream_protocol.hpp>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#  include <fcntl.h>
#  include <sys/resource.h>
#  include <unistd.h>
#endif

using namespace logos::plain;

namespace {

using LocalSocket     = boost::asio::local::stream_protocol::socket;
using LocalConnection = RpcConnection<LocalSocket>;

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

// The loop deliberately closes each pair's peer while the io worker may still
// be draining that connection's queued frames, so a write can land on a socket
// whose other end is gone. On a socketpair that raises SIGPIPE and kills the
// process — asio sets SO_NOSIGPIPE on sockets it creates with socket(), but
// not on the pair socketpair() hands back. Ignore it for the duration, then
// put the previous disposition back so no other test inherits the change.
class SigPipeGuard {
public:
    SigPipeGuard()  : m_prev(std::signal(SIGPIPE, SIG_IGN)) {}
    ~SigPipeGuard() { std::signal(SIGPIPE, m_prev); }
private:
    void (*m_prev)(int);
};

}  // namespace

// Stop a connection that still has frames queued, from a thread that is not
// the io worker, many times over. Any overlap between fail()'s close and the
// strand's write initiation takes the process down.
TEST(RpcConnectionTeardownTest, StopWhileWritesAreInFlight)
{
    constexpr int kCycles = 3000;

    SigPipeGuard noSigPipe;
    auto& ioc = IoContextPool::shared().ioContext();
    auto codec = std::make_shared<JsonCodec>();

    const int fdsBefore = openFdCount();
    const auto t0 = std::chrono::steady_clock::now();

    for (int i = 0; i < kCycles; ++i) {
        LocalSocket mine(ioc);
        LocalSocket peer(ioc);
        boost::system::error_code ec;
        boost::asio::local::connect_pair(mine, peer, ec);
        ASSERT_FALSE(ec) << "connect_pair failed: " << ec.message();

        auto conn = std::make_shared<LocalConnection>(std::move(mine), codec, nullptr);
        conn->start();

        // Queue frames. Each writeFrame() posts onto the strand, so the io
        // worker is initiating an async_write at roughly the moment the stop()
        // below runs on this thread — the production window.
        for (int k = 0; k < 8; ++k) {
            EventMessage evt;
            evt.object = "teardown_probe";
            evt.eventName = "tick";
            evt.data.push_back(RpcValue{static_cast<int64_t>(k)});
            conn->sendEvent(evt);
        }

        // Sweep the offset between "the io worker picked the frame up" and
        // "this thread closes". Stopping instantly every time mostly beats the
        // worker to the socket and never lands in the window; the sweep walks
        // the close across the microseconds the worker spends initiating the
        // write, which is where the production crash lives.
        std::this_thread::sleep_for(std::chrono::microseconds(i % 50));

        conn->stop("test teardown");
        // stop() is idempotent; calling it again must not double-close.
        conn->stop("test teardown again");
        EXPECT_FALSE(conn->isOpen());
        conn.reset();
    }

    const auto elapsed = std::chrono::steady_clock::now() - t0;

    // Teardown must not hang: the close is dispatched, never waited on.
    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 60)
        << "teardown loop took far longer than the work it does — a close that "
           "blocks or waits on the io thread would show up here";

    // Give the strand a moment to drain the last dispatched close before
    // counting, then assert the descriptors actually came back.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const int fdsAfter = openFdCount();
    if (fdsBefore >= 0 && fdsAfter >= 0) {
        EXPECT_LE(fdsAfter, fdsBefore + 8)
            << kCycles << " connect/stop cycles leaked descriptors: "
            << fdsBefore << " -> " << fdsAfter;
    }
}
