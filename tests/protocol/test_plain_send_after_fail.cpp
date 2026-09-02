// A CALL REGISTERED AS THE CONNECTION FAILS — the one send that was never
// answered at all.
//
// sendCallAsync() reads m_stopped and THEN registers its handler under m_mu.
// fail() flips m_stopped and THEN sweeps the pending map under the same mutex.
// Those two orders are opposed, so there is an interleaving in which nobody
// answers the call:
//
//     caller                              fail()
//     ------------------------------      -------------------------------
//     m_stopped.load()  -> false
//                                         CAS m_stopped -> true
//                                         lock(m_mu); swap(m_pendingCalls)
//                                           ... the map is EMPTY ...
//                                         unlock(m_mu)
//     lock(m_mu); m_pendingCalls[id] = h
//     writeFrame()  -> drops, stopped
//
// The handler is now parked in the pending map of a connection that has already
// been torn down. Nothing will ever take it out: fail() runs once and has been,
// no reply can arrive on a closed socket, and the frame was never written. The
// caller is not told anything.
//
// WHAT THAT COSTS THE CALLER, which is the part worth being precise about,
// because "a call is lost" understates it. Every caller of this function has a
// deadline, and the deadline is what answers instead:
//
//   * callMethodAsyncWithError arms a timer before sending, so the user's
//     callback fires — after the FULL timeoutMs — with code "timeout". A
//     connection that is provably gone is reported as a peer that was merely
//     slow, and callers key their retry / re-acquire behaviour off that code.
//   * callMethodWithError blocks its own thread on the future for the whole
//     timeout and reports the same wrong thing.
//   * getMethods() has no caller-supplied timeout at all: it waits out a
//     hard-coded 5 seconds and returns an empty method list.
//
// This predates the io_context fold — master has the identical shape on the
// promise-based path — so it is not a regression of #46; #46's cancelPending()
// only made the orphaned entry self-cleaning rather than permanent.
//
// ── HOW THE INTERLEAVING IS BUILT, rather than waited for ────────────────────
//
// The window is a handful of instructions wide, so these tests place the two
// threads in it instead of racing for it. m_mu is the lever: the test takes the
// connection's own mutex (through the explicit-instantiation access hole
// test_iofold.cpp already uses to read the pending map), which parks the caller
// AFTER its m_stopped check and BEFORE its registration — exactly the gap. The
// test then drops the mutex and calls stop() from the hot thread, while the
// caller is still coming back from a futex wait, so fail() usually reaches the
// mutex first and sweeps a map the caller has not written to yet.
//
// That last handoff is the one part these tests do not control, so nothing is
// asserted about a single round: each runs many and asserts on the aggregate.
// It leans on std::mutex BARGING — a hot thread taking a just-released mutex
// ahead of a waiter the kernel is still waking — which is how both libc++ on
// Darwin (measured: 39-40 of 40 rounds) and glibc's default non-PI mutex behave,
// neither of which promises it. An implementation that instead handed ownership
// straight to the queued waiter would reach the interleaving in NO round, and
// the "was never reached" assertions below are there so that shows up as a
// failure that says exactly that, rather than as four quietly vacuous passes.
//
// Every test asserts that the target interleaving was actually reached at least
// once. Post-fix it is visible in what the call reports — the reclaim answers
// with TRANSPORT_CLOSED / "connection stopped", and that answer is unreachable
// in these rounds by any other route, because the caller demonstrably read
// m_stopped as FALSE (it was parked in the gap before stop() was called at all)
// and so cannot have taken the pre-existing already-stopped early-out. A run in
// which the interleaving was never reached fails, rather than passing quietly.
//
// ── VALIDATED AGAINST THE PRE-FIX TREE ───────────────────────────────────────
//
// Per tests/protocol/CMakeLists.txt: a detector is checked by running it on the
// code the fix replaced — a checkout of commit cf1b9b0, the head of #46 — and
// not against a switch, a build option or a getenv() probe in this tree. The
// numbers those runs produced on an aarch64-darwin box are recorded on each
// test below. Summarised: the parking trick reaches the target interleaving in
// 39-40 of 40 rounds, all four tests are RED on cf1b9b0, and they take 159
// seconds there against 9 here — almost all of the difference is callers
// sitting out deadlines that had already been decided.
//
// WHICH OF THESE ARE DETECTORS OF WHAT, because it is not uniform:
//
//   * tests 1-3 detect the DROPPED call. Each is red on cf1b9b0 by a wide
//     margin (40/40 calls never answered; 40/40 answered as "timeout" at
//     828ms; a 5049ms getMethods).
//   * test 4 detects it only weakly — the unaided race is a few instructions
//     wide, and cf1b9b0 loses 14 calls in 10,000. What test 4 is a strong
//     detector of is the DOUBLE, which is the failure the fix could newly
//     introduce; see the note on it.

