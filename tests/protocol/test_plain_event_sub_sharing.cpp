// One RpcConnection, many handles: a subscription must belong to the HANDLE
// that made it, not to the (object, event) pair.
//
// RpcConnection::m_eventCallbacks is keyed by (object, eventName) and ASSIGNED.
// A single RpcConnection is shared by every PlainLogosObject a
// PlainTransportConnection hands out — and requestObject() mints a fresh handle
// per acquire — so the SECOND handle to subscribe to the same event on the same
// module silently takes the first one's channel away. That includes the deferred
// ("multi") completion channel every handle subscribes to on its first call, so
// the visible symptom is a call that used to answer in single-digit milliseconds
// waiting out its entire timeout and returning nothing.
//
// NO CONCURRENCY IS REQUIRED. Two handles and three sequential calls reproduce
// it, which is why these tests are written straight-line rather than as races.
//
// VALIDATED AGAINST THE REAL PRE-FIX CODE, not against an imitation of it. Every
// test below except the last three compiles UNMODIFIED on
// feat/plain-async-io-fold (#46, cf1b9b0) and on master (c1b0a0f) — neither
// causes or fixes this — and every one of them goes RED there. Numbers from
// aarch64-darwin, Qt 6.9.2, Debug, on cf1b9b0 (master is within noise of it):
//
//   ASecondHandleDoesNotStealTheFirstsCompletionChannel
//        handle A alone         -> 8/8 answered, 0 ms avg
//        handle A once B exists -> 0/8 answered, 1508 ms avg, 8 timeouts
//   TheSameTheftThroughTheRealHost
//        handle A once B exists -> 0/4 answered, 1501 ms avg, 4 timeouts
//   EveryHandleGetsEveryEmission        A=0 of 5, B=5, wildcard=5
//   ANamedAndAWildcardSubscriberEachGetOneCopy
//                                       named=10 and wildcard=10 for 5 emissions
//   ReleasingOneHandleLeavesTheOtherSubscribed
//                                       B=0 of 5 after A left; host sinks=0
//   SubscriptionsSurviveChurn           214 of 480 audited slots lost a delivery
//   ACompletionThatOvertakesItsOwnResultIsStillDelivered
//                                       sync 4/6, async 2/4
//   ANewConsumerAgainstAnOldHost        A=0 of 4; the old host was unsubscribed
//                                       while B was still subscribed
//   AnOldConsumersFrameSequenceAgainstTheNewHost
//                                       2 copies of one emission on the wire
//
//   TeardownRemovesEverySubscription    PASSES pre-fix — a PIN, not a detector
//   PlainParkedCompletionGateTest.*     unit tests of code this change adds, so
//                                       there is no pre-fix tree to run them on
//
// The last three lines are the ones worth reading twice: a green teardown test
// proves nothing about this bug. Only the tests that assert a SECOND live
// subscriber still has its channel detect it.
//
// Reproducing the check costs a worktree at cf1b9b0, a copy of this file into
// tests/protocol with everything from PlainParkedCompletionGateTest onward cut,
// and one line in that directory's CMakeLists.

#include <gtest/gtest.h>

#include "incoming_call_handler.h"
#include "json_codec.h"
#include "logos_async_dispatch.h"
#include "logos_call_error.h"
#include "logos_provider_interface.h"
#include "logos_transport_config.h"
#include "module_proxy.h"
#include "plain_logos_object.h"
#include "plain_transport_connection.h"
#include "plain_transport_host.h"
#include "qvariant_rpc_value.h"
#include "rpc_connection.h"
#include "rpc_framing.h"
#include "rpc_message.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/local/connect_pair.hpp>
#include <boost/asio/local/stream_protocol.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace logos::plain;

namespace {

using LocalSocket     = boost::asio::local::stream_protocol::socket;
using LocalConnection = RpcConnection<LocalSocket>;

// An io_context with its own thread, so both ends of the socketpair really run
// concurrently instead of taking turns on one worker.
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

// -----------------------------------------------------------------------------
// MirrorProvider — a provider whose sink bookkeeping is a LINE-FOR-LINE mirror of
// PlainTransportHost's: sinks live in sinksByEvent[eventName][connectionId],
// assigned on Subscribe and erased by (eventName, connectionId) on Unsubscribe,
// and fan-out visits the named set followed by the wildcard set.
//
// Deliberately a mirror rather than the real host: these tests have to be able to
// count the Subscribe / Unsubscribe frames that actually reached a provider, and
// to push an event at a connection that is no longer subscribed. The real host is
// exercised separately by TheSameTheftThroughTheRealHost, so a divergence between
// this mirror and the shipping host cannot make a broken build look green.
// -----------------------------------------------------------------------------
class MirrorProvider : public IncomingCallHandler {
public:
    // Fan out the way the host did BEFORE the per-connection de-duplication:
    // every matching sink, so a connection subscribed both by name and by
    // wildcard gets two copies. Used to stand in for an OLD host.
    void setLegacyFanOut(bool on) { m_dedupePerConnection = !on; }
    // Push the completion event BEFORE the Result it answers, which is what a
    // "multi" module whose worker finishes inside callMethod really does.
    void setCompletionBeforeResult(bool on) { m_completionFirst = on; }

