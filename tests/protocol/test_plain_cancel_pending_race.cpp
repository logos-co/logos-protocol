// WHERE EXACTLY-ONCE RESTS, ONCE cancelPending() EXISTS.
//
// cancelPending() withdraws a call's registration from the connection's pending
// map. It is tempting — and rpc_connection.h used to say — that a caller which
// has given up therefore "is never called back at all". It is not true, and the
// distance between the two matters, because it decides whether the callers of
// sendCallAsync() need idempotent handlers or merely tidy ones.
//
// dispatchIncoming() COPIES the handler out of the map under m_mu and invokes it
// with the mutex RELEASED. A cancelPending() that arrives in that gap erases an
// entry that is no longer there and returns having stopped nothing; the handler
// then runs to completion. So:
//
//   * at-most-once INVOCATION of a registered handler is the connection's, and
//     it comes from the extract-and-erase under m_mu — three contenders
//     (dispatchIncoming, fail()'s sweep, cancelPending) and only one can win;
//   * exactly-once DELIVERY to the user is NOT the connection's. It belongs to
//     the handler, and for PlainLogosObject it is AsyncCall::deliver()'s CAS.
//
// These tests construct that interleaving by hand rather than racing for it: a
// connection stub reproduces dispatchIncoming's extract-then-invoke exactly and
// lets the test stand between the two halves.
//
// The second test is a validated detector: against a transport whose
// AsyncCall::claim() does not compare-exchange and whose takeCallback() copies
// rather than swaps — a local edit in a throwaway checkout, not a switch in
// this tree; see the top of test_iofold.cpp — it reports 2 deliveries for 1
// call.

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

#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using namespace logos::plain;

namespace {

// A connection that goes no further than its pending map, so a test can occupy
// the gap dispatchIncoming leaves between taking a handler out and running it.
// Everything below m_pending mirrors RpcConnection: registration under the
// mutex, extract-and-erase under the mutex, invocation with it released.
class StubConnection : public RpcConnectionBase {
public:
    void start() override {}
    void stop(const std::string&) override { m_open = false; }
    bool isOpen() const override { return m_open; }

    std::future<ResultMessage> sendCall(CallMessage msg) override
    {
        // The OTHER shape of handler in this codebase: a promise. Registered
        // through the same path, deliberately, so the tests below can reach it.
        auto p = std::make_shared<std::promise<ResultMessage>>();
        auto f = p->get_future();
        sendCallAsync(std::move(msg), [p](ResultMessage r) {
            try { p->set_value(std::move(r)); } catch (...) {}
        });
        return f;
    }

    void sendCallAsync(CallMessage msg, ResultHandler handler) override
    {
        if (!handler) return;
        std::lock_guard<std::mutex> g(m_mu);
        m_lastId = msg.id;
        m_pending[msg.id] = std::move(handler);
    }

    std::future<MethodsResultMessage> sendMethods(MethodsMessage msg) override
    {
        std::promise<MethodsResultMessage> p;
        MethodsResultMessage r; r.id = msg.id; r.ok = true;
        p.set_value(std::move(r));
        return p.get_future();
    }

    void cancelPending(std::uint64_t id) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_pending.erase(id);
        m_cancels.fetch_add(1);
    }

    void sendSubscribe(SubscribeMessage, std::function<void(EventMessage)>) override {}
    void sendUnsubscribe(UnsubscribeMessage) override {}
    void sendEvent(EventMessage) override {}
    void sendToken(TokenMessage) override {}
    void setErrorHandler(ErrorHandler) override {}
    std::uint64_t nextId() override { return m_next.fetch_add(1); }

    // dispatchIncoming's FIRST half, verbatim: find under the mutex, move out,
    // erase, drop the mutex. The caller decides when the second half runs.
    ResultHandler extract(std::uint64_t id)
    {
        std::lock_guard<std::mutex> g(m_mu);
        auto it = m_pending.find(id);
        if (it == m_pending.end()) return nullptr;
        ResultHandler h = std::move(it->second);
        m_pending.erase(it);
        return h;
    }

    std::uint64_t lastId() const { return m_lastId; }
    int  cancels() const         { return m_cancels.load(); }
    size_t pendingCount()        { std::lock_guard<std::mutex> g(m_mu); return m_pending.size(); }

private:
    std::mutex                             m_mu;
    std::map<std::uint64_t, ResultHandler> m_pending;
    std::atomic<std::uint64_t>             m_next{1};
    std::atomic<std::uint64_t>             m_lastId{0};
    std::atomic<int>                       m_cancels{0};
    bool                                   m_open = true;
};

QCoreApplication* ensureApp()
{
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance()) new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

