// Async delivery in a process that has NO Qt event loop.
//
// WHAT WAS BROKEN. Every async completion in the plain transport goes through
// one hop, and that hop was:
//
//     QCoreApplication* app = QCoreApplication::instance();
//     if (!app) return;                       // <- the callback, dropped
//
// In a Qt-free host — the deployment the plain transport exists for — that
// branch is taken for EVERY call, so `callMethodAsyncWithError` (and
// `lp_invoke_async` above it, and every generated async wrapper above that)
// promised a callback exactly once and delivered zero, forever, silently. The
// caller does not get an error; it gets nothing, which turns a bounded call
// into an unbounded hang.
//
// It could not be observed from protocol_tests, whose main() constructs a
// QCoreApplication before the first test runs — hence this second binary. See
// test_main_noqt.cpp.
//
// WHAT IS PINNED HERE:
//
//   1. The callback ARRIVES, on all four outcomes: an immediate reply, a
//      deferred ("multi") completion, a deadline, and a call cancelled by
//      release(). Each of these reaches the delivery hop from a different
//      thread, and the drop was in the hop, so every one of them was affected.
//   2. It does NOT arrive inline. The constraint on any fix here is that
//      delivery must not move onto the Asio stack: this transport delivers user
//      event callbacks inline on its single io thread already, and a user
//      callback running on an Asio read handler is the re-entrancy class that
//      produced a SIGSEGV on the QtRO twin. So the tests assert the callback
//      did not run inside the call, did not run on the caller's thread, and did
//      not run on the io thread. "Inside the call" is measured as NESTING, by
//      the delivering thread, at delivery time — see IssuingScope, and the note
//      there about why counting deliveries after the call returns is a race
//      rather than a check.
//   3. Exactly once still holds with no Qt loop in the process, including when
//      release() races a burst of arriving replies — the same shape
//      test_iofold.cpp uses, run here against the delivery thread instead of
//      the Qt loop.
//   4. It still arrives AFTER main() HAS RETURNED, from a static destructor —
//      the window in which a lazily-constructed delivery vehicle is already
//      dead. No test case can run there, so that one is a static destructor
//      whose verdict is this binary's exit code; see LateDeliveryProbe.
//
// HOW THE DETECTOR WAS VALIDATED: by running this file against the pre-fix
// tree (feat/plain-async-io-fold, cf1b9b0). Every test below that waits for a
// callback fails there — the waits run out with zero deliveries — which is the
// bug, stated as a test. The numbers are in the PR.
//
// The provider is deliberately Qt-FREE too: an IncomingCallHandler on an
// RpcServerTcp, no ModuleProxy and no QObject, because a QObject provider
// needs a thread with a Qt event loop to dispatch into and this process has
// none. That is also the honest shape — a Qt-free consumer talking to a host
// over a socket.

#include <gtest/gtest.h>

#include "logos_call_error.h"
#include "logos_object.h"
#include "logos_transport_config.h"

#include "io_context_pool.h"
#include "json_codec.h"
#include "plain_logos_object.h"
#include "plain_transport_connection.h"
#include "rpc_server.h"

#include "logos_async_dispatch.h"

#include <QCoreApplication>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace logos::plain;

namespace {

// ── a Qt-free provider ──────────────────────────────────────────────────────
//
// Handles exactly the four shapes the tests need:
//   ping   — answers immediately
//   sink   — never answers at all (the caller's deadline resolves it)
//   defer  — answers with the pending sentinel, then pushes the completion
//            event, which is the "multi" provider protocol
//   never  — answers with a sentinel it never completes (cancellation fodder)
//
// onCall runs on the connection's strand, i.e. the process's single io thread,
// which is exactly the thread the delivery hop must NOT run user callbacks on.
// It is recorded here so the tests can assert that.
class QtFreeHandler : public IncomingCallHandler {
public:
    void onCall(const CallMessage& req, CallReply reply) override
    {
        m_ioThread.store(std::this_thread::get_id());
        m_sawCall.store(true);

        if (req.method == "sink") return;      // no reply, ever

        ResultMessage res;
        res.id = req.id;
        res.ok = true;

        if (req.method == "defer" || req.method == "never") {
            const std::string callId =
                (req.method == "defer" ? "cid-" : "never-")
                + std::to_string(m_counter.fetch_add(1));
            RpcMap pending;
            pending.emplace(logos::pendingCallKey().toStdString(), RpcValue(callId));
            res.value = RpcValue(std::move(pending));
            reply(std::move(res));

            if (req.method == "never") return;

            // The completion is pushed from a worker, the way a real "multi"
            // provider does it, so it arrives as an Event frame rather than
            // inline in the reply.
            EventSink sink = eventSink();
            std::thread([sink, callId] {
                if (!sink) return;
                EventMessage evt;
                evt.object    = "omni";
                evt.eventName = logos::callCompleteEvent().toStdString();
                evt.data.push_back(RpcValue(callId));
                evt.data.push_back(RpcValue(static_cast<int64_t>(7)));
                sink(std::move(evt));
            }).detach();
            return;
        }

        res.value = req.args.empty() ? RpcValue(static_cast<int64_t>(1))
                                     : req.args.front();
        reply(std::move(res));
    }