    // Every call answers with a pending sentinel and pushes the completion right
    // after it, over the completion channel — the limiting case of a "multi"
    // module whose worker finishes at once, and a legal one.
    void onCall(const CallMessage& req, CallReply reply) override
    {
        const std::string callId = "cid-" + std::to_string(req.id);
        QVariantMap pending;
        pending[logos::pendingCallKey()] = QString::fromStdString(callId);

        ResultMessage res;
        res.id    = req.id;
        res.ok    = true;
        res.value = qvariantToRpcValue(QVariant(pending));

        const auto pushCompletion = [&] {
            emitEvent(req.object, logos::callCompleteEvent().toStdString(),
                      QVariantList{ QString::fromStdString(callId),
                                    QVariant(kDeferredAnswer) });
        };
        if (m_completionFirst) {
            pushCompletion();
            reply(std::move(res));
        } else {
            reply(std::move(res));
            pushCompletion();
        }
    }

    void onMethods(const MethodsMessage& req, MethodsReply reply) override
    {
        MethodsResultMessage res; res.id = req.id; res.ok = true;
        reply(std::move(res));
    }

    void onSubscribe(const SubscribeMessage& req, EventSink sink,
                     const void* connectionId) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        ++m_subscribeFrames;
        m_sinks[req.eventName][connectionId] = std::move(sink);
    }

    void onUnsubscribe(const UnsubscribeMessage& req, const void* connectionId) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        ++m_unsubscribeFrames;
        auto it = m_sinks.find(req.eventName);
        if (it == m_sinks.end()) return;
        it->second.erase(connectionId);
        if (it->second.empty()) m_sinks.erase(it);
    }

    void onConnectionClosed(const void* connectionId) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        for (auto it = m_sinks.begin(); it != m_sinks.end(); ) {
            it->second.erase(connectionId);
            if (it->second.empty()) it = m_sinks.erase(it);
            else ++it;
        }
    }

    void onToken(const TokenMessage&) override {}

    // Same walk as PlainTransportHost::fanOutEvent, de-duplication included: one
    // copy per CONNECTION, however many of its sinks match. A connection
    // subscribed both by name and by wildcard is one subscriber as far as the
    // wire is concerned. ANamedAndAWildcardSubscriberEachGetOneCopy pins that on
    // the real host rather than on this mirror.
    void emitEvent(const std::string& object, const std::string& event,
                   const QVariantList& data)
    {
        std::vector<EventSink> sinks;
        {
            std::lock_guard<std::mutex> g(m_mu);
            std::set<const void*> seen;
            for (const auto& which : { event, std::string{} }) {
                auto it = m_sinks.find(which);
                if (it == m_sinks.end()) continue;
                for (auto& [id, sink] : it->second)
                    if (!m_dedupePerConnection || seen.insert(id).second)
                        sinks.push_back(sink);
                if (event.empty()) break;   // named == wildcard, don't visit twice
            }
        }
        for (auto& sink : sinks) {
            EventMessage evt;
            evt.object    = object;
            evt.eventName = event;
            evt.data      = qvariantListToRpcList(data);
            try { sink(std::move(evt)); } catch (...) {}
        }
    }

    int sinkCount() const
    {
        std::lock_guard<std::mutex> g(m_mu);
        int n = 0;
        for (const auto& [event, byConn] : m_sinks) n += static_cast<int>(byConn.size());
        return n;
    }

    int subscribeFrames() const
    {
        std::lock_guard<std::mutex> g(m_mu); return m_subscribeFrames;
    }
    int unsubscribeFrames() const
    {
        std::lock_guard<std::mutex> g(m_mu); return m_unsubscribeFrames;
    }

    static constexpr int kDeferredAnswer = 7;

private:
    mutable std::mutex m_mu;
    std::map<std::string, std::map<const void*, EventSink>> m_sinks;
    int  m_subscribeFrames    = 0;
    int  m_unsubscribeFrames  = 0;
    bool m_dedupePerConnection = true;
    bool m_completionFirst     = false;
};

// The same "answer with a sentinel, complete at once" behaviour as a real module
// behind ModuleProxy, for the run through the shipping host.
class InstantMultiModule : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList&) override
    {
        if (method != QLatin1String("work")) return QVariant();
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
    QString providerName() const override { return QStringLiteral("multimod"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }
private:
    EventCallback m_cb;
    std::atomic<unsigned long long> m_counter{0};
};

// Emits one named event per call, from inside callMethod — the ordinary shape of
// a module that pushes on demand.
class EmittingModule : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList&) override
    {
        if (method != QLatin1String("ping")) return QVariant();
        if (m_cb) m_cb(QStringLiteral("tick"), QVariantList{ QVariant(1) });
        return QVariant(true);
    }
    QJsonArray getMethods() override { return QJsonArray{}; }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    void setEventListener(EventCallback cb) override { m_cb = std::move(cb); }
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("emitmod"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }
private:
    EventCallback m_cb;
};

QCoreApplication* ensureApp()
{
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance()) new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

// A socketpair with a live RpcConnection on each end.
struct WirePair {
    Io clientIo, serverIo;
    std::shared_ptr<LocalConnection> client;
    std::shared_ptr<LocalConnection> server;

    explicit WirePair(IncomingCallHandler* provider)
    {
        auto codec = std::make_shared<JsonCodec>();
        LocalSocket clientSock(clientIo.ctx());
        LocalSocket serverSock(serverIo.ctx());
        boost::system::error_code ec;
        boost::asio::local::connect_pair(clientSock, serverSock, ec);
        if (ec) throw std::runtime_error("connect_pair: " + ec.message());
        server = std::make_shared<LocalConnection>(std::move(serverSock), codec, provider);
        client = std::make_shared<LocalConnection>(std::move(clientSock), codec, nullptr);
        server->start();
        client->start();
    }
    ~WirePair() { client->stop(); server->stop(); }
};