#include <gtest/gtest.h>

#include "incoming_call_handler.h"
#include "json_codec.h"
#include "logos_call_error.h"
#include "plain_logos_object.h"
#include "rpc_connection.h"
#include "rpc_message.h"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/local/connect_pair.hpp>
#include <boost/asio/local/stream_protocol.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QVariant>
#include <QVariantList>

#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace logos::plain;

namespace {

using LocalSocket = boost::asio::local::stream_protocol::socket;
using LocalConn   = RpcConnection<LocalSocket>;

// Reads the connection's own mutex and pending maps through the
// explicit-instantiation access hole ([temp.spec] does not check access on the
// template arguments of an explicit instantiation), so the code under test is
// observed — and stalled — exactly as it ships: no friend, no test-only hook,
// no `#define private public`. test_iofold.cpp uses the same idiom to size the
// pending map.
template <typename Tag, typename Tag::type Member>
struct Rob {
    friend typename Tag::type get(Tag) { return Member; }
};

struct ConnMuTag {
    using type = std::mutex LocalConn::*;
    friend type get(ConnMuTag);
};
template struct Rob<ConnMuTag, &LocalConn::m_mu>;

struct PendingCallsTag {
    using type = std::map<std::uint64_t, RpcConnectionBase::ResultHandler> LocalConn::*;
    friend type get(PendingCallsTag);
};
template struct Rob<PendingCallsTag, &LocalConn::m_pendingCalls>;

struct PendingMethodsTag {
    using type = std::map<std::uint64_t,
                          std::shared_ptr<std::promise<MethodsResultMessage>>> LocalConn::*;
    friend type get(PendingMethodsTag);
};
template struct Rob<PendingMethodsTag, &LocalConn::m_pendingMethods>;

// A provider on the far end of the socketpair that answers everything at once.
// It exists so the warm-up call in the object-level test completes for real;
// the racing call never reaches it, because the connection dies first.
//
// It can also be told to HOLD its replies. That is what lets the volume test
// below guarantee — rather than hope — that fail()'s sweep has something to
// sweep at the moment it runs.
class EagerProvider : public IncomingCallHandler {
public:
    void onCall(const CallMessage& req, CallReply reply) override
    {
        if (m_hold.load()) {
            std::lock_guard<std::mutex> g(m_mu);
            m_held.push_back(std::move(reply));
            return;
        }
        ResultMessage res;
        res.id = req.id;
        res.ok = true;
        res.value = RpcValue{static_cast<int64_t>(1)};
        reply(std::move(res));
    }