    void onMethods(const MethodsMessage& req, MethodsReply reply) override
    {
        MethodsResultMessage res;
        res.id = req.id;
        res.ok = true;
        reply(std::move(res));
    }

    void onSubscribe(const SubscribeMessage&, EventSink sink,
                     const void* connectionId) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_sinks[connectionId] = std::move(sink);
    }

    void onUnsubscribe(const UnsubscribeMessage&, const void* connectionId) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_sinks.erase(connectionId);
    }

    void onConnectionClosed(const void* connectionId) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_sinks.erase(connectionId);
    }

    void onToken(const TokenMessage&) override {}

    std::thread::id ioThread() const { return m_ioThread.load(); }
    bool sawCall() const { return m_sawCall.load(); }

private:
    EventSink eventSink()
    {
        std::lock_guard<std::mutex> g(m_mu);
        return m_sinks.empty() ? EventSink{} : m_sinks.begin()->second;
    }

    std::mutex                            m_mu;
    std::map<const void*, EventSink>      m_sinks;
    std::atomic<std::thread::id>          m_ioThread{};
    std::atomic<bool>                     m_sawCall{false};
    std::atomic<std::uint64_t>            m_counter{0};
};

class QtFreeHost {
public:
    QtFreeHost()
    {
        m_server = std::make_shared<RpcServerTcp>(
            IoContextPool::shared().ioContext(), "127.0.0.1", 0,
            std::make_shared<JsonCodec>(), m_handler);
        m_started = m_server->start();
    }
    ~QtFreeHost() { if (m_server) m_server->stop(); }

    bool ok() const { return m_started && m_server->boundPort() != 0; }
    uint16_t port() const { return m_server->boundPort(); }
    QtFreeHandler& handler() { return *m_handler; }

private:
    // DELIBERATELY LEAKED, and this is a harness lifetime bug that was worth
    // finding rather than a style choice. RpcServer keeps a RAW
    // IncomingCallHandler*, hands it to every connection, and nothing joins the
    // io thread — not stop(), not the server's destruction. So a frame that has
    // already been read from the socket can be dispatched into the handler after
    // this object's members would have been destroyed, and a handler that was a
    // member died first: SIGBUS on the io thread, inside
    // RpcConnection::dispatchIncoming calling a virtual on freed storage.
    //
    // Observed at 1 run in 25 (and 1 in 5 under Guard Malloc) once the run got
    // slightly longer — a burst test leaves frames queued, and the process has to
    // stay alive long enough for the io thread to reach them. It is the test's
    // bug, not the transport's, and the fix is to let the handler outlive the io
    // thread: one small object per host, in a test binary that is about to exit.
    QtFreeHandler*                 m_handler = new QtFreeHandler();
    std::shared_ptr<RpcServerTcp>  m_server;
    bool                           m_started = false;
};

std::unique_ptr<PlainTransportConnection> connectTo(uint16_t port)
{
    LogosTransportConfig cfg;
    cfg.protocol = LogosProtocol::Tcp;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    auto conn = std::make_unique<PlainTransportConnection>(cfg);
    if (!conn->connectToHost()) return nullptr;
    return conn;
}

LogosObjectErrorChannel* channelFor(LogosObject* obj)
{
    return dynamic_cast<LogosObjectErrorChannel*>(obj);
}