// A round trip that proves every frame written before it has been applied.
//
// Frames go out in post order on the connection's strand and are dispatched in
// arrival order on the peer's, so a reply to a request issued LAST cannot arrive
// before the effects of everything issued earlier — in either direction. getMethods
// blocks on that reply, so returning from it means both the Subscribe/Unsubscribe
// frames this side wrote AND the Event frames the peer wrote before answering have
// been processed. That is what lets the assertions below be exact counts rather
// than "wait a bit and hope"; a sleep here is a coin flip on a loaded CI runner.
//
// The probe is a handle that never subscribes to anything (getMethods does not
// touch the completion channel), so it changes nothing it is measuring.
void wireBarrier(PlainLogosObject* probe) { probe->getMethods(); }

// Spin until `pred` holds or `budgetMs` elapses. Returns whether it held.
template <typename Pred>
bool waitFor(Pred pred, int budgetMs)
{
    QElapsedTimer t; t.start();
    while (!pred()) {
        if (t.elapsed() > budgetMs) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// A counter the subscription callback owns a share of, so it stays valid after
// the handle that created it has been deleted.
using Counter = std::shared_ptr<std::atomic<int>>;
Counter makeCounter() { return std::make_shared<std::atomic<int>>(0); }

}  // namespace

// ── the headline: the completion channel, over the raw wire ─────────────────
//
// Three sequential calls, no threads. Handle A answers, handle B is created and
// answers, and then handle A is asked the same question again.
TEST(PlainEventSubSharingTest, ASecondHandleDoesNotStealTheFirstsCompletionChannel)
{
    constexpr int kTimeoutMs = 1500;
    constexpr int kRounds    = 8;

    MirrorProvider provider;
    WirePair wire(&provider);

    int aloneAnswered = 0, sharedAnswered = 0, sharedTimeouts = 0;
    qint64 aloneMs = 0, sharedMs = 0;

    for (int r = 0; r < kRounds; ++r) {
        auto* a = new PlainLogosObject("multimod", wire.client);

        logos::CallError err;
        QElapsedTimer t; t.start();
        QVariant v = a->callMethodWithError(QStringLiteral("tok"),
                                            QStringLiteral("work"),
                                            QVariantList{}, kTimeoutMs, &err);
        aloneMs += t.elapsed();
        if (v.toInt() == MirrorProvider::kDeferredAnswer) ++aloneAnswered;

        // B merely exists and makes one call of its own — which is all it takes,
        // because the first call is what subscribes.
        auto* b = new PlainLogosObject("multimod", wire.client);
        err = logos::CallError{};
        v = b->callMethodWithError(QStringLiteral("tok"), QStringLiteral("work"),
                                   QVariantList{}, kTimeoutMs, &err);
        EXPECT_EQ(v.toInt(), MirrorProvider::kDeferredAnswer)
            << "the SECOND handle should always work — it owns the channel pre-fix";

        // Now ask A again.
        err = logos::CallError{};
        t.restart();
        v = a->callMethodWithError(QStringLiteral("tok"), QStringLiteral("work"),
                                   QVariantList{}, kTimeoutMs, &err);
        sharedMs += t.elapsed();
        if (v.toInt() == MirrorProvider::kDeferredAnswer) ++sharedAnswered;
        if (err.code == "timeout")                        ++sharedTimeouts;

        a->release();
        b->release();
    }

    std::cout << "  handle A alone:         " << aloneAnswered << "/" << kRounds
              << " answered, " << (aloneMs / kRounds) << " ms avg\n"
              << "  handle A once B exists: " << sharedAnswered << "/" << kRounds
              << " answered, " << (sharedMs / kRounds) << " ms avg, "
              << sharedTimeouts << " timeouts" << std::endl;

    EXPECT_EQ(aloneAnswered, kRounds) << "the single-handle case is the baseline";
    EXPECT_EQ(sharedTimeouts, 0)
        << sharedTimeouts << " of " << kRounds << " calls on handle A waited out "
           "the whole timeout because handle B took its completion channel";
    EXPECT_EQ(sharedAnswered, kRounds);
}

// ── the same theft, through the shipping stack ──────────────────────────────
//
// PlainTransportHost + ModuleProxy + a "multi" module, and handles that come from
// requestObject() rather than from a constructor — the shape the transport hands
// out in production, where a fresh handle per acquire is the norm.
TEST(PlainEventSubSharingTest, TheSameTheftThroughTheRealHost)
{
    ensureApp();
    constexpr int kTimeoutMs = 1500;
    constexpr int kRounds    = 4;

    LogosTransportConfig cfg;
    cfg.protocol = LogosProtocol::Tcp;
    cfg.host = "127.0.0.1";
    cfg.port = 0;

    auto host = std::make_unique<PlainTransportHost>(cfg);
    ASSERT_TRUE(host->start());

    InstantMultiModule mod;
    ModuleProxy proxy(&mod);
    proxy.saveToken(QStringLiteral("core"), QStringLiteral("tok"));
    ASSERT_TRUE(host->publishObject("multimod", &proxy));

    const QString endpoint = host->endpoint();
    LogosTransportConfig ccfg = cfg;
    ccfg.port = endpoint.mid(endpoint.lastIndexOf(':') + 1).toUShort();
    auto conn = std::make_unique<PlainTransportConnection>(ccfg);
    ASSERT_TRUE(conn->connectToHost());

    std::atomic<int> answered{0}, timeouts{0};
    std::atomic<qint64> sharedMs{0};

    for (int r = 0; r < kRounds; ++r) {
        LogosObject* a = conn->requestObject(QStringLiteral("multimod"), 2000);
        LogosObject* b = conn->requestObject(QStringLiteral("multimod"), 2000);
        ASSERT_NE(a, nullptr);
        ASSERT_NE(b, nullptr);
        auto* ca = dynamic_cast<LogosObjectErrorChannel*>(a);
        auto* cb = dynamic_cast<LogosObjectErrorChannel*>(b);
        ASSERT_NE(ca, nullptr);
        ASSERT_NE(cb, nullptr);

        // The module's events and the host's dispatch both land on THIS thread,
        // so the calls have to be made from another one while this one pumps.
        std::atomic<bool> done{false};
        std::thread driver([&]() {
            logos::CallError err;
            ca->callMethodWithError(QStringLiteral("tok"), QStringLiteral("work"),
                                    QVariantList{}, kTimeoutMs, &err);
            cb->callMethodWithError(QStringLiteral("tok"), QStringLiteral("work"),
                                    QVariantList{}, kTimeoutMs, &err);
            err = logos::CallError{};
            QElapsedTimer t; t.start();
            const QVariant v = ca->callMethodWithError(
                QStringLiteral("tok"), QStringLiteral("work"),
                QVariantList{}, kTimeoutMs, &err);
            sharedMs.fetch_add(t.elapsed());
            if (v.toInt() == 7)        answered.fetch_add(1);
            if (err.code == "timeout") timeouts.fetch_add(1);
            done.store(true);
        });

        QElapsedTimer pump; pump.start();
        while (!done.load() && pump.elapsed() < 3 * kTimeoutMs + 2000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        driver.join();

        a->release();
        b->release();
    }

    std::cout << "  handle A once B exists (real host): " << answered.load() << "/"
              << kRounds << " answered, " << (sharedMs.load() / kRounds)
              << " ms avg, " << timeouts.load() << " timeouts" << std::endl;

    EXPECT_EQ(timeouts.load(), 0)
        << "a handle acquired before another one lost its completion channel";
    EXPECT_EQ(answered.load(), kRounds);

    host.reset();
}

// ── the general case: any event, any number of handles ──────────────────────
TEST(PlainEventSubSharingTest, EveryHandleGetsEveryEmission)
{
    constexpr int kEmissions = 5;

    MirrorProvider provider;
    WirePair wire(&provider);

    auto* probe = new PlainLogosObject("evmod", wire.client);
    auto* a = new PlainLogosObject("evmod", wire.client);
    auto* b = new PlainLogosObject("evmod", wire.client);
    auto* c = new PlainLogosObject("evmod", wire.client);

    Counter na = makeCounter(), nb = makeCounter(), nc = makeCounter();
    a->onEvent(QStringLiteral("tick"), [na](const QString&, const QVariantList&) { na->fetch_add(1); });
    b->onEvent(QStringLiteral("tick"), [nb](const QString&, const QVariantList&) { nb->fetch_add(1); });
    // A WILDCARD subscriber is a third shape on the same object and must not
    // clobber, or be clobbered by, the two named ones.
    c->onEvent(QString(), [nc](const QString&, const QVariantList&) { nc->fetch_add(1); });

    wireBarrier(probe);
    ASSERT_EQ(provider.sinkCount(), 2)
        << "expected one named sink and one wildcard sink for this connection";

    for (int i = 0; i < kEmissions; ++i)
        provider.emitEvent("evmod", "tick", QVariantList{ QVariant(i) });
    wireBarrier(probe);

    std::cout << "  " << kEmissions << " emissions -> A=" << na->load()
              << " B=" << nb->load() << " wildcard=" << nc->load() << std::endl;

    EXPECT_EQ(na->load(), kEmissions) << "the FIRST subscriber lost its channel";
    EXPECT_EQ(nb->load(), kEmissions);
    EXPECT_EQ(nc->load(), kEmissions);

    a->release();
    b->release();
    c->release();
    probe->release();
}

// ── one copy per connection, however many of its sinks match ────────────────
//
// The other half of "the consumer demultiplexes": once it fans a delivery out to
// every local subscriber, the HOST must stop sending a connection two copies of
// the same event because it matched both by name and by wildcard. Runs against
// the shipping PlainTransportHost, because that double-send is its own.
TEST(PlainEventSubSharingTest, ANamedAndAWildcardSubscriberEachGetOneCopy)
{
    ensureApp();
    constexpr int kPings = 5;

    LogosTransportConfig cfg;
    cfg.protocol = LogosProtocol::Tcp;
    cfg.host = "127.0.0.1";
    cfg.port = 0;

    auto host = std::make_unique<PlainTransportHost>(cfg);
    ASSERT_TRUE(host->start());

    EmittingModule mod;
    ModuleProxy proxy(&mod);
    proxy.saveToken(QStringLiteral("core"), QStringLiteral("tok"));
    ASSERT_TRUE(host->publishObject("emitmod", &proxy));

    const QString endpoint = host->endpoint();
    LogosTransportConfig ccfg = cfg;
    ccfg.port = endpoint.mid(endpoint.lastIndexOf(':') + 1).toUShort();
    auto conn = std::make_unique<PlainTransportConnection>(ccfg);
    ASSERT_TRUE(conn->connectToHost());

    LogosObject* named    = conn->requestObject(QStringLiteral("emitmod"), 2000);
    LogosObject* wildcard = conn->requestObject(QStringLiteral("emitmod"), 2000);
    ASSERT_NE(named, nullptr);
    ASSERT_NE(wildcard, nullptr);

    Counter nn = makeCounter(), nw = makeCounter();
    named->onEvent(QStringLiteral("tick"), [nn](const QString&, const QVariantList&) { nn->fetch_add(1); });
    wildcard->onEvent(QString(), [nw](const QString&, const QVariantList&) { nw->fetch_add(1); });

    std::atomic<int> sent{0};
    std::thread driver([&]() {
        auto* ch = dynamic_cast<LogosObjectErrorChannel*>(named);
        for (int i = 0; i < kPings; ++i) {
            logos::CallError err;
            ch->callMethodWithError(QStringLiteral("tok"), QStringLiteral("ping"),
                                    QVariantList{}, 2000, &err);
            sent.fetch_add(1);
        }
    });

    QElapsedTimer pump; pump.start();
    while ((sent.load() < kPings || nn->load() < kPings || nw->load() < kPings)
           && pump.elapsed() < 8000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
    driver.join();
    // Give a duplicate copy time to show up before asserting there is none.
    pump.restart();
    while (pump.elapsed() < 300)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);

    std::cout << "  " << kPings << " emissions -> named=" << nn->load()
              << " wildcard=" << nw->load() << std::endl;

    EXPECT_EQ(nn->load(), kPings)
        << "the host sent this connection more than one copy per emission";
    EXPECT_EQ(nw->load(), kPings);

    named->release();
    wildcard->release();
    host.reset();
}

// ── churn: one handle leaving must not take another's subscription with it ──
TEST(PlainEventSubSharingTest, ReleasingOneHandleLeavesTheOtherSubscribed)
{
    constexpr int kEmissions = 5;

    MirrorProvider provider;
    WirePair wire(&provider);

    auto* probe = new PlainLogosObject("evmod", wire.client);
    auto* a = new PlainLogosObject("evmod", wire.client);
    auto* b = new PlainLogosObject("evmod", wire.client);

    Counter na = makeCounter(), nb = makeCounter();
    a->onEvent(QStringLiteral("tick"), [na](const QString&, const QVariantList&) { na->fetch_add(1); });
    b->onEvent(QStringLiteral("tick"), [nb](const QString&, const QVariantList&) { nb->fetch_add(1); });

    wireBarrier(probe);
    ASSERT_EQ(provider.subscribeFrames(), 2);

    // A goes away. Its Unsubscribe must not evict the sink B is still using, and
    // must not take B's callback out of the connection's table either.
    a->release();
    wireBarrier(probe);
    ASSERT_EQ(provider.sinkCount(), 1)
        << "the host-side sink was removed while a handle was still subscribed";

    for (int i = 0; i < kEmissions; ++i)
        provider.emitEvent("evmod", "tick", QVariantList{ QVariant(i) });
    wireBarrier(probe);

    std::cout << "  after A released: A=" << na->load() << " B=" << nb->load()
              << " host sinks=" << provider.sinkCount() << std::endl;

    EXPECT_EQ(nb->load(), kEmissions)
        << "releasing one handle unsubscribed a DIFFERENT handle's event";
    EXPECT_EQ(na->load(), 0) << "a released handle must not keep receiving events";

    b->release();
    probe->release();
}

// ── and teardown still removes everything ───────────────────────────────────
//
// A PIN, NOT A DETECTOR: this passes on the pre-fix tree too. It is here because
// the fix withholds the Unsubscribe frame until the LAST local subscriber goes,
// which is precisely the sort of change that leaks a host-side sink forever.
TEST(PlainEventSubSharingTest, TeardownRemovesEverySubscription)
{
    MirrorProvider provider;
    WirePair wire(&provider);

    auto* probe = new PlainLogosObject("evmod", wire.client);
    auto* a = new PlainLogosObject("evmod", wire.client);
    auto* b = new PlainLogosObject("evmod", wire.client);

    Counter na = makeCounter(), nb = makeCounter();
    a->onEvent(QStringLiteral("tick"), [na](const QString&, const QVariantList&) { na->fetch_add(1); });
    b->onEvent(QStringLiteral("tick"), [nb](const QString&, const QVariantList&) { nb->fetch_add(1); });
    a->onEvent(QStringLiteral("tock"), [na](const QString&, const QVariantList&) { na->fetch_add(1); });

    wireBarrier(probe);
    ASSERT_EQ(provider.sinkCount(), 2);

    a->release();
    b->release();
    wireBarrier(probe);

    EXPECT_EQ(provider.sinkCount(), 0)
        << "the host still holds " << provider.sinkCount()
        << " sink(s) after every handle was released";

    // Push an event at the connection anyway — a peer that never saw the
    // Unsubscribe would — and prove the consumer side is empty too.
    EventMessage evt;
    evt.object    = "evmod";
    evt.eventName = "tick";
    evt.data      = qvariantListToRpcList(QVariantList{ QVariant(1) });
    wire.server->sendEvent(std::move(evt));
    wireBarrier(probe);

    std::cout << "  after teardown: sinks=" << provider.sinkCount()
              << " unsubscribe frames=" << provider.unsubscribeFrames()
              << " late deliveries A=" << na->load() << " B=" << nb->load()
              << std::endl;

    EXPECT_EQ(na->load(), 0);
    EXPECT_EQ(nb->load(), 0);
    probe->release();
}

// ── no subscription is lost under subscribe / unsubscribe / release churn ───
//
// Rounds of concurrent churn on a shared connection, each followed by a QUIESCED
// audit: one emission, and every handle that is subscribed at that moment must
// receive exactly one, while every handle that is not must receive none. The
// audit is exact rather than statistical, so a single lost subscription in any
// interleaving fails the test.
TEST(PlainEventSubSharingTest, SubscriptionsSurviveChurn)
{
    constexpr int kThreads = 4;
    constexpr int kPerThread = 3;
    constexpr int kRounds  = 40;

    MirrorProvider provider;
    WirePair wire(&provider);
    auto* probe = new PlainLogosObject("churn", wire.client);

    struct Handle {
        PlainLogosObject* obj = nullptr;
        Counter           hits;
        bool              subscribed = false;
        int               expected = 0;
    };
    std::vector<Handle> handles(kThreads * kPerThread);
    for (auto& s : handles) s.hits = makeCounter();

    std::atomic<int> arrived{0};
    std::atomic<int> generation{0};
    std::atomic<bool> stop{false};

    auto worker = [&](int tid) {
        std::mt19937 rng(1234 + tid);
        int seen = 0;
        while (true) {
            // Wait for the auditor to open the next churn window.
            while (generation.load(std::memory_order_acquire) == seen && !stop.load())
                std::this_thread::yield();
            if (stop.load()) return;
            seen = generation.load(std::memory_order_acquire);

            for (int i = 0; i < kPerThread; ++i) {
                Handle& s = handles[tid * kPerThread + i];
                const int roll = static_cast<int>(rng() % 3);
                if (roll == 0) continue;                 // leave it alone
                if (s.subscribed) {                      // drop it
                    s.obj->release();
                    s.obj = nullptr;
                    s.subscribed = false;
                } else {                                 // take it
                    s.obj = new PlainLogosObject("churn", wire.client);
                    Counter c = s.hits;
                    s.obj->onEvent(QStringLiteral("beat"),
                                   [c](const QString&, const QVariantList&) { c->fetch_add(1); });
                    s.subscribed = true;
                }
            }
            arrived.fetch_add(1, std::memory_order_release);
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker, t);

    int lost = 0, spurious = 0, audited = 0;
    for (int r = 0; r < kRounds; ++r) {
        arrived.store(0, std::memory_order_release);
        generation.fetch_add(1, std::memory_order_release);
        while (arrived.load(std::memory_order_acquire) < kThreads)
            std::this_thread::yield();

        // Quiesced: nothing is churning now, and the barrier proves every frame
        // the churn produced has reached the provider. The audit that follows is
        // therefore an EXACT count, not a poll with a timeout.
        wireBarrier(probe);
        int live = 0;
        for (auto& s : handles) if (s.subscribed) ++live;
        for (auto& s : handles) if (s.subscribed) ++s.expected;

        provider.emitEvent("churn", "beat", QVariantList{ QVariant(r) });
        wireBarrier(probe);

        for (auto& s : handles) {
            ++audited;
            const int got = s.hits->load();
            if (got < s.expected) { ++lost; s.expected = got; }
            else if (got > s.expected) { ++spurious; s.expected = got; }
        }
        (void)live;
    }

    stop.store(true);
    for (auto& t : threads) t.join();

    std::cout << "  " << kRounds << " audited rounds over " << handles.size()
              << " handles -> lost=" << lost << " spurious=" << spurious
              << " (audited slots: " << audited << ")" << std::endl;

    EXPECT_EQ(lost, 0) << lost << " subscriptions missed an emission they were "
                                  "subscribed for";
    EXPECT_EQ(spurious, 0) << spurious << " handles received an emission they were "
                                          "not subscribed for";

    for (auto& s : handles) if (s.obj) s.obj->release();
    probe->release();
}

// ── a completion that overtakes its own Result is still delivered ────────────
//
// The reason the staging area in CallState exists, and the reason it cannot
// simply drop everything it fails to attribute. A "multi" module whose worker
// finishes inside callMethod pushes the completion event before the host has
// written the sentinel, so the completion is decoded FIRST — at which point the
// callId names a call whose Result the consumer has not seen and cannot match.
// It has to be parked; the gate that keeps parking bounded must not lose it.
TEST(PlainEventSubSharingTest, ACompletionThatOvertakesItsOwnResultIsStillDelivered)
{
    ensureApp();
    constexpr int kTimeoutMs = 1500;

    MirrorProvider provider;
    provider.setCompletionBeforeResult(true);
    WirePair wire(&provider);

    auto* a = new PlainLogosObject("multimod", wire.client);
    auto* b = new PlainLogosObject("multimod", wire.client);

    // Synchronous, on both handles, alternating — the path that has to park.
    int answered = 0;
    for (int i = 0; i < 6; ++i) {
        PlainLogosObject* obj = (i % 2) ? b : a;
        logos::CallError err;
        const QVariant v = obj->callMethodWithError(
            QStringLiteral("tok"), QStringLiteral("work"), QVariantList{},
            kTimeoutMs, &err);
        if (v.toInt() == MirrorProvider::kDeferredAnswer) ++answered;
    }

    // And asynchronously, which parks nothing (the reply handler files the callId
    // under `deferred` on the strand) but must survive the same reordering.
    std::atomic<int> asyncAnswered{0};
    for (int i = 0; i < 4; ++i) {
        PlainLogosObject* obj = (i % 2) ? b : a;
        obj->callMethodAsyncWithError(
            QStringLiteral("tok"), QStringLiteral("work"), QVariantList{}, kTimeoutMs,
            [&asyncAnswered](QVariant v, const logos::CallError&) {
                if (v.toInt() == MirrorProvider::kDeferredAnswer)
                    asyncAnswered.fetch_add(1);
            });
    }
    QElapsedTimer pump; pump.start();
    while (asyncAnswered.load() < 4 && pump.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);

    std::cout << "  completion-before-result: sync " << answered << "/6, async "
              << asyncAnswered.load() << "/4" << std::endl;

    EXPECT_EQ(answered, 6);
    EXPECT_EQ(asyncAnswered.load(), 4);

    a->release();
    b->release();
}

// ── mixed versions, direction 1: a NEW consumer against an OLD host ─────────
//
// The wire does not move, so this is exercisable in one process: the provider
// below is the host's PRE-FIX bookkeeping — assign the sink on Subscribe, erase
// it by (event, connection) on Unsubscribe, and fan out every matching sink with
// no per-connection de-duplication. Everything the consumer half of the fix does
// has to work against it, because that is what an unpatched daemon looks like.
TEST(PlainEventSubSharingTest, ANewConsumerAgainstAnOldHost)
{
    constexpr int kEmissions = 4;

    MirrorProvider oldHost;
    oldHost.setLegacyFanOut(true);
    WirePair wire(&oldHost);

    auto* probe = new PlainLogosObject("evmod", wire.client);
    auto* a = new PlainLogosObject("evmod", wire.client);
    auto* b = new PlainLogosObject("evmod", wire.client);

    Counter na = makeCounter(), nb = makeCounter();
    a->onEvent(QStringLiteral("tick"), [na](const QString&, const QVariantList&) { na->fetch_add(1); });
    b->onEvent(QStringLiteral("tick"), [nb](const QString&, const QVariantList&) { nb->fetch_add(1); });

    wireBarrier(probe);
    ASSERT_EQ(oldHost.subscribeFrames(), 2);
    // An old host holds ONE sink for the pair however many Subscribes arrive —
    // which is exactly why the consumer, not the host, has to demultiplex.
    EXPECT_EQ(oldHost.sinkCount(), 1);

    for (int i = 0; i < kEmissions; ++i)
        oldHost.emitEvent("evmod", "tick", QVariantList{ QVariant(i) });
    wireBarrier(probe);
    EXPECT_EQ(na->load(), kEmissions);
    EXPECT_EQ(nb->load(), kEmissions);

    // A leaves; the old host is told nothing, so B keeps its sink.
    a->release();
    wireBarrier(probe);
    for (int i = 0; i < kEmissions; ++i)
        oldHost.emitEvent("evmod", "tick", QVariantList{ QVariant(i) });
    wireBarrier(probe);

    std::cout << "  new consumer / old host: A=" << na->load() << " B=" << nb->load()
              << " sinks=" << oldHost.sinkCount()
              << " unsubscribe frames=" << oldHost.unsubscribeFrames() << std::endl;

    EXPECT_EQ(nb->load(), 2 * kEmissions);
    EXPECT_EQ(na->load(), kEmissions);
    EXPECT_EQ(oldHost.unsubscribeFrames(), 0);

    // B leaves: now the old host hears about it, and its table is empty.
    b->release();
    wireBarrier(probe);
    EXPECT_EQ(oldHost.sinkCount(), 0);
    EXPECT_EQ(oldHost.unsubscribeFrames(), 1);
    probe->release();
}

// ── mixed versions, direction 2: an OLD consumer against the NEW host ───────
//
// An old consumer cannot be linked into this binary alongside the new one, so
// this speaks its wire directly: a raw TCP socket to a real PlainTransportHost,
// hand-built Subscribe / Unsubscribe frames in the sequence a pre-fix consumer
// emits (one of each per handle, no subscription id — there never was one), and
// a count of the Event frames that come back.
TEST(PlainEventSubSharingTest, AnOldConsumersFrameSequenceAgainstTheNewHost)
{
    ensureApp();

    LogosTransportConfig cfg;
    cfg.protocol = LogosProtocol::Tcp;
    cfg.host = "127.0.0.1";
    cfg.port = 0;

    auto host = std::make_unique<PlainTransportHost>(cfg);
    ASSERT_TRUE(host->start());

    EmittingModule mod;
    ModuleProxy proxy(&mod);
    proxy.saveToken(QStringLiteral("core"), QStringLiteral("tok"));
    ASSERT_TRUE(host->publishObject("emitmod", &proxy));

    const QString endpoint = host->endpoint();
    const unsigned short port = endpoint.mid(endpoint.lastIndexOf(':') + 1).toUShort();

    boost::asio::io_context ioc;
    boost::asio::ip::tcp::socket sock(ioc);
    boost::asio::ip::tcp::resolver resolver(ioc);
    boost::system::error_code ec;
    boost::asio::connect(sock, resolver.resolve("127.0.0.1", std::to_string(port)), ec);
    ASSERT_FALSE(ec) << ec.message();
    sock.non_blocking(true);

    auto codec = std::make_shared<JsonCodec>();
    FrameReader reader;
    std::vector<uint8_t> buf(4096);

    const auto writeMsg = [&](AnyMessage m) {
        const auto frame = encodeFrame(*codec, std::move(m));
        std::size_t off = 0;
        while (off < frame.size()) {
            boost::system::error_code wec;
            const auto n = sock.write_some(
                boost::asio::buffer(frame.data() + off, frame.size() - off), wec);
            if (wec == boost::asio::error::would_block) continue;
            ASSERT_FALSE(wec) << wec.message();
            off += n;
        }
    };
    std::vector<EventMessage> events;
    std::atomic<uint64_t> lastMethodsResult{0};

    // Pump both sides for a while, collecting Event frames and noting the id of
    // any MethodsResult that comes back (the barrier below reads that).
    const auto pumpFor = [&](int ms) {
        QElapsedTimer t; t.start();
        while (t.elapsed() < ms) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
            boost::system::error_code rec;
            const auto n = sock.read_some(boost::asio::buffer(buf), rec);
            if (rec == boost::asio::error::would_block || n == 0) continue;
            reader.append(buf.data(), n);
            MessageType tag;
            std::vector<uint8_t> payload;
            while (reader.next(tag, payload)) {
                auto any = codec->decode(tag, payload.data(), payload.size());
                if (tag == MessageType::Event)
                    events.push_back(std::get<EventMessage>(any));
                else if (tag == MessageType::MethodsResult)
                    lastMethodsResult.store(std::get<MethodsResultMessage>(any).id);
            }
        }
    };

    // AN IN-BAND BARRIER instead of a sleep, because a sleep here is a coin flip
    // on a loaded runner and a wrong one fails the test rather than skipping it.
    // A Methods request travels the same socket and is dispatched on the same
    // strand as everything written before it, and its reply comes back only after
    // the host's Qt thread has answered — so a MethodsResult in hand proves every
    // earlier Subscribe/Unsubscribe frame has been applied.
    uint64_t barrierId = 1000;
    const auto barrier = [&]() {
        const uint64_t id = ++barrierId;
        writeMsg(MethodsMessage{id, "tok", "emitmod"});
        QElapsedTimer t; t.start();
        while (lastMethodsResult.load() != id && t.elapsed() < 10000) pumpFor(10);
        return lastMethodsResult.load() == id;
    };

    // Emitting straight through the module, so no Call frame (and no token
    // handshake) is needed to make the host fan an event out.
    const auto emitAndCount = [&]() {
        events.clear();
        mod.callMethod(QStringLiteral("ping"), QVariantList{});
        // One more barrier: the Event frame is written on the same strand and
        // therefore lands ahead of the MethodsResult that follows it.
        barrier();
        pumpFor(50);
        return events;
    };

    // Two "handles", each sending its own Subscribe frame for the same pair.
    writeMsg(SubscribeMessage{"emitmod", "tick"});
    writeMsg(SubscribeMessage{"emitmod", "tick"});
    ASSERT_TRUE(barrier()) << "the host never answered — nothing below is meaningful";
    auto got = emitAndCount();
    ASSERT_EQ(got.size(), 1u)
        << "an old peer must still get exactly one copy per emission";
    // The frame itself is byte-for-byte what it always was.
    EXPECT_EQ(got[0].object, "emitmod");
    EXPECT_EQ(got[0].eventName, "tick");
    ASSERT_EQ(got[0].data.size(), 1u);
    EXPECT_TRUE(got[0].data[0].isIntegral());

    // The one place an old peer sees a DIFFERENT number of frames: subscribed
    // both by name and by wildcard, it used to be sent two copies of the same
    // event and delivered each to both of its callbacks. Now it is sent one.
    writeMsg(SubscribeMessage{"emitmod", ""});
    ASSERT_TRUE(barrier());
    got = emitAndCount();
    EXPECT_EQ(got.size(), 1u)
        << "a connection matching twice must still be sent one copy";

    // And its per-handle Unsubscribe frames still mean what they always meant.
    writeMsg(UnsubscribeMessage{"emitmod", ""});
    writeMsg(UnsubscribeMessage{"emitmod", "tick"});
    ASSERT_TRUE(barrier());
    got = emitAndCount();
    EXPECT_EQ(got.size(), 0u) << "the host kept sending after Unsubscribe";
    // A second, redundant Unsubscribe (an old consumer sends one per handle)
    // must be harmless.
    writeMsg(UnsubscribeMessage{"emitmod", "tick"});
    ASSERT_TRUE(barrier());
    got = emitAndCount();
    EXPECT_EQ(got.size(), 0u);

    sock.close(ec);
    host.reset();
}

// ── the staging area's gate, directly ───────────────────────────────────────
//
// CallState::parkCompletion is the one piece of this change that DROPS data on
// purpose, so it is pinned on its own rather than only through a transport.
TEST(PlainParkedCompletionGateTest, ParkingIsGatedOnHavingSomethingOutstanding)
{
    PlainLogosObject::CallState st;
    QVariant out;

    // Idle: a completion arriving now belongs to another handle on the shared
    // connection and is dropped rather than kept for the life of the connection.
    st.parkCompletion(QStringLiteral("foreign"), QVariant(1));
    EXPECT_FALSE(st.takeCompletion(QStringLiteral("foreign"), &out));

    // A synchronous call outstanding: park it, because the sentinel that names it
    // may be the very next frame.
    ++st.syncOutstanding;
    st.parkCompletion(QStringLiteral("mine"), QVariant(42));
    EXPECT_TRUE(st.takeCompletion(QStringLiteral("mine"), &out));
    EXPECT_EQ(out.toInt(), 42);

    // Anything left over when the last call finishes is provably unclaimable.
    st.parkCompletion(QStringLiteral("leftover"), QVariant(7));
    --st.syncOutstanding;
    st.dropUnclaimedIfIdle();
    EXPECT_FALSE(st.takeCompletion(QStringLiteral("leftover"), &out));
    EXPECT_TRUE(st.completions.empty());
    EXPECT_TRUE(st.completionOrder.empty());
}

TEST(PlainParkedCompletionGateTest, TheStagingAreaIsCappedAndEvictsOldestFirst)
{
    PlainLogosObject::CallState st;
    ++st.syncOutstanding;                 // busy for the whole test

    constexpr int kFlood = 4000;
    for (int i = 0; i < kFlood; ++i)
        st.parkCompletion(QStringLiteral("id-%1").arg(i), QVariant(i));

    std::cout << "  " << kFlood << " unclaimed completions parked -> "
              << st.completions.size() << " retained" << std::endl;

    EXPECT_LE(st.completions.size(), 512u)
        << "the staging area grew without bound";
    EXPECT_EQ(st.completions.size(), st.completionOrder.size())
        << "the map and the eviction order drifted apart";

    // Newest survives, oldest is gone.
    QVariant out;
    EXPECT_TRUE(st.takeCompletion(QStringLiteral("id-%1").arg(kFlood - 1), &out));
    EXPECT_EQ(out.toInt(), kFlood - 1);
    EXPECT_FALSE(st.takeCompletion(QStringLiteral("id-0"), &out));
    // And the pair stayed in step across a claim.
    EXPECT_EQ(st.completions.size(), st.completionOrder.size());
}
