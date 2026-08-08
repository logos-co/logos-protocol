// The io_context fold: an async call is state plus a deadline, not a thread.
// This file is the evidence for that, and the replacement for
// test_plain_waiter_publish_is_last.cpp, whose subject (a waiter thread's
// publish ordering) no longer exists.
//
// WHAT IS PINNED HERE, and why each one needs its own test:
//
//   1. NO THREAD PER PENDING RPC — the point of the exercise. Measured with the
//      OS thread count of the PROCESS, against calls genuinely parked in a
//      provider that will not answer. Pre-fold this is +1 per in-flight call.
//
//   2. EXACTLY ONCE, on every path, counted PER CALL. This is the guarantee most
//      at risk from the rework, because its mechanism changed: it used to be
//      structural (one thread, one function body, and a join proving it had
//      finished) and is now a single CAS that three independent callers race for
//      — the reply handler, the deadline, and teardown. The plain outcomes
//      (normal / deferred / timeout / cancelled) are WEAK detectors of that: a
//      call resolved once calls deliver() once, and stays green with the CAS
//      removed. Measured, not assumed — so the strong one is a separate test
//      that puts teardown and a stream of arriving replies on the same calls.
//
//   3. THE DEADLINE IS NOT HOSTAGE TO THE IO THREAD. The version of this work
//      that put the per-call timer on the connection's strand had exactly one
//      regression, and this was it: IoContextPool runs ONE thread for the whole
//      process and this transport delivers user onEvent callbacks INLINE on it,
//      so an event handler that made a 2000ms call delayed a 200ms deadline on a
//      DIFFERENT connection to 2003ms. Both shapes are pinned — a busy io thread
//      and a permanently blocked one.
//
//   4. RETENTION IS BOUNDED BY WHAT IS IN FLIGHT, on both sides: the handle's
//      CallState registries AND the connection's pending-call map, which nothing
//      used to erase for a call resolved by its deadline.
//
//   5. NO USE-AFTER-FREE, which is what replaced the join. Nothing waits for the
//      io thread; instead no handler can reach the handle.
//
// HOW THE DETECTORS ARE VALIDATED. Configure the tests tree once with
//
//   cmake -S tests -B build-broken -DLOGOS_PROTOCOL_DETECTOR_INVERSIONS=ON
//
// which compiles the transport with (a) the exactly-once CAS removed and (b) the
// deadline back on the shared io_context. (It also removes a third mechanism,
// the completion-subscription serialization, which belongs to
// test_plain_completion_sub_order.cpp — see tests/protocol/CMakeLists.txt for
// the full list of what must go red.) In that build these must go RED, and
// these are the numbers they were seen to go red with:
//
//   ReleaseRacingRepliesInFlightDeliversEachCallOnce  (a)  6-16 double
//                                                          deliveries per
//                                                          10,000 calls, 4 runs
//   DeadlineFiresOnTimeWhileTheIoThreadIsBusy         (b)  200ms deadline fires
//                                                          at 2002ms
//   DeadlineFiresWhileTheIoThreadIsBlockedForever     (b)  never fires at all
//
// A green run of those in the inverted build means the test is not exercising
// what it claims to, and is a bug in the test. Two candidates were REJECTED as
// exactly-once detectors on exactly that ground — see the comment on
// ReleaseRacingRepliesInFlightDeliversEachCallOnce.
//
// Everything runs against a live in-process PlainTransportHost over real TCP.

#include <gtest/gtest.h>

#include "logos_async_dispatch.h"
#include "logos_call_error.h"
#include "logos_object.h"
#include "logos_provider_interface.h"
#include "logos_transport_config.h"
#include "module_proxy.h"

#include "plain_logos_object.h"
#include "plain_transport_connection.h"
#include "plain_transport_host.h"

#include <boost/asio/ip/tcp.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QString>
#include <QThread>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <mach/mach.h>
#else
#include <cstring>
#include <fstream>
#endif

using namespace logos::plain;

namespace {

// Live OS threads in this process. The measurement has to be of the PROCESS,
// not of anything the object reports about itself: the claim is that pending
// calls stopped costing threads, and an object-level counter would only be
// restating the implementation.
int liveThreads()
{
#ifdef __APPLE__
    thread_act_array_t list = nullptr;
    mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS) return -1;
    for (mach_msg_type_number_t i = 0; i < n; ++i)
        mach_port_deallocate(mach_task_self(), list[i]);
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(list),
                  n * sizeof(thread_act_t));
    return static_cast<int>(n);
#elif defined(__linux__)
    std::ifstream st("/proc/self/status");
    std::string line;
    while (std::getline(st, line)) {
        if (line.rfind("Threads:", 0) == 0)
            return std::atoi(line.c_str() + 8);
    }
    return -1;
#else
    return -1;   // the assertions below are skipped where this is unavailable
#endif
}

// One provider covering every outcome the fold has to preserve:
//   ping     — answers immediately (normal completion)
//   block    — parks until letGo() (in-flight; timeout; cancellation)
//   defer    — "multi": returns the pending sentinel and completes it later
//   sink     — "multi": returns the sentinel and NEVER completes it
//   slowsink — "multi": the sentinel itself arrives after the caller's deadline
//   fire     — emits a user event, which this transport delivers INLINE on the
//              consumer's io thread
class OmniProvider : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList& args) override
    {
        if (method == QLatin1String("ping")) return args.value(0, QVariant(1));
        if (method == QLatin1String("block")) {
            std::unique_lock<std::mutex> lk(m_mu);
            m_cv.wait(lk, [this] { return m_released; });
            return QVariant(42);
        }
        if (method == QLatin1String("fire")) {
            if (m_eventCb) m_eventCb(QStringLiteral("tick"), QVariantList{ QVariant(1) });
            return QVariant(true);
        }
        if (method == QLatin1String("slowsink")) {
            // The sentinel itself arrives LATE — after the caller's deadline
            // has already elapsed and the call has been resolved as a timeout.
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            QVariantMap pending;
            pending[logos::pendingCallKey()] = QStringLiteral("late-%1").arg(
                static_cast<qulonglong>(m_counter.fetch_add(1)));
            return pending;
        }
        if (method == QLatin1String("sink")) {
            QVariantMap pending;
            pending[logos::pendingCallKey()] = QStringLiteral("never-%1").arg(
                static_cast<qulonglong>(m_counter.fetch_add(1)));
            return pending;
        }
        if (method == QLatin1String("defer")) {
            const int delayUs = args.value(0).toInt();
            const QString callId = QStringLiteral("cid-%1").arg(
                static_cast<qulonglong>(m_counter.fetch_add(1)));
            auto cb = m_eventCb;
            // The completion is pushed from a worker, which is what a real
            // "multi" provider does and what makes the event arrive on the
            // consumer's io thread rather than inline in the reply.
            std::thread([cb, callId, delayUs]() {
                if (delayUs > 0)
                    std::this_thread::sleep_for(std::chrono::microseconds(delayUs));
                if (cb) cb(logos::callCompleteEvent(),
                           QVariantList{ callId, QVariant(7) });
            }).detach();
            QVariantMap pending;
            pending[logos::pendingCallKey()] = callId;
            return pending;
        }
        return QVariant();
    }

    void letGo()
    {
        {
            std::lock_guard<std::mutex> g(m_mu);
            m_released = true;
        }
        m_cv.notify_all();
    }

    QJsonArray getMethods() override { return QJsonArray{}; }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    void setEventListener(EventCallback cb) override { m_eventCb = std::move(cb); }
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("omni"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }

private:
    std::mutex                 m_mu;
    std::condition_variable    m_cv;
    bool                       m_released = false;
    EventCallback              m_eventCb;
    std::atomic<std::uint64_t> m_counter{0};
};

QCoreApplication* ensureApp()
{
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance())
        new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

class LiveHost {
public:
    LiveHost()
    {
        LogosTransportConfig cfg;
        cfg.protocol = LogosProtocol::Tcp;
        cfg.host = "127.0.0.1";
        cfg.port = 0;   // ephemeral
        m_host = std::make_unique<PlainTransportHost>(cfg);
        m_started = m_host->start();

        m_proxy = new ModuleProxy(&m_provider);
        m_proxy->saveToken(QStringLiteral("origin"), QStringLiteral("live-token"));
        m_thread = new QThread;
        m_proxy->moveToThread(m_thread);
        m_thread->start();

        m_published = m_host->publishObject("omni_module", m_proxy);
        const QString endpoint = m_host->endpoint();
        m_port = endpoint.mid(endpoint.lastIndexOf(':') + 1).toUShort();
    }

    ~LiveHost()
    {
        m_provider.letGo();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
        m_host.reset();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        m_thread->quit();
        m_thread->wait();
        delete m_proxy;
        delete m_thread;
    }

    bool ok() const { return m_started && m_published && m_port != 0; }
    uint16_t port() const { return m_port; }
    OmniProvider& provider() { return m_provider; }

private:
    OmniProvider m_provider;
    std::unique_ptr<PlainTransportHost> m_host;
    ModuleProxy* m_proxy = nullptr;
    QThread* m_thread = nullptr;
    bool m_started = false;
    bool m_published = false;
    uint16_t m_port = 0;
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

// Reads the registries through the explicit-instantiation access hole
// ([temp.spec] does not check access on the template arguments of an explicit
// instantiation), so the code under test is observed exactly as it ships — no
// friend, no test-only accessor, no `#define private public`.
template <typename Tag, typename Tag::type Member>
struct Rob {
    friend typename Tag::type get(Tag) { return Member; }
};

struct StateTag {
    using type = std::shared_ptr<PlainLogosObject::CallState> PlainLogosObject::*;
    friend type get(StateTag);
};
template struct Rob<StateTag, &PlainLogosObject::m_state>;

struct ConnTag {
    using type = std::shared_ptr<RpcConnectionBase> PlainLogosObject::*;
    friend type get(ConnTag);
};
template struct Rob<ConnTag, &PlainLogosObject::m_conn>;

using TcpConn = RpcConnection<boost::asio::ip::tcp::socket>;

struct PendingCallsTag {
    using type = std::map<std::uint64_t, RpcConnectionBase::ResultHandler> TcpConn::*;
    friend type get(PendingCallsTag);
};
template struct Rob<PendingCallsTag, &TcpConn::m_pendingCalls>;

struct ConnMuTag {
    using type = std::mutex TcpConn::*;
    friend type get(ConnMuTag);
};
template struct Rob<ConnMuTag, &TcpConn::m_mu>;

struct Registries {
    size_t inflight;
    size_t deferred;
    size_t completions;
    size_t pendingOnConnection;
};

Registries registries(PlainLogosObject* obj)
{
    Registries r{0, 0, 0, 0};
    {
        auto& st = obj->*get(StateTag{});
        std::lock_guard<std::mutex> g(st->mu);
        r.inflight    = st->inflight.size();
        r.deferred    = st->deferred.size();
        r.completions = st->completions.size();
    }
    auto& base = obj->*get(ConnTag{});
    if (auto* c = dynamic_cast<TcpConn*>(base.get())) {
        std::lock_guard<std::mutex> g(c->*get(ConnMuTag{}));
        r.pendingOnConnection = (c->*get(PendingCallsTag{})).size();
    }
    return r;
}

// Per-call delivery counts. Both 0 and 2 are failures, and they are counted per
// call rather than in aggregate — a double delivery on one call plus a dropped
// one on another balances out in a total.
struct Deliveries {
    explicit Deliveries(int n) : counts(n) {}
    std::vector<std::atomic<int>> counts;
    std::atomic<int> total{0};

    std::mutex  mu;
    std::string lastCode;
    QVariant    lastValue;

