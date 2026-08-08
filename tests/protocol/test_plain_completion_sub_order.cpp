// The completion subscription has to be UP, not merely CLAIMED, before the
// call that depends on it goes out.
//
// ensureCompletionSub() used to raise a flag under the rendezvous mutex and then
// RELEASE that mutex before subscribing. Two threads entering callMethod() on
// the same fresh object is enough: the second reads "subscribed", builds its
// Call and puts it on the wire while the first has not enqueued the Subscribe
// frame yet. A "multi" provider that answers such a call quickly then emits its
// completion into a subscription nobody has registered — the host finds no sink
// for that connection and DROPS the event — and the caller waits out its whole
// timeout for a result that was computed and thrown away.
//
// A LOST COMPLETION, NOT A CRASH, and that is why it survived: the failure looks
// like a slow provider or a flaky network, arrives seconds after the code that
// caused it, and leaves nothing behind to find.
//
// Two tests, deliberately not one:
//
//   * the RAW WIRE case is the gate. A socketpair, a real RpcConnection on each
//     end, and a provider that stamps a sequence number on every frame it
//     receives — so "the Call arrived before the Subscribe" is observed rather
//     than inferred, and a drop is counted at the instant of emission.
//   * the REAL STACK case is the consequence. PlainTransportHost + ModuleProxy
//     + a "multi" module, nothing instrumented at all: the drop, if it happens,
//     is PlainTransportHost::fanOutEvent's, and all this test can see is a
//     caller that timed out.
//
// BOTH ARE VALIDATED DETECTORS, and validated against the real thing rather
// than an imitation of it: this file compiles unmodified on master, where
// ensureCompletionSub() still raises its flag under the rendezvous mutex and
// drops the mutex before subscribing. Both go RED there, in every run. Numbers
// from four runs on an aarch64-darwin box:
//
//   raw wire     18 / 26 / 27 / 28 of 250 rounds put the Call on the wire ahead
//                of the Subscribe frame; every one of those rounds also dropped
//                the completion and timed its caller out
//   real stack   6 to 10 of 500 calls lost their completion at
//                PlainTransportHost::fanOutEvent and timed out
//
// That is also the attribution: the bug is on master, so it predates the
// lifetime fix this ships with instead of being introduced by it.
//
// Reproducing the check costs a worktree on master, a copy of this file into
// tests/protocol and one line in that directory's CMakeLists. Deliberately
// that, and not a compile-time switch that would put a second, knowingly wrong
// ensureCompletionSub() into the shipped transport.

#include <gtest/gtest.h>

#include "incoming_call_handler.h"
#include "json_codec.h"
#include "logos_async_dispatch.h"
#include "logos_provider_interface.h"
#include "logos_transport_config.h"
#include "module_proxy.h"
#include "plain_logos_object.h"
#include "plain_transport_connection.h"
#include "plain_transport_host.h"
#include "qvariant_rpc_value.h"
#include "rpc_connection.h"
#include "rpc_message.h"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/local/connect_pair.hpp>
#include <boost/asio/local/stream_protocol.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using namespace logos::plain;

namespace {

using LocalSocket     = boost::asio::local::stream_protocol::socket;
using LocalConnection = RpcConnection<LocalSocket>;

// An io_context with its own thread, so the two ends of the socketpair really
// do run concurrently instead of taking turns on one worker.
class Io {
public:
    Io() : m_guard(boost::asio::make_work_guard(m_ioc)),
           m_thread([this] { m_ioc.run(); }) {}
    ~Io() { m_guard.reset(); m_ioc.stop(); if (m_thread.joinable()) m_thread.join(); }
    boost::asio::io_context& ctx() { return m_ioc; }
private:
    boost::asio::io_context m_ioc;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> m_guard;
    std::thread m_thread;
};

// What the provider saw, per round.
struct Round {
    std::atomic<int> seq{0};
    std::atomic<int> subscribeSeq{-1};   // when the completion Subscribe landed
    std::atomic<int> firstCallSeq{-1};   // when the first Call landed
    std::atomic<int> dropped{0};         // completions emitted with no sink
};

// Answers every call with a PENDING SENTINEL and completes it AT ONCE — the
// limiting case of a "multi" module whose worker finishes immediately, and a
// legal one. Drops the completion when nothing is subscribed, which is what
// PlainTransportHost::fanOutEvent does with an empty sink set.
class InstantMultiProvider : public IncomingCallHandler {
public:
    void beginRound(Round* r)
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_round = r;
        m_sink = nullptr;
    }