// ── the inline check, done properly ─────────────────────────────────────────
//
// "Did the callback run INLINE inside the call that issued it" is a question
// about NESTING, and the only place it can be answered is at the moment of
// delivery, on the delivering thread. This thread-local depth counter is that
// answer: IssuingScope raises it around an issuing call, and a callback that
// runs nested inside that call — which can only happen on the issuing thread,
// because inline means on this stack — sees its own thread's copy raised. A
// callback on the delivery thread reads that thread's copy, which is zero.
// Nothing races: no shared state, no ordering, no timing.
//
// WHAT THIS REPLACES, because it was wrong in a way worth remembering. The first
// version of test 1 read `d.total` AFTER callMethodAsyncWithError returned and
// asserted it was still zero. That is not an inline check, it is a race with the
// delivery thread — which is allowed to deliver the instant the call returns —
// and it failed 3 in 200 plain / 2 in 40 under Guard Malloc, always with
// on-caller-thread=0, i.e. always with nothing whatsoever having run inline.
// This file already says as much about tests 2 and 5 ("that measures scheduling
// luck"); test 1 was doing it anyway. A post-hoc count read cannot distinguish
// "ran inline" from "ran promptly, elsewhere". Nesting can.
thread_local int t_insideIssue = 0;

struct IssuingScope {
    IssuingScope()  { ++t_insideIssue; }
    ~IssuingScope() { --t_insideIssue; }
    IssuingScope(const IssuingScope&) = delete;
    IssuingScope& operator=(const IssuingScope&) = delete;
};

// Per-call delivery counts, plus where the delivery happened. Both 0 and 2 are
// failures and both are counted per call, because a double on one call and a
// drop on another cancel out in a total.
struct Deliveries {
    explicit Deliveries(int n) : counts(n) {}
    std::vector<std::atomic<int>> counts;
    std::atomic<int>              total{0};

    // Deliveries that ran nested inside the call that issued them. Observed by
    // the delivering thread itself, at delivery time — see IssuingScope.
    std::atomic<int>              inlineDeliveries{0};

    // A thread no delivery may ever run on, counted rather than sampled.
    // Set before the first call is issued and only read after the last has
    // landed, so it needs no synchronisation of its own.
    //
    // Sampling the LAST delivery's thread is not enough for this claim: 300
    // calls delivered correctly and one delivered on the caller's stack is
    // still the bug, and a last-writer-wins field would miss it 299 times out
    // of 300.
    std::thread::id  forbiddenThread{};
    std::atomic<int> onForbiddenThread{0};

    std::mutex      mu;
    std::string     lastCode;
    QVariant        lastValue;
    std::thread::id lastThread;

    void record(int i, QVariant v, const logos::CallError& e)
    {
        // FIRST, and on the delivering thread: nesting is only observable from
        // inside the delivery.
        if (t_insideIssue > 0) inlineDeliveries.fetch_add(1);
        if (forbiddenThread != std::thread::id{}
            && std::this_thread::get_id() == forbiddenThread)
            onForbiddenThread.fetch_add(1);
        {
            std::lock_guard<std::mutex> g(mu);
            lastCode   = e.code;
            lastValue  = std::move(v);
            lastThread = std::this_thread::get_id();
        }
        counts[i].fetch_add(1);
        total.fetch_add(1);
    }
    std::string code() { std::lock_guard<std::mutex> g(mu); return lastCode; }
    std::thread::id thread() { std::lock_guard<std::mutex> g(mu); return lastThread; }
    int worst() const
    {
        int w = 0;
        for (const auto& c : counts) w = std::max(w, c.load());
        return w;
    }
    int missing() const
    {
        int m = 0;
        for (const auto& c : counts) if (c.load() == 0) ++m;
        return m;
    }
};

// There is no event loop to pump: the only thing to do is wait. A budget rather
// than a fixed sleep so the passing case is fast and the failing case is
// unambiguous.
bool waitFor(Deliveries& d, int target, int budgetMs)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(budgetMs);
    while (d.total.load() < target
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return d.total.load() >= target;
}