// Deliveries land on the Qt loop, so nothing is counted until it is pumped.
void pumpUntil(std::atomic<int>& counter, int target, int budgetMs)
{
    QElapsedTimer t; t.start();
    while (counter.load() < target && t.elapsed() < budgetMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

void pump(int ms)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

ResultMessage okResult(std::uint64_t id, int value)
{
    ResultMessage res;
    res.id = id;
    res.ok = true;
    res.value = RpcValue{static_cast<int64_t>(value)};
    return res;
}

}  // namespace

// ── 1. the literal claim: cancelPending() does NOT prevent a callback ────────
TEST(PlainCancelPendingRaceTest, CancelPendingCannotStopAnAlreadyExtractedHandler)
{
    ensureApp();
    auto conn = std::make_shared<StubConnection>();
    auto* obj = new PlainLogosObject("cancel_probe", conn);

    std::atomic<int> deliveries{0};
    obj->callMethodAsyncWithError(QStringLiteral("tok"), QStringLiteral("ping"),
                                  QVariantList{}, 20000,
                                  [&deliveries](QVariant, const logos::CallError&) {
                                      deliveries.fetch_add(1);
                                  });
    const std::uint64_t id = conn->lastId();

    // The reply is decoded: the handler leaves the map.
    RpcConnectionBase::ResultHandler h = conn->extract(id);
    ASSERT_TRUE(h) << "the call never registered a handler";

    // The caller gives up HERE, in the gap. cancelPending() finds nothing.
    conn->cancelPending(id);
    EXPECT_EQ(conn->pendingCount(), 0u);
    EXPECT_EQ(deliveries.load(), 0) << "nothing should have been delivered yet";

    // ...and the handler runs anyway.
    h(okResult(id, 42));
    pumpUntil(deliveries, 1, 2000);

    EXPECT_EQ(deliveries.load(), 1)
        << "cancelPending() ran before this callback and did not stop it — which "
           "is the point: the comment claiming a cancelled caller 'is never "
           "called back at all' is what is wrong, not the code";

    obj->release();
    pump(50);
}

// ── 2. so exactly-once has to come from somewhere else: the CAS ─────────────
//
// The same gap, but with the second resolver being the one that actually exists
// in production — teardown, which cancels every outstanding call. Both paths
// reach AsyncCall::deliver() for the same call. With the CAS removed this
// reports 2.
TEST(PlainCancelPendingRaceTest, AnExtractedReplyRacingTeardownDeliversExactlyOnce)
{
    ensureApp();
    auto conn = std::make_shared<StubConnection>();
    auto* obj = new PlainLogosObject("cancel_probe", conn);

    std::atomic<int> deliveries{0};
    std::atomic<int> released{0};      // callErrorReleased — teardown won
    std::atomic<int> answered{0};      // no error            — the reply won
    obj->callMethodAsyncWithError(QStringLiteral("tok"), QStringLiteral("ping"),
                                  QVariantList{}, 20000,
                                  [&](QVariant, const logos::CallError& e) {
                                      deliveries.fetch_add(1);
                                      if (e.ok()) answered.fetch_add(1);
                                      else        released.fetch_add(1);
                                  });
    const std::uint64_t id = conn->lastId();

    // The io thread has the handler in hand...
    RpcConnectionBase::ResultHandler h = conn->extract(id);
    ASSERT_TRUE(h);

    // ...teardown resolves the call and frees the handle out from under it...
    obj->release();

    // ...and only then does the reply run. It holds a shared_ptr to its
    // AsyncCall, so it is safe to run at all — and finds the gate taken.
    h(okResult(id, 42));

    pumpUntil(deliveries, 1, 2000);
    pump(100);   // a second delivery would land in here

    EXPECT_EQ(deliveries.load(), 1)
        << "the call was delivered " << deliveries.load()
        << " times; exactly-once is AsyncCall::deliver()'s CAS and nothing else";
    EXPECT_EQ(released.load(), 1) << "teardown should have been the resolver";
    EXPECT_EQ(answered.load(), 0);

    pump(50);
}

// ── 3. the OTHER caller of sendCallAsync: a promise, not an AsyncCall ────────
//
// RpcConnection::sendCall() registers a promise-fulfilling handler through the
// same path, and callMethodWithError() / getMethods() call cancelPending() on
// their timeout. Neither of those handlers has a CAS — so the question is
// whether at-most-once INVOCATION is enough for them, and it is: the extract is
// what makes it single, and firing into a future nobody will read is harmless.
// Run here rather than argued, on a REAL RpcConnection.
TEST(PlainCancelPendingRaceTest, ThePromiseShapedHandlerSurvivesTheSameGap)
{
    auto conn = std::make_shared<StubConnection>();

    CallMessage msg;
    msg.id     = conn->nextId();
    msg.object = "promise_probe";
    msg.method = "ping";
    auto fut = conn->sendCall(std::move(msg));
    const std::uint64_t id = conn->lastId();

    // Extract, cancel in the gap, then run the promise handler.
    RpcConnectionBase::ResultHandler h = conn->extract(id);
    ASSERT_TRUE(h);
    conn->cancelPending(id);
    EXPECT_EQ(conn->pendingCount(), 0u)
        << "nobody else can reach this handler now — that is what makes the "
           "single invocation single";

    // Fulfils a future the caller has already walked away from. No throw, no
    // second invocation possible.
    EXPECT_NO_THROW(h(okResult(id, 7)));
    ASSERT_EQ(fut.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_TRUE(fut.get().ok);

    // And the abandoned-future case, which is what the sync path actually does:
    // the caller times out, returns, and its future dies before the reply lands.
    CallMessage msg2;
    msg2.id     = conn->nextId();
    msg2.object = "promise_probe";
    msg2.method = "ping";
    {
        auto doomed = conn->sendCall(std::move(msg2));
        (void)doomed;   // goes out of scope exactly as callMethodWithError's does
    }
    const std::uint64_t id2 = conn->lastId();
    RpcConnectionBase::ResultHandler h2 = conn->extract(id2);
    ASSERT_TRUE(h2);
    conn->cancelPending(id2);
    EXPECT_NO_THROW(h2(okResult(id2, 9)))
        << "setting a value on a shared state whose future is gone must be a "
           "no-op, not a throw";
}

// ── 4. the same gap on the REAL connection, not a stub ───────────────────────
//
// Everything above uses a stub that copies dispatchIncoming's shape. This checks
// the shape really is the connection's: a genuine RpcConnection pair, a provider
// that HOLDS its reply until the test says so, and a caller that cancels in
// between. The reply then travels the real wire into the real dispatchIncoming,
// which must find no registration and drop it in silence.
TEST(PlainCancelPendingRaceTest, ARealConnectionDropsAReplyThatArrivesAfterCancel)
{
    using LocalSocket     = boost::asio::local::stream_protocol::socket;
    using LocalConnection = RpcConnection<LocalSocket>;

    // A provider that answers nothing until told to.
    class HeldReplyProvider : public IncomingCallHandler {
    public:
        void onCall(const CallMessage& req, CallReply reply) override
        {
            std::lock_guard<std::mutex> g(mu);
            id = req.id;
            held = std::move(reply);
            arrived.fetch_add(1);
        }
        void onMethods(const MethodsMessage& req, MethodsReply reply) override
        {
            MethodsResultMessage r; r.id = req.id; r.ok = true; reply(std::move(r));
        }
        void onSubscribe(const SubscribeMessage&, EventSink, const void*) override {}
        void onUnsubscribe(const UnsubscribeMessage&, const void*) override {}
        void onConnectionClosed(const void*) override {}
        void onToken(const TokenMessage&) override {}

        void answer()
        {
            CallReply r;
            std::uint64_t rid = 0;
            {
                std::lock_guard<std::mutex> g(mu);
                r = std::move(held);
                rid = id;
            }
            if (!r) return;
            ResultMessage res;
            res.id = rid; res.ok = true;
            res.value = RpcValue{static_cast<int64_t>(1)};
            r(std::move(res));
        }

        std::mutex       mu;
        CallReply        held;
        std::uint64_t    id = 0;
        std::atomic<int> arrived{0};
    };

    boost::asio::io_context ioc;
    auto guard = boost::asio::make_work_guard(ioc);
    std::thread worker([&ioc] { ioc.run(); });

    LocalSocket a(ioc), b(ioc);
    boost::system::error_code ec;
    boost::asio::local::connect_pair(a, b, ec);
    ASSERT_FALSE(ec) << ec.message();

    HeldReplyProvider provider;
    auto codec    = std::make_shared<JsonCodec>();
    auto client   = std::make_shared<LocalConnection>(std::move(a), codec, nullptr);
    auto provider_conn =
        std::make_shared<LocalConnection>(std::move(b), codec, &provider);
    client->start();
    provider_conn->start();

    std::atomic<int> handlerRuns{0};
    CallMessage msg;
    msg.id     = client->nextId();
    msg.object = "real_probe";
    msg.method = "ping";
    const std::uint64_t id = msg.id;
    client->sendCallAsync(std::move(msg), [&handlerRuns](ResultMessage) {
        handlerRuns.fetch_add(1);
    });

    // Wait for the provider to be holding the call, then give up on it.
    QElapsedTimer t; t.start();
    while (provider.arrived.load() == 0 && t.elapsed() < 5000)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ASSERT_EQ(provider.arrived.load(), 1) << "the call never reached the provider";

    client->cancelPending(id);

    // Only now does the answer go out, over the real wire.
    provider.answer();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    EXPECT_EQ(handlerRuns.load(), 0)
        << "a reply arriving after cancelPending() must find no registration — "
           "that is the semantic cancelPending() exists for, and it is the half "
           "of the comment that IS true";

    client->stop();
    provider_conn->stop();
    guard.reset();
    ioc.stop();
    worker.join();
}