    void hold(bool on) { m_hold.store(on); }
    void dropHeld()
    {
        std::vector<CallReply> gone;
        {
            std::lock_guard<std::mutex> g(m_mu);
            gone.swap(m_held);
        }
        // Destroyed here, outside the lock: each closure holds a share of its
        // connection, so this is also what lets that connection die.
    }
    void onMethods(const MethodsMessage& req, MethodsReply reply) override
    {
        MethodsResultMessage r; r.id = req.id; r.ok = true; reply(std::move(r));
    }
    void onSubscribe(const SubscribeMessage&, EventSink, const void*) override {}
    void onUnsubscribe(const UnsubscribeMessage&, const void*) override {}
    void onConnectionClosed(const void*) override {}
    void onToken(const TokenMessage&) override {}

private:
    std::atomic<bool>      m_hold{false};
    std::mutex             m_mu;
    std::vector<CallReply> m_held;
};

// One io_context and worker thread, shared by every connection a test builds.
class IoWorker {
public:
    IoWorker()
        : m_guard(boost::asio::make_work_guard(m_ioc))
        , m_thread([this] { m_ioc.run(); })
    {}
    ~IoWorker()
    {
        m_guard.reset();
        m_ioc.stop();
        if (m_thread.joinable()) m_thread.join();
    }
    boost::asio::io_context& ioc() { return m_ioc; }

private:
    boost::asio::io_context m_ioc;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> m_guard;
    std::thread m_thread;
};

// A live client/provider pair over a connected socketpair. Fresh per round,
// because each round kills its connection.
struct Wire {
    std::shared_ptr<LocalConn> client;
    std::shared_ptr<LocalConn> provider;
};

// A HARNESS DETAIL WITH NOTHING TO SAY ABOUT THE PRODUCT, recorded so nobody
// reads it as one. On Darwin a write to a socket whose peer has closed raises
// SIGPIPE, and the default disposition kills the process — which is what a
// provider still answering a burst does the instant the client half is stopped
// (observed: one run in six died with signal 13 before this).
//
// It is specific to connect_pair: asio sets SO_NOSIGPIPE itself in
// socket_ops::socket() and socket_ops::accept(), which is how every socket in
// the shipped transports is created, but socketpair() descriptors are handed to
// basic_socket::assign() and assign() does not. (On Linux the question does not
// arise — asio passes MSG_NOSIGNAL on every send.) So this sets the option asio
// would have set, rather than touching the process-wide signal disposition out
// from under the rest of the suite.
void suppressSigpipe(LocalSocket& s)
{
#ifdef SO_NOSIGPIPE
    int on = 1;
    ::setsockopt(s.native_handle(), SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)s;
#endif
}

Wire makeWire(boost::asio::io_context& ioc, IncomingCallHandler* handler)
{
    LocalSocket a(ioc), b(ioc);
    boost::system::error_code ec;
    boost::asio::local::connect_pair(a, b, ec);
    if (ec) return {};
    suppressSigpipe(a);
    suppressSigpipe(b);
    auto codec = std::make_shared<JsonCodec>();
    Wire w;
    w.client   = std::make_shared<LocalConn>(std::move(a), codec, nullptr);
    w.provider = std::make_shared<LocalConn>(std::move(b), codec, handler);
    w.client->start();
    w.provider->start();
    return w;
}

// Everything a result handler touches, SHARED-OWNED. A handler that loses its
// race still runs — on the io thread, at a moment this test does not control —
// so nothing it reads may live in a round's stack frame.
struct Outcome {
    std::atomic<int> calls{0};
    std::mutex       mu;
    std::string      errCode;
    std::string      errText;
    logos::CallError err;
};

QCoreApplication* ensureApp()
{
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance()) new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

void pump(int ms)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

void pumpUntil(std::atomic<int>& counter, int target, int budgetMs)
{
    QElapsedTimer t; t.start();
    while (counter.load() < target && t.elapsed() < budgetMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

// Park `work` in sendCallAsync's gap, then let fail() through ahead of it.
//
// On entry the connection's mutex is taken, so `work` — which must be something
// that ends in a registration under that mutex — runs its m_stopped check
// (false: the connection is live) and then blocks. Dropping the mutex and
// calling stop() from THIS thread, which is hot and is not returning from a
// futex wait, is what usually gets fail() to the mutex first.
//
// Returns once both halves are done. Whether the target order was reached is
// decided by the caller, from what the call reported.
void raceRegistrationAgainstFail(const std::shared_ptr<LocalConn>& conn,
                                 std::function<void()> work)
{
    std::mutex& mu = conn.get()->*get(ConnMuTag{});
    std::unique_lock<std::mutex> hold(mu);

    std::thread caller(std::move(work));
    // Long enough that the caller is provably parked ON THE MUTEX: it cannot
    // have got past the registration, because this thread holds it.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    hold.unlock();
    conn->stop("peer vanished");

    caller.join();
}

size_t pendingCalls(const std::shared_ptr<LocalConn>& conn)
{
    std::lock_guard<std::mutex> g(conn.get()->*get(ConnMuTag{}));
    return (conn.get()->*get(PendingCallsTag{})).size();
}

size_t pendingMethods(const std::shared_ptr<LocalConn>& conn)
{
    std::lock_guard<std::mutex> g(conn.get()->*get(ConnMuTag{}));
    return (conn.get()->*get(PendingMethodsTag{})).size();
}

const char* kToken = "tok";

} // namespace

class PlainSendAfterFailTest : public ::testing::Test {
protected:
    void SetUp() override { ensureApp(); }
};

// ── 1. the transport-level claim: the handler is invoked, once, always ───────
//
// Straight at RpcConnection, with no PlainLogosObject above it, so what is
// measured is the registration itself rather than anything the handle does to
// compensate for it.
//
// PRE-FIX (cf1b9b0), 40 rounds: ALL 40 reached the interleaving and in all 40
// the handler was NEVER INVOKED, each leaving its registration parked in the
// pending map of a connection that had already been torn down (unanswered=40,
// leaked=40). POST-FIX: 39-40 answered by the reclaim, unanswered 0, leaked 0.
TEST_F(PlainSendAfterFailTest, ACallRegisteredAsTheConnectionFailsIsStillAnswered)
{
    IoWorker io;
    EagerProvider provider;

    constexpr int kRounds = 40;
    int reached    = 0;   // fail() swept before the registration landed
    int sweptFirst = 0;   // the registration landed first; fail()'s sweep took it
    int unanswered = 0;   // nobody answered at all — the defect
    int doubled    = 0;
    int leaked     = 0;   // registration still parked on the dead connection

    for (int r = 0; r < kRounds; ++r) {
        Wire w = makeWire(io.ioc(), &provider);
        ASSERT_NE(w.client, nullptr);

        auto out = std::make_shared<Outcome>();
        const std::uint64_t id = w.client->nextId();
        auto client = w.client;

        raceRegistrationAgainstFail(client, [client, out, id] {
            CallMessage msg;
            msg.id = id; msg.object = "probe"; msg.method = "ping";
            client->sendCallAsync(std::move(msg), [out](ResultMessage res) {
                {
                    std::lock_guard<std::mutex> g(out->mu);
                    out->errCode = res.errCode;
                }
                out->calls.fetch_add(1);
            });
        });

        // Everything that could still answer this call has run: fail() is
        // complete (stop() returned) and the caller has returned from
        // sendCallAsync.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        const int n = out->calls.load();
        std::string seen;
        { std::lock_guard<std::mutex> g(out->mu); seen = out->errCode; }

        if (n == 0)     ++unanswered;
        else if (n > 1) ++doubled;
        if (seen == "TRANSPORT_CLOSED") ++reached;
        if (seen == "TRANSPORT_ERROR")  ++sweptFirst;

        leaked += static_cast<int>(pendingCalls(client));
        w.provider->stop();
    }

    std::cout << "  " << kRounds << " rounds parked in sendCallAsync's gap -> "
              << "answered-by-reclaim=" << reached
              << " answered-by-sweep=" << sweptFirst
              << " NEVER ANSWERED=" << unanswered
              << " doubled=" << doubled
              << " registrations left on a dead connection=" << leaked
              << std::endl;

    EXPECT_EQ(unanswered, 0)
        << unanswered << " of " << kRounds << " calls registered on a connection "
           "fail() had already swept, and were never answered by anything. The "
           "caller is left to its deadline.";
    EXPECT_EQ(doubled, 0) << "a handler was invoked more than once";
    EXPECT_EQ(leaked, 0)
        << "a handler is still parked in the pending map of a stopped connection";
    EXPECT_GT(reached, 0)
        << "the interleaving this test exists for was never reached in "
        << kRounds << " rounds — fail() always lost the mutex to the caller, so "
           "this run proved nothing. It is not evidence of a fix.";
}

// ── 2. what the CALLER is told, and how long it waits to hear it ────────────
//
// The same interleaving one layer up, through callMethodAsyncWithError, which
// is where the cost actually lands: the deadline is the only thing left that
// can resolve the call, so the caller waits it out in full and is then told
// "timeout" — a diagnosis that is not merely imprecise but names the wrong
// party, since the transport knew the connection was gone before the call was
// ever written.
//
// PRE-FIX (cf1b9b0), 40 rounds: all 40 were answered only by the deadline —
// worst 828ms, against the 800ms deadline that validation run used — with code
// "timeout". POST-FIX: 0 reported as a timeout, worst latency 35ms idle and
// 52ms under 8x CPU oversubscription, which is this test's own 30ms parking
// sleep plus the hop through the Qt loop.
//
// ── WHERE THE BARS ARE SET, after this failed once on a shared CI runner ─────
//
// Run 33637026342 (macos-latest) failed this test on a commit ubuntu-latest
// passed, and four consecutive local runs passed 559/559; nothing on that
// branch touches this path. The bars below are set for a contended runner
// WITHOUT moving the property, which is that the reclaim answers the call and
// the deadline does not:
//
//   * The DEADLINE is 2000ms, not 800ms. Post-fix no round reaches it — the
//     answer arrives ~35ms in — so its size is free, and only a run that is
//     already failing pays for it. What it buys is that a runner must stall for
//     two seconds, not eight hundred milliseconds, to manufacture the "timeout"
//     verdict this test exists to forbid. Pre-fix every round still burns it in
//     full, so re-validating against cf1b9b0 now costs ~80s here, not ~33s.
//   * PROMPTNESS is an absolute 400ms, not half the deadline. It bounds a 30ms
//     park plus a Qt hop, which the deadline has no bearing on; tied to the
//     deadline it would have silently slackened to 1000ms just now.
//   * Both bars tolerate 2 of 40 rounds, so one descheduled round is not a red
//     suite. Pre-fix ALL 40 miss them, leaving a 20x margin. Zero tolerance is
//     kept where it costs nothing: tests 1 and 4 assert unanswered/dropped == 0
//     on counts with no clock in them, and they are the detectors of the drop
//     itself.
//
// Measured under 8x oversubscription, none of the above is what moves: worst
// latency held at 36-52ms and timedOut at 0, while answered-by-reclaim fell
// from 40 to 18-31 as the mutex handoff lost more often. That counter is
// asserted only to be non-zero — the weakest form that is still not vacuous.
TEST_F(PlainSendAfterFailTest,
       TheCallerIsToldTheTransportClosedInsteadOfWaitingOutItsDeadline)
{
    IoWorker io;
    EagerProvider provider;

    constexpr int kRounds         = 40;
    constexpr int kTimeoutMs      = 2000;   // see WHERE THE BARS ARE SET, above
    constexpr int kPromptMs       = 400;    // answered by the transport, not the clock
    constexpr int kStallTolerance = 2;      // rounds a contended runner may stall

    int reached  = 0;
    int timedOut = 0;
    int missing  = 0;
    int doubled  = 0;
    std::vector<qint64> latencies;

    for (int r = 0; r < kRounds; ++r) {
        Wire w = makeWire(io.ioc(), &provider);
        ASSERT_NE(w.client, nullptr);

        auto* obj = new PlainLogosObject("probe_module", w.client);

        // ONE COMPLETED CALL FIRST, and it is load-bearing rather than tidy:
        // the first callMethodAsyncWithError on a handle also runs
        // ensureCompletionSub(), whose Subscribe takes the connection mutex.
        // Without the warm-up the racing caller would park on that mutex —
        // BEFORE its m_stopped check rather than after it — and the round would
        // exercise the pre-existing already-stopped early-out instead of the gap.
        {
            auto warm = std::make_shared<Outcome>();
            obj->callMethodAsyncWithError(kToken, QStringLiteral("ping"),
                                          QVariantList{}, 5000,
                                          [warm](QVariant, const logos::CallError&) {
                                              warm->calls.fetch_add(1);
                                          });
            pumpUntil(warm->calls, 1, 5000);
            ASSERT_EQ(warm->calls.load(), 1) << "the warm-up call was not answered";
        }

        auto out = std::make_shared<Outcome>();
        QElapsedTimer clock;
        clock.start();
        raceRegistrationAgainstFail(w.client, [obj, out] {
            obj->callMethodAsyncWithError(kToken, QStringLiteral("ping"),
                                          QVariantList{}, kTimeoutMs,
                                          [out](QVariant, const logos::CallError& e) {
                                              {
                                                  std::lock_guard<std::mutex> g(out->mu);
                                                  out->err = e;
                                              }
                                              out->calls.fetch_add(1);
                                          });
        });

        // Generous: pre-fix this needs the whole deadline plus the hop through
        // the Qt loop, and the test has to OBSERVE that rather than give up on it.
        pumpUntil(out->calls, 1, kTimeoutMs + 4000);
        const qint64 elapsed = clock.elapsed();
        pump(60);   // a second delivery would land here

        logos::CallError seen;
        { std::lock_guard<std::mutex> g(out->mu); seen = out->err; }

        if (out->calls.load() == 0)     ++missing;
        else if (out->calls.load() > 1) ++doubled;
        if (seen.code == "timeout") ++timedOut;
        // "connection stopped" is the reclaim's own wording; fail()'s sweep
        // reports its reason instead. Either is a prompt, honest answer — this
        // only distinguishes which half of the race ran.
        if (seen.message.find("connection stopped") != std::string::npos) ++reached;
        if (out->calls.load() == 1) latencies.push_back(elapsed);

        obj->release();
        w.provider->stop();
        pump(20);
    }

    std::sort(latencies.begin(), latencies.end());
    const qint64 worst  = latencies.empty() ? -1 : latencies.back();
    const qint64 median = latencies.empty() ? -1 : latencies[latencies.size() / 2];
    const int    slow   = static_cast<int>(std::count_if(
        latencies.begin(), latencies.end(),
        [](qint64 ms) { return ms >= kPromptMs; }));

    std::cout << "  " << kRounds << " rounds, " << kTimeoutMs
              << "ms deadline -> answered-by-reclaim=" << reached
              << " reported-as-TIMEOUT=" << timedOut
              << " never-delivered=" << missing << " doubled=" << doubled
              << " latency median=" << median << "ms worst=" << worst
              << "ms over-" << kPromptMs << "ms=" << slow << "/" << kRounds
              << std::endl;

    EXPECT_EQ(missing, 0);
    EXPECT_EQ(doubled, 0);
    EXPECT_LE(timedOut, kStallTolerance)
        << timedOut << " of " << kRounds << " calls waited out their entire "
        << kTimeoutMs << "ms deadline and were then reported as a TIMEOUT. The "
           "connection was already torn down when the call was made; the honest "
           "code is transport_error, and it was available immediately.";
    ASSERT_FALSE(latencies.empty());
    EXPECT_LE(slow, kStallTolerance)
        << slow << " of " << kRounds << " calls needed " << kPromptMs
        << "ms or more to be told the transport was gone (median " << median
        << "ms, worst " << worst << "ms). A healthy round is answered by the "
           "reclaim in about the 30ms this test parks the caller for, and the "
           "deadline plays no part in it.";
    EXPECT_GT(reached, 0)
        << "the interleaving this test exists for was never reached in "
        << kRounds << " rounds — this run proved nothing";
}

// ── 3. the same gap on the methods map, where there is no caller timeout ─────
//
// sendMethods() has the identical shape, and getMethods() above it waits on a
// hard-coded five-second future. A caller cannot shorten that, so the cost of
// losing this registration is a fixed five-second stall per introspection —
// which is what module discovery does on a connection that has just dropped.
//
// PRE-FIX (cf1b9b0), 25 rounds: 24 reached the interleaving and each blocked its
// caller for the full five seconds (worst 5049ms) before giving up with no
// answer, leaving 24 promises parked on dead connections. POST-FIX: worst wait
// 30-44ms, 0 promises left.
TEST_F(PlainSendAfterFailTest,
       GetMethodsDoesNotWaitOutItsFiveSecondFutureWhenTheConnectionFails)
{
    IoWorker io;
    EagerProvider provider;

    constexpr int kRounds = 25;
    int reached = 0;
    int leaked  = 0;
    std::vector<qint64> latencies;

    for (int r = 0; r < kRounds; ++r) {
        Wire w = makeWire(io.ioc(), &provider);
        ASSERT_NE(w.client, nullptr);

        auto client = w.client;
        auto elapsed = std::make_shared<std::atomic<qint64>>(-1);
        auto text    = std::make_shared<std::string>();

        raceRegistrationAgainstFail(client, [client, elapsed, text] {
            QElapsedTimer t; t.start();
            MethodsMessage msg;
            msg.id = client->nextId();
            msg.object = "probe_module";
            auto fut = client->sendMethods(std::move(msg));
            // The same five seconds the real getMethods() waits.
            if (fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready)
                *text = fut.get().err;
            elapsed->store(t.elapsed());
        });

        latencies.push_back(elapsed->load());
        if (*text == "connection stopped") ++reached;

        leaked += static_cast<int>(pendingMethods(client));
        w.provider->stop();
    }

    std::sort(latencies.begin(), latencies.end());
    const qint64 worst = latencies.back();
    std::cout << "  " << kRounds << " getMethods rounds -> answered-by-reclaim="
              << reached << " worst wait=" << worst
              << "ms, promises left on a dead connection=" << leaked << std::endl;

    EXPECT_LT(worst, 1000)
        << "a getMethods() waited " << worst << "ms — its whole hard-coded "
           "future timeout — because its promise was registered on a connection "
           "fail() had already swept";
    EXPECT_EQ(leaked, 0)
        << "a promise is still parked in the methods map of a stopped connection";
    EXPECT_GT(reached, 0) << "the interleaving was never reached; nothing proved";
}

// ── 4. exactly-once, at volume, on the path the fix adds ────────────────────
//
// THE THREE TESTS ABOVE ARE NOT DETECTORS OF EXACTLY-ONCE. Each resolves one
// call once, so each stays green whatever guards the handler — the same trap
// tests/protocol/CMakeLists.txt records for the fold's per-path tests. The fix
// adds a THIRD contender for a registered handler (the post-registration
// reclaim, alongside dispatchIncoming and fail()'s sweep), so the property that
// needs re-proving is that the extract-and-erase under m_mu still lets exactly
// one of them have it.
//
// So: 10,000 calls, registered from four threads, with stop() landing in the
// middle of the burst. Every call counts its own invocations. Both failures are
// counted separately, because they are different bugs — a handler invoked twice
// means the reclaim and the sweep both won, and a handler never invoked is the
// defect this file is about, measured at volume instead of one round at a time.
//
// VALIDATED AS A DOUBLE-DELIVERY DETECTOR, against the obvious spelling of the
// fix rather than an imaginary one: a throwaway checkout in which sendCallAsync
// COPIES its handler into the map and then delivers that copy without the
// extract-and-erase (the "I already have the handler, why look it up" version).
// It reports 4, 6, 7 and 8 doubles per 10,000 over four runs. Thrown away with
// the checkout; nothing in this tree switches it on.
//
// AND WHY IT IS NEEDED AT ALL, given the suite already has a 10,000-call
// release-race: IoFoldTest.ReleaseRacingRepliesInFlightDeliversEachCallOnce is
// BLIND to this path — measured, 0 doubles against the same broken reclaim —
// because it races teardown of the HANDLE against replies while the connection
// stays up, so sendCallAsync's stopped branch is never taken. The contended
// object has to be the connection.
//
// PRE-FIX (cf1b9b0): 14 of the 10,000 calls dropped, and 14 registrations left
// on dead connections — a real but weak signal, since the unaided window is only
// a few instructions wide (tests 1-3 are the wide detectors of the drop).
// POST-FIX: 0 dropped, 0 doubled, over 8 runs.
TEST_F(PlainSendAfterFailTest, AStopRacingABurstOfRegistrationsAnswersEveryCallExactlyOnce)
{
    IoWorker io;
    EagerProvider provider;

    constexpr int kRounds   = 10;
    constexpr int kWarm     = 40;
    constexpr int kThreads  = 4;
    constexpr int kPer      = 240;
    constexpr int kPerRound = kWarm + kThreads * kPer;   // 1,000
    constexpr int kTotal    = kRounds * kPerRound;       // 10,000

    int doubled  = 0;
    int dropped  = 0;
    int answered = 0;
    int byReply  = 0;
    int bySweep  = 0;
    int byClosed = 0;
    int leaked   = 0;

    for (int r = 0; r < kRounds; ++r) {
        Wire w = makeWire(io.ioc(), &provider);
        ASSERT_NE(w.client, nullptr);
        auto client = w.client;
        provider.hold(false);

        // Shared-owned for the same reason as Outcome above: a handler that
        // loses the race still runs, on a thread this loop does not join.
        auto counts = std::make_shared<std::vector<std::atomic<int>>>(kPerRound);
        auto reply  = std::make_shared<std::atomic<int>>(0);
        auto closed = std::make_shared<std::atomic<int>>(0);
        auto swept  = std::make_shared<std::atomic<int>>(0);
        auto issued = std::make_shared<std::atomic<int>>(0);

        auto send = [client, counts, reply, closed, swept](int slot) {
            CallMessage msg;
            msg.id     = client->nextId();
            msg.object = "probe";
            msg.method = "ping";
            client->sendCallAsync(std::move(msg),
                [counts, reply, closed, swept, slot](ResultMessage res) {
                    if (res.ok)                                 reply->fetch_add(1);
                    else if (res.errCode == "TRANSPORT_CLOSED") closed->fetch_add(1);
                    else                                        swept->fetch_add(1);
                    (*counts)[slot].fetch_add(1);
                });
        };

        // ── EACH RESOLVER IS MADE LIVE BY CONSTRUCTION, NOT BY TIMING ────────
        //
        // The three "this resolver ran" assertions at the bottom are what stops
        // a green run from being vacuous, so none of them may rest on a sleep.
        // An earlier cut set the teardown off after a fixed delay and then after
        // the first reply, and both are wrong in opposite directions on a slow
        // box: the first landed the stop ahead of the whole burst, and the
        // second behind all of it (measured on a 3-core macOS CI runner — 0
        // calls took the stopped path, so the run proved nothing about it).
        //
        // Phase 1 answers normally and is WAITED for, which is what makes a
        // reply-resolved call certain.
        for (int i = 0; i < kWarm; ++i) send(i);
        {
            QElapsedTimer t; t.start();
            while (reply->load() == 0 && t.elapsed() < 15000)
                std::this_thread::yield();
        }
        ASSERT_GT(reply->load(), 0)
            << "round " << r << ": the provider never answered anything";

        // Phase 2 answers NOTHING — the provider holds every reply — so every
        // call registered between here and the stop is still pending when
        // fail() sweeps, which is what makes a sweep-resolved call certain.
        provider.hold(true);

        std::vector<std::thread> callers;
        callers.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            callers.emplace_back([send, issued, t] {
                for (int i = 0; i < kPer; ++i) {
                    send(kWarm + t * kPer + i);
                    issued->fetch_add(1);
                }
            });
        }

        // And the stop is triggered by a COUNT of registrations rather than a
        // clock, so calls are still being issued when it lands however slow the
        // box is — which is what makes a reclaim-resolved call certain. The
        // trigger sweeps across rounds and stays far below the 960 the burst
        // will issue.
        const int trigger = 80 + r * 60;
        {
            QElapsedTimer t; t.start();
            while (issued->load() < trigger && t.elapsed() < 15000)
                std::this_thread::yield();
        }
        client->stop("peer vanished mid-burst");

        for (auto& th : callers) th.join();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));