    void onCall(const CallMessage& req, CallReply reply) override
    {
        Round* r = nullptr;
        EventSink sink;
        {
            std::lock_guard<std::mutex> g(m_mu);
            r = m_round;
            sink = m_sink;
        }
        if (r) {
            const int s = r->seq.fetch_add(1);
            int expected = -1;
            r->firstCallSeq.compare_exchange_strong(expected, s);
        }

        const std::string callId = "cid-" + std::to_string(req.id);
        QVariantMap pending;
        pending[logos::pendingCallKey()] = QString::fromStdString(callId);
        ResultMessage res;
        res.id    = req.id;
        res.ok    = true;
        res.value = qvariantToRpcValue(QVariant(pending));
        reply(std::move(res));

        EventMessage evt;
        evt.object    = req.object;
        evt.eventName = logos::callCompleteEvent().toStdString();
        evt.data      = qvariantListToRpcList(
            QVariantList{ QString::fromStdString(callId), QVariant(7) });
        if (sink)      sink(std::move(evt));
        else if (r)    r->dropped.fetch_add(1);
    }

    void onMethods(const MethodsMessage& req, MethodsReply reply) override
    {
        MethodsResultMessage res; res.id = req.id; res.ok = true;
        reply(std::move(res));
    }

    void onSubscribe(const SubscribeMessage& req, EventSink sink, const void*) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (m_round && req.eventName == logos::callCompleteEvent().toStdString()) {
            const int s = m_round->seq.fetch_add(1);
            int expected = -1;
            m_round->subscribeSeq.compare_exchange_strong(expected, s);
        }
        m_sink = std::move(sink);
    }

    void onUnsubscribe(const UnsubscribeMessage&, const void*) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_sink = nullptr;
    }

    void onConnectionClosed(const void*) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_sink = nullptr;
    }

    void onToken(const TokenMessage&) override {}

private:
    std::mutex m_mu;
    Round*     m_round = nullptr;
    EventSink  m_sink;
};

// The same provider behaviour as a real module behind ModuleProxy.
class InstantMultiModule : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList&) override
    {
        if (method != QLatin1String("fast")) return QVariant();
        const QString callId = QStringLiteral("mc-%1").arg(
            static_cast<qulonglong>(m_counter.fetch_add(1)));
        std::thread([this, callId]() {
            if (m_cb) m_cb(logos::callCompleteEvent(),
                           QVariantList{ callId, QVariant(7) });
        }).detach();
        QVariantMap pending;
        pending[logos::pendingCallKey()] = callId;
        return pending;
    }
    QJsonArray getMethods() override { return QJsonArray{}; }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    void setEventListener(EventCallback cb) override { m_cb = std::move(cb); }
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("fastmod"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }
private:
    EventCallback m_cb;
    std::atomic<unsigned long long> m_counter{0};
};

QCoreApplication* ensureApp()
{
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance()) new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

// Two threads, released together, each making the FIRST call of a fresh
// object's life. Records whether their two calls actually overlapped, because a
// green run of a test that stopped racing proves nothing.
struct TwoCallers {
    std::atomic<int>  ready{0};
    std::atomic<bool> go{false};
    std::atomic<long long> lastEnter{0};
    std::atomic<long long> firstExit{0x7fffffffffffffffLL};
    std::atomic<int>  timeouts{0};
    std::atomic<int>  answered{0};

    template <typename Call>
    void run(Call call)
    {
        auto body = [&]() {
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) { /* spin to one instant */ }
            const auto t0 = std::chrono::steady_clock::now().time_since_epoch().count();
            long long prev = lastEnter.load();
            while (t0 > prev && !lastEnter.compare_exchange_weak(prev, t0)) {}
            call(*this);
            const auto t1 = std::chrono::steady_clock::now().time_since_epoch().count();
            prev = firstExit.load();
            while (t1 < prev && !firstExit.compare_exchange_weak(prev, t1)) {}
        };
        std::thread a(body), b(body);
        while (ready.load() < 2) { /* spin */ }
        go.store(true, std::memory_order_release);
        a.join();
        b.join();
    }

    bool overlapped() const { return lastEnter.load() < firstExit.load(); }
};

}  // namespace