void settle(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

const char* kToken = "noqt-token";

// ── the delivery that happens AFTER main() has returned ─────────────────────
//
// THE DEFECT THIS PROBE EXISTS FOR. The delivery vehicle used to be an ordinary
// function-local static, constructed on the FIRST async delivery in the process.
// Anything with static storage constructed before that — which is everything
// constructed during dynamic initialisation — is therefore destroyed AFTER it,
// so a delivery issued from such a destructor posted into an io_context that had
// already run its own destructor, on a thread that had already been joined. Under
// Guard Malloc: SIGSEGV in scheduler::post_immediate_completion, reached through
// __cxa_finalize, three runs out of three. Without it: silence, delivered=0 —
// which is the exact bug this file was written to kill, moved to a later moment
// in the process's life.
//
// WHY IT IS A STATIC OBJECT AND NOT A TEST CASE. The moment under test is "after
// main() returned", and no test case runs there. So the check is a destructor,
// and its verdict is the PROCESS EXIT CODE: a failure calls _Exit with a
// distinctive status, which is what CI (and `ninja test`) sees, because gtest has
// long since printed its summary and returned. There is no other way to assert
// on this window.
//
// ORDERING, which is the whole point and is easy to break by accident: this
// object must be constructed BEFORE the first delivery in the process, so that on
// a tree that destroys the vehicle it is destroyed AFTER it. Dynamic
// initialisation gives that for free — and it is why this probe must NOT warm the
// vehicle up in its own constructor: doing so would register the vehicle's
// destructor first and hand it the LONGER life, quietly turning the reproduction
// into a no-op. TheVehicleOutlivesStaticDestructors below does the warm-up, from
// inside a test, where it lands after all dynamic initialisation.
std::atomic<int>  g_lateDeliveries{0};
std::atomic<bool> g_lateOffTheIssuingThread{false};
std::atomic<bool> g_lateProbeArmed{false};

struct LateDeliveryProbe {
    LateDeliveryProbe() { g_lateProbeArmed.store(true); }

    ~LateDeliveryProbe()
    {
        const std::thread::id issuing = std::this_thread::get_id();
        std::fprintf(stderr,
            "\n[post-main] issuing an async call from a static destructor "
            "(after main() returned)\n");
        std::fflush(stderr);

        // The null-connection early return: the smallest path that reaches the
        // delivery hop, with no socket, no host and no io thread of its own.
        PlainLogosObject obj("nobody", nullptr);
        obj.callMethodAsyncWithError(kToken, QStringLiteral("ping"), {}, 1000,
            [issuing](QVariant, const logos::CallError&) {
                if (std::this_thread::get_id() != issuing)
                    g_lateOffTheIssuingThread.store(true);
                g_lateDeliveries.fetch_add(1);
            });

        // Bounded, like every other wait in this file: the passing case is fast
        // and the failing case is unambiguous rather than a hang at exit.
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(5000);
        while (g_lateDeliveries.load() == 0
               && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));

        const int  delivered = g_lateDeliveries.load();
        const bool offThread = g_lateOffTheIssuingThread.load();
        std::fprintf(stderr,
            "[post-main] delivered=%d off-the-issuing-thread=%d\n",
            delivered, static_cast<int>(offThread));
        std::fflush(stderr);

        if (delivered != 1 || !offThread) {
            std::fprintf(stderr,
                "\nPOST-MAIN DELIVERY FAILED: callMethodAsyncWithError promises "
                "its callback exactly once, and a delivery issued after main() "
                "returned got %d of them%s.\n"
                "  The delivery vehicle must outlive every possible caller, "
                "which for a process-wide static means it must never be "
                "destroyed. See DeliveryService in plain_logos_object.cpp.\n",
                delivered,
                (delivered == 1 && !offThread)
                    ? " (and ran on the issuing thread, which is the inline "
                      "delivery the hop exists to prevent)"
                    : "");
            std::fflush(stderr);
            std::_Exit(70);
        }
    }
};

// Constructed during dynamic initialisation; destroyed after main(). See above.
LateDeliveryProbe g_lateDeliveryProbe;

} // namespace

class NoQtLoopTest : public ::testing::Test {
protected:
    // The premise of the whole file, checked on every test rather than assumed:
    // if something ever constructs a QCoreApplication in this process, these
    // tests silently stop testing anything.
    void SetUp() override
    {
        ASSERT_EQ(QCoreApplication::instance(), nullptr)
            << "this binary must run with NO QCoreApplication — otherwise it is "
               "just protocol_tests with fewer tests";
    }
};