    void record(int i, QVariant v, const logos::CallError& e)
    {
        {
            std::lock_guard<std::mutex> g(mu);
            lastCode  = e.code;
            lastValue = std::move(v);
        }
        counts[i].fetch_add(1);
        total.fetch_add(1);
    }
    std::string code() { std::lock_guard<std::mutex> g(mu); return lastCode; }
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

void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

void pumpUntilTotal(Deliveries& d, int target, int budgetMs)
{
    QElapsedTimer t;
    t.start();
    while (d.total.load() < target && t.elapsed() < budgetMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

const char* kToken = "live-token";

} // namespace

class IoFoldTest : public ::testing::Test {
protected:
    void SetUp() override { ensureApp(); }
};

// ── 1. no thread per pending RPC ────────────────────────────────────────────
//
// 32 calls parked in a provider that will not answer. Pre-fold each of those is
// an OS thread sitting in a sliced future wait — measured at exactly +1 per
// in-flight call. The claim here is that the same 32 calls cost none.
//
// The bound is 2 rather than 0 on purpose: Qt is free to service the connection
// from a pool thread of its own, and the two singleton threads this transport
// owns (the io worker and the deadline clock) are lazily created, which is what
// the warm-up call below is for. What must not happen is growth WITH the call
// count — with 32 in flight, anything at or near 32 is the old design.
TEST_F(IoFoldTest, PendingCallsDoNotCostThreads)
{
    if (liveThreads() < 0) GTEST_SKIP() << "no thread counter on this platform";

    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* obj = conn->requestObject(QStringLiteral("omni_module"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);

    // One warm-up call, completed, so the io worker, the deadline thread and any
    // Qt pool threads exist before the baseline is taken. Otherwise the fold gets
    // blamed for threads that lazy initialisation created.
    {
        Deliveries warm(1);
        ch->callMethodAsyncWithError(kToken, QStringLiteral("ping"),
                                     QVariantList{ QVariant(1) }, 5000,
                                     [&warm](QVariant v, const logos::CallError& e) {
                                         warm.record(0, std::move(v), e);
                                     });
        pumpUntilTotal(warm, 1, 10000);
        ASSERT_EQ(warm.total.load(), 1);
    }
    pump(150);

    const int base = liveThreads();

    constexpr int kInFlight = 32;
    Deliveries d(kInFlight);
    for (int i = 0; i < kInFlight; ++i) {
        ch->callMethodAsyncWithError(kToken, QStringLiteral("block"), {}, 20000,
                                     [&d, i](QVariant v, const logos::CallError& e) {
                                         d.record(i, std::move(v), e);
                                     });
    }
    // Long enough for every call to be registered and genuinely outstanding.
    pump(400);
    const int parked = liveThreads();
    ASSERT_EQ(d.total.load(), 0) << "the provider answered; nothing was in flight";

    std::cout << "  " << kInFlight << " calls PARKED in the provider -> threads "
              << base << " -> " << parked << " (delta " << (parked - base) << ")"
              << std::endl;

    EXPECT_LE(parked - base, 2)
        << "in-flight calls are still costing threads: +" << (parked - base)
        << " for " << kInFlight << " pending calls";

    QElapsedTimer timer;
    timer.start();
    obj->release();
    const qint64 releaseMs = timer.elapsed();

    pumpUntilTotal(d, kInFlight, 5000);
    pump(200);
    const int after = liveThreads();

    std::cout << "  release() with " << kInFlight << " in flight took "
              << releaseMs << "ms -> threads " << after
              << ", callbacks " << d.total.load() << "/" << kInFlight
              << " worst=" << d.worst() << std::endl;

    // Fast teardown in its harshest form: 32 calls outstanding, each with 20s
    // left on its clock. The old design's floor was one 25ms wait slice; there
    // is no slice any more, because there is no future being polled.
    EXPECT_LT(releaseMs, 100)
        << "teardown is waiting for something again";
    EXPECT_EQ(d.total.load(), kInFlight);
    EXPECT_EQ(d.worst(), 1);
    EXPECT_EQ(d.missing(), 0);

    host.provider().letGo();
    pump(200);
}

// ── 2. exactly once, per call, on the four ordinary outcomes ────────────────
//
// NORMAL and DEFERRED-THEN-COMPLETED are run at volume because they are the two
// that go through the reply handler and the completion-event handler
// respectively — the two sites where the deadline is still armed and racing.
//
// These are WEAK detectors of the exactly-once gate on their own (one resolver,
// one deliver()); the two race tests further down are the strong ones.
TEST_F(IoFoldTest, NormalAndDeferredCompletionsDeliverExactlyOnceAtVolume)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* obj = conn->requestObject(QStringLiteral("omni_module"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);
    auto* plain = dynamic_cast<PlainLogosObject*>(obj);
    ASSERT_NE(plain, nullptr);

    constexpr int kNormal   = 400;
    constexpr int kDeferred = 400;
    Deliveries normal(kNormal);
    Deliveries deferred(kDeferred);

    for (int i = 0; i < kNormal; ++i) {
        ch->callMethodAsyncWithError(kToken, QStringLiteral("ping"),
                                     QVariantList{ QVariant(i) }, 5000,
                                     [&normal, i](QVariant v, const logos::CallError& e) {
                                         normal.record(i, std::move(v), e);
                                     });
    }
    // Completion delays swept across the sub-millisecond band, so the event
    // sometimes beats the sentinel's own reply out of the provider and
    // sometimes trails it — both orders exercised rather than assumed.
    for (int i = 0; i < kDeferred; ++i) {
        ch->callMethodAsyncWithError(kToken, QStringLiteral("defer"),
                                     QVariantList{ QVariant((i % 12) * 40) }, 8000,
                                     [&deferred, i](QVariant v, const logos::CallError& e) {
                                         deferred.record(i, std::move(v), e);
                                     });
    }

    pumpUntilTotal(normal, kNormal, 60000);
    pumpUntilTotal(deferred, kDeferred, 60000);
    pump(500);   // a duplicate delivery would land here

    const Registries r = registries(plain);
    std::cout << "  normal   " << normal.total.load() << "/" << kNormal
              << " worst=" << normal.worst() << " missing=" << normal.missing()
              << " code='" << normal.code() << "'" << std::endl;
    std::cout << "  deferred " << deferred.total.load() << "/" << kDeferred
              << " worst=" << deferred.worst() << " missing=" << deferred.missing()
              << " code='" << deferred.code() << "'" << std::endl;
    std::cout << "  after " << (kNormal + kDeferred) << " completed calls: inflight="
              << r.inflight << " deferred=" << r.deferred << " completions="
              << r.completions << " connection-pending=" << r.pendingOnConnection
              << std::endl;

    EXPECT_EQ(normal.worst(), 1);
    EXPECT_EQ(normal.missing(), 0);
    EXPECT_TRUE(normal.code().empty());
    EXPECT_EQ(deferred.worst(), 1);
    EXPECT_EQ(deferred.missing(), 0);
    EXPECT_TRUE(deferred.code().empty()) << "a completed deferred call reported an error";
    {
        std::lock_guard<std::mutex> g(deferred.mu);
        EXPECT_EQ(deferred.lastValue.toInt(), 7)
            << "the deferred call delivered the sentinel instead of the completion";
    }

    // Retention, on both sides, after 800 completed calls on one handle.
    EXPECT_EQ(r.inflight, 0u);
    EXPECT_EQ(r.deferred, 0u);
    EXPECT_EQ(r.completions, 0u);
    EXPECT_EQ(r.pendingOnConnection, 0u);

    obj->release();
    pump(100);
}

// TIMEOUT and CANCELLATION, the two outcomes the deadline and teardown own. Run
// in alternation so a cancellation lands while other calls are mid-timeout.
TEST_F(IoFoldTest, TimeoutAndCancellationDeliverExactlyOnce)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    constexpr int kRounds = 40;
    Deliveries timedOut(kRounds);
    Deliveries cancelled(kRounds);

    for (int r = 0; r < kRounds; ++r) {
        LogosObject* obj = conn->requestObject(QStringLiteral("omni_module"), 5000);
        ASSERT_NE(obj, nullptr);
        auto* ch = channelFor(obj);
        ASSERT_NE(ch, nullptr);

        // One that will hit its deadline, and one the release will cancel.
        ch->callMethodAsyncWithError(kToken, QStringLiteral("block"), {}, 250,
                                     [&timedOut, r](QVariant v, const logos::CallError& e) {
                                         timedOut.record(r, std::move(v), e);
                                     });
        ch->callMethodAsyncWithError(kToken, QStringLiteral("sink"), {}, 9000,
                                     [&cancelled, r](QVariant v, const logos::CallError& e) {
                                         cancelled.record(r, std::move(v), e);
                                     });
        // Let the deadline pass and the sentinel come back, so the release
        // lands on a call that is genuinely parked in the deferred half.
        pumpUntilTotal(timedOut, r + 1, 5000);
        obj->release();
        pumpUntilTotal(cancelled, r + 1, 5000);
    }
    pump(500);

    std::cout << "  timeout   " << timedOut.total.load() << "/" << kRounds
              << " worst=" << timedOut.worst() << " code='" << timedOut.code()
              << "'" << std::endl;
    std::cout << "  cancelled " << cancelled.total.load() << "/" << kRounds
              << " worst=" << cancelled.worst() << " code='" << cancelled.code()
              << "'" << std::endl;

    EXPECT_EQ(timedOut.worst(), 1);
    EXPECT_EQ(timedOut.missing(), 0);
    EXPECT_EQ(timedOut.code(), "timeout")
        << "moving the deadline onto a steady_timer must not change what it reports";
    EXPECT_EQ(cancelled.worst(), 1);
    EXPECT_EQ(cancelled.missing(), 0);
    EXPECT_EQ(cancelled.code(), "transport_error")
        << "a call abandoned by release() is a torn-down transport, not a timeout";

    host.provider().letGo();
    pump(200);
}

// ── 2b. a call that hits its deadline and is THEN answered ──────────────────
//
// The shape that used to be the second resolver: the timer resolves the call,
// the reply turns up afterwards, and something has to make sure the caller is
// not told twice. TWO independent mechanisms now cover it, and this pins the
// pair:
//
//   * the delivery WITHDRAWS the registration from the connection
//     (cancelPending), so a reply arriving later finds no handler and is dropped
//     at the transport — which is also the retention fix, and is why the count
//     of pending registrations below must be zero;
//   * if the reply beats the withdrawal, the exactly-once CAS in deliver() takes
//     it.
//
// Be precise about what that makes this test: with an 800ms gap the withdrawal
// always wins, so it is a strong detector of the WITHDRAWAL and a weak one of
// the CAS. The strong CAS detector is ReleaseRacingAnInFlightCompletionIsSafe,
// where teardown and the completion handler genuinely arrive together.
TEST_F(IoFoldTest, ATimedOutCallThatIsLaterAnsweredStillDeliversOnce)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* obj = conn->requestObject(QStringLiteral("omni_module"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);
    auto* plain = dynamic_cast<PlainLogosObject*>(obj);
    ASSERT_NE(plain, nullptr);

    constexpr int kCalls = 24;
    Deliveries d(kCalls);
    for (int i = 0; i < kCalls; ++i) {
        // 200ms deadline against a provider that is parked: the timer fires
        // first, then letGo() releases the real reply into the same call.
        ch->callMethodAsyncWithError(kToken, QStringLiteral("block"), {}, 200,
                                     [&d, i](QVariant v, const logos::CallError& e) {
                                         d.record(i, std::move(v), e);
                                     });
    }
    pumpUntilTotal(d, kCalls, 10000);
    ASSERT_EQ(d.total.load(), kCalls);
    ASSERT_EQ(d.code(), "timeout");

    host.provider().letGo();
    pump(800);   // the reply arrives here, for calls already timed out

    const Registries r = registries(plain);
    std::cout << "  " << kCalls << " timed-out-then-answered calls -> deliveries="
              << d.total.load() << " worst=" << d.worst()
              << " inflight=" << r.inflight
              << " connection-pending=" << r.pendingOnConnection << std::endl;

    EXPECT_EQ(d.worst(), 1)
        << "a call was delivered twice: the exactly-once gate is not holding";
    EXPECT_EQ(d.missing(), 0);
    EXPECT_EQ(r.inflight, 0u);

    obj->release();
    pump(100);
}

// ── 2c. THE STRONG exactly-once detector: teardown against replies in flight ─
//
// Finding a race wide enough to be a reliable detector took some doing, and the
// two obvious candidates are both too narrow to trust — this comment is the map,
// because a detector that "usually" fires is not one.
//
//   * timeout-then-answer (2b above) is not a race at all any more: the delivery
//     withdraws the registration, so a reply arriving a comfortable interval
//     later never reaches the call.
//   * release-against-one-completion (further down) needs teardown to snapshot
//     the call in the few instructions between the completion handler taking it
//     out of `deferred` and deliver() taking it out of `inflight`. With the gate
//     removed it caught nothing in 6 solo runs of 300 rounds each, and caught
//     one double in a seventh run inside the full suite. It is a fine
//     use-after-free hammer and an unreliable exactly-once detector; a test that
//     fails one run in seven on broken code is not a gate.
//
// This one is wide by construction. Teardown snapshots the whole in-flight map
// under the lock and then delivers the calls ONE AT A TIME with the lock
// released, so with N calls outstanding the window in which the io thread can
// deliver a reply for a call teardown has already claimed is N deliveries long
// — not a handful of instructions. Both paths then arrive at deliver() for the
// same AsyncCall, and the CAS is the only thing deciding.
//
// With LOGOS_PROTOCOL_DETECTOR_INVERSIONS=ON this must report double
// deliveries. If it does not, the exactly-once assertions everywhere else in
// this file are decoration.
TEST_F(IoFoldTest, ReleaseRacingRepliesInFlightDeliversEachCallOnce)
{
    LiveHost host;
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
        LogosObject* obj = conn->requestObject(QStringLiteral("omni_module"), 5000);
        ASSERT_NE(obj, nullptr);
        auto* ch = channelFor(obj);
        ASSERT_NE(ch, nullptr);

        auto d = std::make_shared<Deliveries>(kCalls);
        auto codes = std::make_shared<std::vector<std::atomic<int>>>(2); // [reply, teardown]
        for (int i = 0; i < kCalls; ++i) {
            ch->callMethodAsyncWithError(kToken, QStringLiteral("ping"),
                                         QVariantList{ QVariant(i) }, 20000,
                                         [d, codes, i](QVariant v, const logos::CallError& e) {
                                             (*codes)[e.code.empty() ? 0 : 1].fetch_add(1);
                                             d->record(i, std::move(v), e);
                                         });
        }
        // No pump: release() lands while the provider is still answering, so the
        // io thread is delivering replies for exactly the calls teardown is
        // walking. A tiny jittered pause sweeps where in the burst it lands.
        QThread::usleep(static_cast<unsigned long>((r % 6) * 120));
        obj->release();

        pumpUntilTotal(*d, kCalls, 15000);
        pump(50);   // a duplicate would land here

        for (const auto& c : d->counts) {
            if (c.load() > 1) doubled += c.load() - 1;
            if (c.load() == 0) ++dropped;
        }
        byReply    += (*codes)[0].load();
        byTeardown += (*codes)[1].load();
    }

    std::cout << "  " << kRounds << " rounds x " << kCalls
              << " calls released mid-burst -> answered-by-reply=" << byReply
              << " cancelled-by-teardown=" << byTeardown
              << " DOUBLE deliveries=" << doubled << " dropped=" << dropped
              << std::endl;

    // Both resolvers have to have been live, or the race was not run.
    EXPECT_GT(byReply, 0)    << "no call was answered by its reply";
    EXPECT_GT(byTeardown, 0) << "no call was cancelled by teardown — release() "
                                "is landing after the whole burst completed and "
                                "this test is racing nothing";
    EXPECT_EQ(doubled, 0) << doubled << " calls were delivered more than once";
    EXPECT_EQ(dropped, 0) << dropped << " calls were never delivered at all";

    host.provider().letGo();
    pump(200);
}

// ── 3. the deadline is not hostage to the io thread ─────────────────────────
//
// THE ONE REGRESSION THE FIRST CUT OF THIS WORK HAD. Putting the per-call timer
// on the connection's strand looks obviously right — it serializes with the
// reply handler for free — and it makes every deadline in the process wait on a
// single thread that ordinary module code is allowed to occupy: this transport
// runs user onEvent callbacks INLINE on it (rpc_connection.h dispatchIncoming),
// and an event handler calling another module is not exotic.
//
// This is the exact shape that failed, and the numbers it produced: an onEvent
// handler holding the io thread for 2000ms, while a 200ms deadline is
// outstanding on a COMPLETELY DIFFERENT connection. On the pre-fold design that
// deadline fires at ~200ms because it has its own thread. With the timer on the
// shared strand it fired at 2003ms. It must be back to ~200ms.
TEST_F(IoFoldTest, DeadlineFiresOnTimeWhileTheIoThreadIsBusy)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());