// ── the gate: the wire order itself ─────────────────────────────────────────
TEST(PlainCompletionSubOrderTest, AConcurrentFirstCallCannotOutrunTheSubscription)
{
    constexpr int kRounds    = 250;
    constexpr int kTimeoutMs = 150;   // a lost completion costs this much, once

    Io clientIo, serverIo;
    auto codec = std::make_shared<JsonCodec>();

    LocalSocket clientSock(clientIo.ctx());
    LocalSocket serverSock(serverIo.ctx());
    boost::system::error_code ec;
    boost::asio::local::connect_pair(clientSock, serverSock, ec);
    ASSERT_FALSE(ec) << "connect_pair failed: " << ec.message();

    InstantMultiProvider provider;
    auto serverConn = std::make_shared<LocalConnection>(std::move(serverSock), codec,
                                                        &provider);
    auto clientConn = std::make_shared<LocalConnection>(std::move(clientSock), codec,
                                                        nullptr);
    serverConn->start();
    clientConn->start();

    int inverted = 0, roundsWithDrop = 0, timeouts = 0, answered = 0, raced = 0;

    for (int r = 0; r < kRounds; ++r) {
        Round round;
        provider.beginRound(&round);

        // A fresh handle every round: the subscription is per-object, so this is
        // the only way to keep making FIRST calls. release() unsubscribes, so
        // the next round starts with no sink at the provider either.
        auto* obj = new PlainLogosObject("order_probe", clientConn);

        TwoCallers callers;
        callers.run([&](TwoCallers& c) {
            logos::CallError err;
            const QVariant v = obj->callMethodWithError(
                QStringLiteral("tok"), QStringLiteral("fast"), QVariantList{},
                kTimeoutMs, &err);
            if (err.code == "timeout")    c.timeouts.fetch_add(1);
            else if (v.toInt() == 7)      c.answered.fetch_add(1);
        });

        const int sub  = round.subscribeSeq.load();
        const int call = round.firstCallSeq.load();
        if (call >= 0 && sub >= 0 && call < sub) ++inverted;
        if (round.dropped.load() > 0)            ++roundsWithDrop;
        if (callers.overlapped())                ++raced;
        timeouts += callers.timeouts.load();
        answered += callers.answered.load();

        obj->release();
    }

    std::cout << "  " << kRounds << " rounds x 2 concurrent first calls -> "
              << "call-before-subscribe=" << inverted
              << " dropped-completions=" << roundsWithDrop
              << " caller timeouts=" << timeouts
              << " answered=" << answered
              << " (rounds where the two callers genuinely overlapped: "
              << raced << ")" << std::endl;

    // The race has to have been RUN, or the rest of this proves nothing.
    EXPECT_GT(raced, kRounds / 2)
        << "the two callers did not overlap — this test is racing nothing";

    EXPECT_EQ(inverted, 0)
        << inverted << " rounds put a Call on the wire ahead of the Subscribe "
                       "frame the completion depends on";
    EXPECT_EQ(roundsWithDrop, 0)
        << roundsWithDrop << " completions were emitted into a subscription "
                             "that did not exist yet, and dropped";
    EXPECT_EQ(timeouts, 0)
        << timeouts << " callers waited out their whole timeout for a result "
                       "the provider had already computed";
    EXPECT_EQ(answered, kRounds * 2);

    provider.beginRound(nullptr);
    clientConn->stop();
    serverConn->stop();
}

// ── the consequence, through the shipping stack ─────────────────────────────
TEST(PlainCompletionSubOrderTest, TheSameRaceThroughTheRealHost)
{
    ensureApp();
    constexpr int kRounds    = 250;
    constexpr int kTimeoutMs = 300;

    LogosTransportConfig cfg;
    cfg.protocol = LogosProtocol::Tcp;
    cfg.host = "127.0.0.1";
    cfg.port = 0;

    auto host = std::make_unique<PlainTransportHost>(cfg);
    ASSERT_TRUE(host->start());

    InstantMultiModule mod;
    ModuleProxy proxy(&mod);
    proxy.saveToken(QStringLiteral("core"), QStringLiteral("tok"));
    ASSERT_TRUE(host->publishObject("fastmod", &proxy));

    const QString endpoint = host->endpoint();
    LogosTransportConfig ccfg = cfg;
    ccfg.port = endpoint.mid(endpoint.lastIndexOf(':') + 1).toUShort();
    auto conn = std::make_unique<PlainTransportConnection>(ccfg);
    ASSERT_TRUE(conn->connectToHost());

    int timeouts = 0, answered = 0, raced = 0;

    for (int r = 0; r < kRounds; ++r) {
        LogosObject* obj = conn->requestObject(QStringLiteral("fastmod"), 2000);
        ASSERT_NE(obj, nullptr);
        auto* ch = dynamic_cast<LogosObjectErrorChannel*>(obj);
        ASSERT_NE(ch, nullptr);

        // The provider's thread is this one (ModuleProxy queues its events onto
        // it, and PlainTransportHost::onCall queues the dispatch onto it), so
        // the two callers have to run elsewhere while this one pumps.
        TwoCallers callers;
        std::atomic<int> done{0};
        std::thread driver([&]() {
            callers.run([&](TwoCallers& c) {
                logos::CallError err;
                const QVariant v = ch->callMethodWithError(
                    QStringLiteral("tok"), QStringLiteral("fast"), QVariantList{},
                    kTimeoutMs, &err);
                if (err.code == "timeout") c.timeouts.fetch_add(1);
                else if (v.toInt() == 7)   c.answered.fetch_add(1);
                done.fetch_add(1);
            });
        });

        QElapsedTimer t; t.start();
        while (done.load() < 2 && t.elapsed() < 8000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        driver.join();

        timeouts += callers.timeouts.load();
        answered += callers.answered.load();
        if (callers.overlapped()) ++raced;
        obj->release();
    }

    std::cout << "  " << kRounds << " rounds through PlainTransportHost -> "
              << "caller timeouts=" << timeouts << " answered=" << answered
              << " (overlapping rounds: " << raced << ")" << std::endl;

    EXPECT_GT(raced, kRounds / 2)
        << "the two callers did not overlap — this test is racing nothing";
    EXPECT_EQ(timeouts, 0)
        << timeouts << " calls lost their completion at the host's fan-out";
    EXPECT_EQ(answered, kRounds * 2);

    host.reset();
}