// ── 1. the smallest possible statement of the bug ───────────────────────────
//
// No host, no socket, no threads of our own: a handle whose connection is not
// open takes the early-return branch in callMethodAsyncWithError, which posts
// the failure through the same delivery hop as everything else. Pre-fix the
// callback is dropped there and this waits out its whole budget.
TEST_F(NoQtLoopTest, AFailedCallDeliversItsCallbackWithNoQtLoop)
{
    PlainLogosObject obj("nobody", nullptr);

    Deliveries d(1);
    const std::thread::id caller = std::this_thread::get_id();
    d.forbiddenThread = caller;

    {
        // The nesting marker, raised for exactly the duration of the call. A
        // callback that runs inline runs INSIDE this scope, on this thread, and
        // says so from inside itself — which is a fact about the stack and not
        // about who won a race. Reading d.total after the call returns would be
        // the race; see IssuingScope.
        IssuingScope inCall;
        obj.callMethodAsyncWithError(kToken, QStringLiteral("ping"), {}, 1000,
                                     [&d](QVariant v, const logos::CallError& e) {
                                         d.record(0, std::move(v), e);
                                     });
    }

    const bool arrived = waitFor(d, 1, 5000);
    settle(50);   // a duplicate would land here

    std::cout << "  no-Qt failure path: delivered=" << d.total.load()
              << " code='" << d.code() << "'"
              << " inline=" << d.inlineDeliveries.load()
              << " on-caller-thread=" << d.onForbiddenThread.load() << std::endl;

    ASSERT_TRUE(arrived)
        << "the callback was never delivered: with no QCoreApplication the "
           "delivery hop dropped it, and callMethodAsyncWithError's "
           "exactly-once promise became exactly-never";
    EXPECT_EQ(d.total.load(), 1);
    EXPECT_EQ(d.code(), "transport_error");
    EXPECT_EQ(d.inlineDeliveries.load(), 0)
        << "the callback ran inline inside callMethodAsyncWithError";
    EXPECT_EQ(d.onForbiddenThread.load(), 0)
        << "the callback ran on the caller's thread — delivery must stay off "
           "the issuing stack";
}