    // TWO connections, to make the point that this is not about one call
    // queueing behind another on the same socket: they share nothing except the
    // process-wide io_context, which is the whole problem.
    auto connA = connectTo(host.port());
    ASSERT_NE(connA, nullptr);
    auto connB = connectTo(host.port());
    ASSERT_NE(connB, nullptr);

    LogosObject* a = connA->requestObject(QStringLiteral("omni_module"), 5000);
    ASSERT_NE(a, nullptr);
    auto* chA = channelFor(a);
    ASSERT_NE(chA, nullptr);
    LogosObject* b = connB->requestObject(QStringLiteral("omni_module"), 5000);
    ASSERT_NE(b, nullptr);
    auto* chB = channelFor(b);
    ASSERT_NE(chB, nullptr);

    std::atomic<bool> handlerRunning{false};
    std::atomic<bool> handlerDone{false};
    std::atomic<qint64> handlerHeldMs{0};
    const std::thread::id mainThread = std::this_thread::get_id();
    std::atomic<bool> onIoThread{false};

    a->onEvent(QStringLiteral("tick"), [&](const QString&, const QVariantList&) {
        onIoThread.store(std::this_thread::get_id() != mainThread);
        handlerRunning.store(true);
        QElapsedTimer held;
        held.start();
        // A SYNCHRONOUS call with a 2000ms budget, from inside an event handler.
        // Ordinary module code. It cannot be answered — the thread that would
        // decode the reply is this one — so it occupies the io thread for its
        // full 2000ms and then reports a timeout, which is precisely the
        // "handler that takes a while" case.
        logos::CallError err;
        chA->callMethodWithError(kToken, QStringLiteral("block"), {}, 2000, &err);
        handlerHeldMs.store(held.elapsed());
        handlerDone.store(true);
    });