        for (const auto& c : *counts) {
            const int n = c.load();
            if (n == 0) ++dropped; else ++answered;
            if (n > 1)  doubled += n - 1;
        }
        byReply  += reply->load();
        byClosed += closed->load();
        bySweep  += swept->load();

        leaked += static_cast<int>(pendingCalls(client));
        w.provider->stop();
        provider.dropHeld();
    }

    std::cout << "  " << kTotal << " calls racing stop() -> answered=" << answered
              << " (by a reply=" << byReply << ", by fail()'s sweep=" << bySweep
              << ", stopped-connection=" << byClosed << ") DROPPED=" << dropped
              << " DOUBLED=" << doubled
              << " left registered on a dead connection=" << leaked << std::endl;

    EXPECT_EQ(dropped, 0)
        << dropped << " of " << kTotal << " calls were never answered at all";
    EXPECT_EQ(doubled, 0)
        << doubled << " calls were answered more than once: the extract-and-erase "
           "no longer makes the reclaim and fail()'s sweep mutually exclusive";
    EXPECT_EQ(leaked, 0);
    // All three resolvers have to have been live, or the burst did not straddle
    // the teardown and this test raced nothing. Each is arranged for above
    // rather than hoped for; a failure here is a broken harness, not a broken
    // transport, and says which of the three did not run.
    EXPECT_GT(byReply, 0)  << "no call was answered by a reply";
    EXPECT_GT(bySweep, 0)  << "no call was answered by fail()'s sweep";
    EXPECT_GT(byClosed, 0) << "no call was answered as a stopped connection";
}