// ── 2. a real call over a real socket ───────────────────────────────────────
//
// The normal path, at volume, with the delivery thread as the only place a
// callback can land. Also the place to pin the constraint that shapes the fix:
// the callback must not run on the io thread, which is where the reply is
// decoded.
TEST_F(NoQtLoopTest, RepliesDeliverExactlyOnceWithNoQtLoop)
{
    QtFreeHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* obj = conn->requestObject(QStringLiteral("omni"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);

    constexpr int kCalls = 300;
    Deliveries d(kCalls);
    const std::thread::id caller = std::this_thread::get_id();
    // NOT "no callback had arrived by the time the loop finished" — with a real
    // delivery thread, callbacks for the early calls legitimately land while the
    // later ones are still being issued, so that measures scheduling luck. The
    // claim is per delivery and it is about the THREAD: no callback may run on
    // the stack that issued the call.
    d.forbiddenThread = caller;

    for (int i = 0; i < kCalls; ++i) {
        // Per call, so the nesting claim covers all 300 issue points and not
        // just the loop as a whole.
        IssuingScope inCall;
        ch->callMethodAsyncWithError(kToken, QStringLiteral("ping"),
                                     QVariantList{ QVariant(i) }, 10000,
                                     [&d, i](QVariant v, const logos::CallError& e) {
                                         d.record(i, std::move(v), e);
                                     });
    }

    const bool arrived = waitFor(d, kCalls, 20000);
    settle(200);   // duplicates would land here

    std::cout << "  no-Qt replies: " << d.total.load() << "/" << kCalls
              << " worst=" << d.worst() << " missing=" << d.missing()
              << " on-caller-thread=" << d.onForbiddenThread.load()
              << " delivery-thread!=io-thread="
              << (d.thread() != host.handler().ioThread()) << std::endl;

    ASSERT_TRUE(arrived) << "only " << d.total.load() << " of " << kCalls
                         << " callbacks were delivered with no Qt loop";
    EXPECT_EQ(d.inlineDeliveries.load(), 0)
        << d.inlineDeliveries.load() << " callbacks ran nested inside the call "
           "that issued them";
    EXPECT_EQ(d.onForbiddenThread.load(), 0)
        << d.onForbiddenThread.load() << " callbacks ran on the thread that "
           "issued the call";
    EXPECT_EQ(d.worst(), 1);
    EXPECT_EQ(d.missing(), 0);
    EXPECT_TRUE(d.code().empty());
    ASSERT_TRUE(host.handler().sawCall());
    EXPECT_NE(d.thread(), host.handler().ioThread())
        << "the callback ran on the transport's io thread — that is the "
           "re-entrancy the delivery hop exists to prevent, and delivering "
           "inline there would be a worse bug than the drop";

    obj->release();
    settle(50);
}

// ── 3. the deferred ("multi") completion ────────────────────────────────────
//
// Reaches the hop from the completion-event handler on the io thread, which is
// a different resolver from the reply path above and was dropped just as hard.
TEST_F(NoQtLoopTest, DeferredCompletionsDeliverWithNoQtLoop)
{
    QtFreeHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* obj = conn->requestObject(QStringLiteral("omni"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);

    constexpr int kCalls = 40;
    Deliveries d(kCalls);
    for (int i = 0; i < kCalls; ++i) {
        ch->callMethodAsyncWithError(kToken, QStringLiteral("defer"), {}, 10000,
                                     [&d, i](QVariant v, const logos::CallError& e) {
                                         d.record(i, std::move(v), e);
                                     });
    }
    const bool arrived = waitFor(d, kCalls, 20000);
    settle(200);

    std::cout << "  no-Qt deferred completions: " << d.total.load() << "/"
              << kCalls << " worst=" << d.worst() << " code='" << d.code()
              << "'" << std::endl;

    ASSERT_TRUE(arrived) << "only " << d.total.load() << " of " << kCalls
                         << " deferred completions were delivered";
    EXPECT_EQ(d.worst(), 1);
    EXPECT_EQ(d.missing(), 0);
    EXPECT_TRUE(d.code().empty());
    {
        std::lock_guard<std::mutex> g(d.mu);
        EXPECT_EQ(d.lastValue.toInt(), 7)
            << "the deferred call delivered the sentinel, not the completion";
    }

    obj->release();
    settle(50);
}

// ── 4. the deadline ─────────────────────────────────────────────────────────
//
// Reaches the hop from the DeadlineService thread — the third distinct
// resolver. A dropped timeout is the worst of the four: the call is never going
// to be answered, so the caller waits forever on a callback that was the only
// thing that could have told it so.
TEST_F(NoQtLoopTest, TimeoutsDeliverWithNoQtLoop)
{
    QtFreeHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* obj = conn->requestObject(QStringLiteral("omni"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);

    constexpr int kCalls = 20;
    Deliveries d(kCalls);
    for (int i = 0; i < kCalls; ++i) {
        ch->callMethodAsyncWithError(kToken, QStringLiteral("sink"), {}, 200,
                                     [&d, i](QVariant v, const logos::CallError& e) {
                                         d.record(i, std::move(v), e);
                                     });
    }
    const bool arrived = waitFor(d, kCalls, 10000);
    settle(200);

    std::cout << "  no-Qt timeouts: " << d.total.load() << "/" << kCalls
              << " worst=" << d.worst() << " code='" << d.code() << "'"
              << std::endl;

    ASSERT_TRUE(arrived) << "only " << d.total.load() << " of " << kCalls
                         << " deadlines were delivered";
    EXPECT_EQ(d.worst(), 1);
    EXPECT_EQ(d.missing(), 0);
    EXPECT_EQ(d.code(), "timeout");

    obj->release();
    settle(50);
}

// ── 5. cancellation by release() ────────────────────────────────────────────
//
// The fourth resolver: teardown, on the caller's own thread. This one is the
// reason the fix cannot be "call it inline when there is no Qt loop" — inline
// here means running user code from inside release(), i.e. from inside a
// destructor path, which is exactly the re-entrancy that has already produced a
// SIGSEGV in this codebase on the QtRO twin.
TEST_F(NoQtLoopTest, CancelledCallsDeliverWithNoQtLoopAndNotInsideRelease)
{
    QtFreeHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* obj = conn->requestObject(QStringLiteral("omni"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);

    constexpr int kCalls = 20;
    Deliveries d(kCalls);
    const std::thread::id caller = std::this_thread::get_id();
    d.forbiddenThread = caller;
    for (int i = 0; i < kCalls; ++i) {
        IssuingScope inCall;
        ch->callMethodAsyncWithError(kToken, QStringLiteral("never"), {}, 30000,
                                     [&d, i](QVariant v, const logos::CallError& e) {
                                         d.record(i, std::move(v), e);
                                     });
    }
    settle(300);   // let every sentinel come back, so the calls are parked
    ASSERT_EQ(d.total.load(), 0) << "the calls resolved before the release";

    {
        // The same nesting marker, around release() this time: a cancellation
        // callback that ran from inside release() would run inside this scope,
        // on this thread, and would count itself.
        IssuingScope inRelease;
        obj->release();
    }

    const bool arrived = waitFor(d, kCalls, 10000);
    settle(200);

    std::cout << "  no-Qt cancellations: " << d.total.load() << "/" << kCalls
              << " worst=" << d.worst() << " code='" << d.code()
              << "' inside-release=" << d.inlineDeliveries.load()
              << " on-releasing-thread=" << d.onForbiddenThread.load()
              << std::endl;

    ASSERT_TRUE(arrived) << "only " << d.total.load() << " of " << kCalls
                         << " cancellations were delivered";
    // "How many had arrived by the time release() returned" is NOT the inline
    // check, and measuring it that way is how this test first went red under
    // Guard Malloc: with everything slowed down, the delivery thread finished
    // all twenty before the releasing thread executed its next statement, which
    // is correct behaviour and reads as a violation. The claim is about NESTING,
    // and the marker above measures exactly that — plus the thread, since the
    // releasing thread is the forbidden one here.
    EXPECT_EQ(d.inlineDeliveries.load(), 0)
        << "a cancellation callback ran from inside release()";
    EXPECT_EQ(d.worst(), 1);
    EXPECT_EQ(d.missing(), 0);
    EXPECT_EQ(d.code(), "transport_error");
    EXPECT_EQ(d.onForbiddenThread.load(), 0)
        << "a cancellation callback ran on the releasing thread";
}

// ── 6. exactly once, with the delivery thread as the hop ────────────────────
//
// The release-racing-replies shape from test_iofold.cpp — the only one that
// actually detects a broken exactly-once gate — re-run here so that the gate is
// pinned against the OTHER delivery vehicle too. Teardown snapshots the
// in-flight map and then delivers one call at a time with the lock released,
// so the io thread has a window N deliveries wide in which to answer a call
// teardown has already claimed.
//
// VALIDATED AS A DETECTOR HERE, and the validation turned up something about the
// mechanism that the note in tests/protocol/CMakeLists.txt does not say. There
// are TWO gates in AsyncCall, not one: claim()'s compare-exchange, and the
// swap in takeCallback() which leaves a second caller holding an empty
// std::function. Removing the CAS alone changes nothing measurable — this test,
// its Qt twin and PlainCancelPendingRaceTest all stay green, 0 doubles — because
// the swap still absorbs the duplicate. With BOTH removed this test reports 22
// doubled deliveries in 20 rounds x 500 calls, while the three PER-PATH tests
// above stay green, which is the difference between a detector and a pin.
//
// So: a validation that removes one of the two gates proves nothing, and the
// exactly-once guarantee is stronger than the CAS on its own. Neither half is
// redundant — the CAS is what stops the second caller from also erasing
// registries and cancelling timers — but the CALLBACK is protected by the swap.
TEST_F(NoQtLoopTest, ReleaseRacingRepliesDeliversEachCallOnceWithNoQtLoop)
{
    QtFreeHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    constexpr int kRounds = 20;
    constexpr int kCalls  = 500;
    int doubled = 0;
    int dropped = 0;
    int byReply = 0;
    int byTeardown = 0;

    for (int r = 0; r < kRounds; ++r) {
        LogosObject* obj = conn->requestObject(QStringLiteral("omni"), 5000);
        ASSERT_NE(obj, nullptr);
        auto* ch = channelFor(obj);
        ASSERT_NE(ch, nullptr);

        auto d = std::make_shared<Deliveries>(kCalls);
        auto codes = std::make_shared<std::vector<std::atomic<int>>>(2);
        for (int i = 0; i < kCalls; ++i) {
            ch->callMethodAsyncWithError(kToken, QStringLiteral("ping"),
                                         QVariantList{ QVariant(i) }, 20000,
                                         [d, codes, i](QVariant v, const logos::CallError& e) {
                                             (*codes)[e.code.empty() ? 0 : 1].fetch_add(1);
                                             d->record(i, std::move(v), e);
                                         });
        }
        // WAIT FOR THE FIRST REPLY, then release — and this is not a tidy-up,
        // it is what makes the test a race at all.
        //
        // In this binary the provider and the consumer share the process's ONE
        // io thread (the QtFreeHandler runs on the same strand that writes the
        // client's frames), unlike the Qt twin in test_iofold.cpp whose provider
        // sits on its own QThread. So the issuing thread can enqueue all 500
        // calls and release before the io thread has drained a single one: on a
        // slow sandbox that produced answered-by-reply=0,
        // cancelled-by-teardown=10000 — one resolver, no race, and the
        // exactly-once assertions below reduced to decoration. (It still
        // reported 0 doubles and 0 drops, which is exactly why the
        // "both resolvers were live" guards further down have to exist.)
        //
        // Waiting for one delivery proves the reply path is running; with 500
        // calls in the burst, hundreds are still outstanding for teardown to
        // cancel. The jittered nudge then sweeps where in the burst it lands.
        ASSERT_TRUE(waitFor(*d, 1, 20000))
            << "round " << r << ": no reply came back at all before the release";
        std::this_thread::sleep_for(std::chrono::microseconds((r % 6) * 120));
        obj->release();

        // Per round rather than in aggregate, so a run against a tree that
        // drops callbacks stops on the first round instead of waiting out
        // twenty budgets.
        ASSERT_TRUE(waitFor(*d, kCalls, 20000))
            << "round " << r << ": only " << d->total.load() << " of " << kCalls
            << " callbacks were delivered";
        settle(50);

        for (const auto& c : d->counts) {
            if (c.load() > 1) doubled += c.load() - 1;
            if (c.load() == 0) ++dropped;
        }
        byReply    += (*codes)[0].load();
        byTeardown += (*codes)[1].load();
    }

    std::cout << "  no-Qt: " << kRounds << " rounds x " << kCalls
              << " calls released mid-burst -> answered-by-reply=" << byReply
              << " cancelled-by-teardown=" << byTeardown
              << " DOUBLE deliveries=" << doubled << " dropped=" << dropped
              << std::endl;

    EXPECT_GT(byReply, 0)    << "no call was answered by its reply";
    EXPECT_GT(byTeardown, 0) << "no call was cancelled by teardown — the race "
                                "did not happen";
    EXPECT_EQ(doubled, 0) << doubled << " calls were delivered more than once";
    EXPECT_EQ(dropped, 0) << dropped << " calls were never delivered at all";
}

// ── 7. the delivery vehicle outlives static destruction ─────────────────────
//
// The visible half of the post-main probe above: this test WARMS THE VEHICLE UP
// — constructing it here, i.e. after all dynamic initialisation, is what puts it
// on the wrong side of the destruction order from the probe — and states where
// the verdict will appear. The assertion itself cannot live in a test case,
// because the moment it is about is after main() returns; it lives in
// LateDeliveryProbe::~LateDeliveryProbe and reports through the process exit
// code. See the note over that struct.
//
// On the tree this was written against (the delivery service as an ordinary
// function-local static) the run ends in `[post-main] delivered=0` followed by
// exit 70, or SIGSEGV under Guard Malloc. Both are what a failure looks like
// here, and both were observed before the fix.
TEST_F(NoQtLoopTest, TheVehicleOutlivesStaticDestructors)
{
    ASSERT_TRUE(g_lateProbeArmed.load())
        << "the post-main probe was not constructed during dynamic "
           "initialisation, so nothing will check the after-main() window";

    PlainLogosObject obj("nobody", nullptr);
    Deliveries d(1);
    {
        IssuingScope inCall;
        obj.callMethodAsyncWithError(kToken, QStringLiteral("ping"), {}, 1000,
                                     [&d](QVariant v, const logos::CallError& e) {
                                         d.record(0, std::move(v), e);
                                     });
    }
    ASSERT_TRUE(waitFor(d, 1, 5000))
        << "the warm-up delivery never arrived, so the vehicle was never "
           "constructed and the probe below tests nothing";
    EXPECT_EQ(d.inlineDeliveries.load(), 0);

    std::cout << "  delivery vehicle constructed inside the run; the after-main "
                 "delivery is checked by LateDeliveryProbe (exit code 70 on "
                 "failure)" << std::endl;
}