    // Kick the event off. `fire` makes the provider emit "tick" back at us.
    Deliveries fired(1);
    chA->callMethodAsyncWithError(kToken, QStringLiteral("fire"), {}, 5000,
                                  [&fired](QVariant v, const logos::CallError& e) {
                                      fired.record(0, std::move(v), e);
                                  });

    // Wait for the handler to actually be on the io thread and blocking.
    {
        QElapsedTimer t;
        t.start();
        while (!handlerRunning.load() && t.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    ASSERT_TRUE(handlerRunning.load()) << "the event handler never ran";
    ASSERT_TRUE(onIoThread.load())
        << "the event did not arrive on the io thread — this test is not "
           "exercising the coupling it claims to";

    // Now, with the io thread held, put a 200ms deadline on the OTHER connection.
    Deliveries d(1);
    QElapsedTimer deadline;
    deadline.start();
    chB->callMethodAsyncWithError(kToken, QStringLiteral("block"), {}, 200,
                                  [&d](QVariant v, const logos::CallError& e) {
                                      d.record(0, std::move(v), e);
                                  });
    pumpUntilTotal(d, 1, 6000);
    const qint64 firedAt = deadline.elapsed();
    // Sampled BEFORE the unwind below, because it is half the claim: the io
    // thread has to still be held at the moment the deadline fires, or this
    // test is measuring an idle process.
    const bool stillHeld = !handlerDone.load();

    // Let everything unwind, so the held time can be reported rather than
    // guessed at.
    host.provider().letGo();
    {
        QElapsedTimer t;
        t.start();
        while (!handlerDone.load() && t.elapsed() < 8000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }

    std::cout << "  io thread held by an onEvent handler for "
              << handlerHeldMs.load() << "ms (still held when the deadline fired: "
              << stillHeld << "); a 200ms deadline on another connection fired at "
              << firedAt << "ms" << std::endl;

    EXPECT_EQ(d.total.load(), 1) << "the deadline never fired at all";
    EXPECT_EQ(d.code(), "timeout");
    EXPECT_LT(firedAt, 800)
        << "the deadline waited for the io thread: fired at " << firedAt
        << "ms instead of ~200ms. The per-call timer is coupled to the shared "
           "io_context again.";
    EXPECT_GE(firedAt, 150)
        << "the deadline fired early — this is measuring something else";
    EXPECT_TRUE(stillHeld)
        << "the handler let go before the deadline fired; the test proved nothing";

    pump(300);
    a->release();
    b->release();
    pump(100);
}

// The harsher half: a handler that never lets go at all. With the deadline on
// the shared io thread this call would hang for as long as the handler does,
// which is forever — the timeout stops existing exactly when it is most needed.
TEST_F(IoFoldTest, DeadlineFiresWhileTheIoThreadIsBlockedForever)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto connA = connectTo(host.port());
    ASSERT_NE(connA, nullptr);
    auto connB = connectTo(host.port());
    ASSERT_NE(connB, nullptr);

    LogosObject* a = connA->requestObject(QStringLiteral("omni_module"), 5000);
    ASSERT_NE(a, nullptr);
    auto* chA = channelFor(a);
    ASSERT_NE(chA, nullptr);
    LogosObject* b = connB->requestObject(QStringLiteral("omni_module"), 5000);
    ASSERT_NE(b, nullptr);
    auto* chB = channelFor(b);
    ASSERT_NE(chB, nullptr);

    std::mutex mu;
    std::condition_variable cv;
    bool letHandlerGo = false;
    std::atomic<bool> handlerRunning{false};
    std::atomic<bool> handlerDone{false};

    // The handler holds the process's only io thread, so EVERY exit from this
    // function — including a failed ASSERT — has to let it go. Without this a
    // regression here does not fail the suite, it hangs it, and every test that
    // runs afterwards hangs too.
    struct Unblock {
        std::mutex* mu; std::condition_variable* cv; bool* flag;
        ~Unblock()
        {
            { std::lock_guard<std::mutex> g(*mu); *flag = true; }
            cv->notify_all();
        }
    } unblock{&mu, &cv, &letHandlerGo};

    a->onEvent(QStringLiteral("tick"), [&](const QString&, const QVariantList&) {
        handlerRunning.store(true);
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return letHandlerGo; });
        handlerDone.store(true);
    });

    Deliveries fired(1);
    chA->callMethodAsyncWithError(kToken, QStringLiteral("fire"), {}, 5000,
                                  [&fired](QVariant v, const logos::CallError& e) {
                                      fired.record(0, std::move(v), e);
                                  });
    {
        QElapsedTimer t;
        t.start();
        while (!handlerRunning.load() && t.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    ASSERT_TRUE(handlerRunning.load()) << "the event handler never ran";

    Deliveries d(1);
    QElapsedTimer deadline;
    deadline.start();
    chB->callMethodAsyncWithError(kToken, QStringLiteral("block"), {}, 250,
                                  [&d](QVariant v, const logos::CallError& e) {
                                      d.record(0, std::move(v), e);
                                  });
    pumpUntilTotal(d, 1, 4000);
    const qint64 firedAt = deadline.elapsed();

    std::cout << "  io thread blocked with no end in sight; a 250ms deadline "
              << (d.total.load() ? "fired at " : "NEVER FIRED (")
              << firedAt << "ms" << (d.total.load() ? "" : ")")
              << ", handler still blocked: " << (!handlerDone.load()) << std::endl;

    EXPECT_FALSE(handlerDone.load())
        << "the handler unblocked itself; the test proved nothing";
    EXPECT_EQ(d.total.load(), 1)
        << "the deadline never fired: it is waiting for an io thread that is "
           "never coming back";
    if (d.total.load() == 1) {
        EXPECT_EQ(d.code(), "timeout");
        EXPECT_LT(firedAt, 1500);
    }

    {
        std::lock_guard<std::mutex> g(mu);
        letHandlerGo = true;
    }
    cv.notify_all();
    host.provider().letGo();
    {
        QElapsedTimer t;
        t.start();
        while (!handlerDone.load() && t.elapsed() < 8000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    pump(300);
    a->release();
    b->release();
    pump(100);
}

// The same deadline, with nothing in the way: the accuracy the timer thread
// delivers when the process is idle. This is the baseline the two tests above
// are compared against, and it is what a caller's timeoutMs actually means.
TEST_F(IoFoldTest, DeadlineAccuracyWhenIdle)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* obj = conn->requestObject(QStringLiteral("omni_module"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);

    constexpr int kRounds = 12;
    constexpr int kDeadlineMs = 200;
    std::vector<qint64> observed;
    for (int i = 0; i < kRounds; ++i) {
        Deliveries d(1);
        QElapsedTimer t;
        t.start();
        ch->callMethodAsyncWithError(kToken, QStringLiteral("block"), {}, kDeadlineMs,
                                     [&d](QVariant v, const logos::CallError& e) {
                                         d.record(0, std::move(v), e);
                                     });
        pumpUntilTotal(d, 1, 5000);
        ASSERT_EQ(d.total.load(), 1);
        ASSERT_EQ(d.code(), "timeout");
        observed.push_back(t.elapsed());
    }
    std::sort(observed.begin(), observed.end());
    const qint64 lo = observed.front();
    const qint64 med = observed[observed.size() / 2];
    const qint64 hi = observed.back();
    std::cout << "  " << kRounds << " idle " << kDeadlineMs
              << "ms deadlines: min=" << lo << " median=" << med
              << " max=" << hi << "ms" << std::endl;

    EXPECT_GE(lo, kDeadlineMs - 20) << "a deadline fired early";
    // Generous, because the delivery hop through the Qt event loop and this
    // test's own 5ms pump granularity are both inside the measurement.
    EXPECT_LE(med, kDeadlineMs + 120);
    EXPECT_LE(hi, kDeadlineMs + 400);

    host.provider().letGo();
    obj->release();
    pump(100);
}

// ── 4. retention: what a call that is never answered leaves behind ──────────
//
// TWO registries, and until this change only one of them was emptied. The
// handle's CallState::inflight is erased by the delivery. The CONNECTION's
// m_pendingCalls was erased by exactly two events — a decoded reply carrying
// that id, and fail()'s teardown sweep — so a call resolved by its DEADLINE was
// in neither, and its registration stayed for the life of the connection, which
// outlives every handle it hands out. That was true of the promise it held
// before the fold too; the fold made the orphan bigger (a handler closing over
// the caller's std::function rather than a promise), so it is closed here
// rather than inherited: AsyncCall::deliver() withdraws the registration.
TEST_F(IoFoldTest, CallsResolvedByTheirDeadlineLeaveNothingBehind)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* obj = conn->requestObject(QStringLiteral("omni_module"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);
    auto* plain = dynamic_cast<PlainLogosObject*>(obj);
    ASSERT_NE(plain, nullptr);

    // Every one of these times out against a provider parked in `block`, and is
    // never answered — the exact shape nothing used to clean up.
    constexpr int kCalls = 200;
    Deliveries d(kCalls);
    for (int i = 0; i < kCalls; ++i) {
        ch->callMethodAsyncWithError(kToken, QStringLiteral("block"), {}, 60,
                                     [&d, i](QVariant v, const logos::CallError& e) {
                                         d.record(i, std::move(v), e);
                                     });
    }
    pumpUntilTotal(d, kCalls, 30000);
    pump(300);

    const Registries r = registries(plain);
    std::cout << "  " << kCalls << " deadline-orphaned calls -> inflight="
              << r.inflight << " deferred=" << r.deferred
              << " connection-pending=" << r.pendingOnConnection
              << " deliveries=" << d.total.load() << " worst=" << d.worst()
              << " code='" << d.code() << "'" << std::endl;

    EXPECT_EQ(d.total.load(), kCalls);
    EXPECT_EQ(d.worst(), 1);
    EXPECT_EQ(d.code(), "timeout");
    EXPECT_EQ(r.inflight, 0u);
    EXPECT_EQ(r.deferred, 0u);
    EXPECT_EQ(r.pendingOnConnection, 0u)
        << r.pendingOnConnection << " of " << kCalls << " calls left their "
        << "registration in the connection's pending map: retention is growing "
        << "with call count on the connection, which outlives every handle.";

    host.provider().letGo();
    pump(200);
    obj->release();
    pump(100);
}

// A "multi" provider can answer the pending sentinel AFTER the caller's deadline
// has passed. The deadline has already resolved the call by then; the reply
// handler then arrives, sees a sentinel, and — in the first cut of this design —
// filed the AsyncCall under CallState::deferred and re-armed. Nothing took it out
// again, because the re-armed deadline's deliver() returned at the exactly-once
// gate before reaching the erase: one leaked map entry per slow-sentinel call,
// which is exactly the retention the fold exists to remove, reintroduced by a
// different route.
//
// The fix is two-part and both parts are load-bearing: deliver() leaves the
// registries BEFORE the gate rather than after it, and the reply handler declines
// to file a call that is already delivered.
TEST_F(IoFoldTest, ASentinelArrivingAfterItsDeadlineDoesNotLeakARegistryEntry)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* obj = conn->requestObject(QStringLiteral("omni_module"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);
    auto* plain = dynamic_cast<PlainLogosObject*>(obj);
    ASSERT_NE(plain, nullptr);

    // The provider sleeps 400ms per call on its single proxy thread, so these
    // serialize; each one's 100ms deadline is long gone when its sentinel lands.
    constexpr int kCalls = 6;
    Deliveries d(kCalls);
    for (int i = 0; i < kCalls; ++i) {
        ch->callMethodAsyncWithError(kToken, QStringLiteral("slowsink"), {}, 100,
                                     [&d, i](QVariant v, const logos::CallError& e) {
                                         d.record(i, std::move(v), e);
                                     });
    }
    pumpUntilTotal(d, kCalls, 20000);
    // Every call has timed out; now let the late sentinels arrive and be
    // processed. This is the window the leak opens in.
    pump(3000);

    const Registries r = registries(plain);
    std::cout << "  " << kCalls << " late-sentinel calls -> inflight=" << r.inflight
              << " deferred=" << r.deferred << " connection-pending="
              << r.pendingOnConnection << " deliveries=" << d.total.load()
              << " worst=" << d.worst() << " code='" << d.code() << "'" << std::endl;

    EXPECT_EQ(d.total.load(), kCalls);
    EXPECT_EQ(d.worst(), 1);
    EXPECT_EQ(d.code(), "timeout");
    EXPECT_EQ(r.inflight, 0u);
    EXPECT_EQ(r.deferred, 0u)
        << "a sentinel that arrived after its own deadline left " << r.deferred
        << " entries behind: retention is growing with call count again";
    EXPECT_EQ(r.pendingOnConnection, 0u);

    obj->release();
    pump(100);
}

// ── 5. release racing an in-flight COMPLETION ───────────────────────────────
//
// The hazard that already existed, and the second strong exactly-once detector.
// The completion-event subscription runs on the Asio io thread, RpcConnection
// invokes it with its own lock released, and NOTHING joins that thread. Under
// the thread-per-call design the subscription captured raw `this`, so release()'s
// `delete this` could land inside the callback — reproduced as a SIGSEGV under
// Guard Malloc in test_plain_completion_sub_lifetime.cpp, which is a separate
// change from this one and where that detector lives.
//
// What this adds is the FOLD's version of the same race: teardown and the
// completion handler both trying to resolve the same call. It is one of the two
// places where two resolvers genuinely arrive at deliver() for one call, so it
// is also where a broken exactly-once gate shows up.
//
// Run it under Guard Malloc to make a freed access fatal rather than
// probabilistic:
//   DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib \
//     ./protocol_tests --gtest_filter='IoFoldTest.ReleaseRacing*'
TEST_F(IoFoldTest, ReleaseRacingAnInFlightCompletionIsSafe)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    constexpr int kRounds = 300;
    int done = 0;
    int doubled = 0;
    QElapsedTimer total;
    total.start();

    for (int i = 0; i < kRounds; ++i) {
        LogosObject* obj = conn->requestObject(QStringLiteral("omni_module"), 5000);
        ASSERT_NE(obj, nullptr);
        auto* ch = channelFor(obj);
        ASSERT_NE(ch, nullptr);

        auto d = std::make_shared<Deliveries>(1);
        // 0..~2.4ms of completion delay, swept, so the release below lands
        // before, during and after the completion callback across the run.
        ch->callMethodAsyncWithError(kToken, QStringLiteral("defer"),
                                     QVariantList{ QVariant((i % 25) * 100) }, 8000,
                                     [d](QVariant v, const logos::CallError& e) {
                                         d->record(0, std::move(v), e);
                                     });
        if (i % 4 != 0)
            QThread::usleep(static_cast<unsigned long>((i % 25) * 100));

        obj->release();

        // Whatever the race decided, the caller is told once: either the
        // completion landed first (value 7) or the release did (transport_error).
        pumpUntilTotal(*d, 1, 5000);
        pump(5);
        if (d->total.load() > 1) ++doubled;
        ASSERT_EQ(d->total.load(), 1)
            << "round " << i << ": " << d->total.load() << " callbacks, not one";
        ++done;
    }

    std::cout << "  " << done << "/" << kRounds
              << " release-during-completion rounds, exactly one callback each, in "
              << total.elapsed() << "ms (double deliveries: " << doubled << ")"
              << std::endl;
    EXPECT_EQ(done, kRounds);
    EXPECT_EQ(doubled, 0);
}

// ── why "wait for the io thread" was not an option ──────────────────────────
//
// The obvious alternative to shared ownership is a barrier: post a no-op onto
// the strand at teardown and block until it runs, which would prove no handler
// is mid-flight. It cannot be used here, and this is the reason.
//
// IoContextPool runs EXACTLY ONE worker thread and is a process-wide singleton
// (io_context_pool.cpp), and the plain transport delivers user event callbacks
// INLINE on it (rpc_connection.h). So a user handler that releases its handle —
// the reentrant-release-from-event-dispatch shape remote_transport.cpp documents
// as shipped production behaviour — is running ON the only thread that could
// ever drain that barrier. It would wedge the process. (Measured: it does.)
//
// This design has nothing to wait for, so the same call just returns. The
// watchdog turns a regression into a named abort rather than a CI job that hangs
// until its timeout.
TEST_F(IoFoldTest, ReleaseFromInsideAnIoThreadEventCallbackDoesNotWedge)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* obj = conn->requestObject(QStringLiteral("omni_module"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);

    std::atomic<bool> releasedFromCallback{false};
    std::atomic<bool> onIoThread{false};
    const std::thread::id mainThread = std::this_thread::get_id();

    obj->onEvent(QStringLiteral("tick"),
                 [&](const QString&, const QVariantList&) {
                     onIoThread.store(std::this_thread::get_id() != mainThread);
                     obj->release();                 // reentrant, on the io thread
                     releasedFromCallback.store(true);
                 });

    // A call left outstanding, so the reentrant release has real work to do:
    // it must cancel this and deliver its callback.
    Deliveries d(1);
    ch->callMethodAsyncWithError(kToken, QStringLiteral("sink"), {}, 9000,
                                 [&d](QVariant v, const logos::CallError& e) {
                                     d.record(0, std::move(v), e);
                                 });
    pump(200);
    ASSERT_EQ(d.total.load(), 0);

    // A watchdog, because the failure mode is a hang and not an assertion.
    std::atomic<bool> finished{false};
    std::thread dog([&] {
        for (int i = 0; i < 200 && !finished.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!finished.load()) {
            std::fprintf(stderr, "\nWATCHDOG: reentrant release() from an io-thread "
                                 "event callback wedged the process.\n");
            std::fflush(stderr);
            std::abort();
        }
    });

    // Fire the event. The provider's own reply to `fire` is irrelevant; what
    // matters is that the event handler runs on the io thread and releases.
    auto fired = std::make_shared<Deliveries>(1);
    ch->callMethodAsyncWithError(kToken, QStringLiteral("fire"), {}, 5000,
                                 [fired](QVariant v, const logos::CallError& e) {
                                     fired->record(0, std::move(v), e);
                                 });

    QElapsedTimer t;
    t.start();
    while (!releasedFromCallback.load() && t.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    finished.store(true);
    dog.join();

    pumpUntilTotal(d, 1, 3000);
    pump(300);

    std::cout << "  reentrant release from the io thread returned after "
              << t.elapsed() << "ms (on io thread: " << onIoThread.load()
              << "), abandoned call delivered " << d.total.load()
              << " time(s) code='" << d.code() << "'" << std::endl;

    EXPECT_TRUE(releasedFromCallback.load())
        << "release() never returned from inside the io-thread event callback";
    EXPECT_TRUE(onIoThread.load())
        << "the event did not arrive on the io thread — this test is not "
           "exercising the reentrancy it claims to";
    EXPECT_EQ(d.total.load(), 1);
    EXPECT_EQ(d.code(), "transport_error");

    host.provider().letGo();
    pump(200);
}
