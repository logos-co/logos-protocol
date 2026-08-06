// A long-lived PlainLogosObject must not accumulate the waiters of calls that
// have already finished.
//
// Its sibling suite (test_plain_object_teardown.cpp) pins what happens to a
// waiter that is still IN FLIGHT when the handle goes away. This one pins the
// other half: what is left behind by a call that COMPLETED NORMALLY.
//
// The defect these tests exist to prevent. Waiter threads are joined rather
// than detached, because they capture `this` and release() deletes it. But a
// thread cannot join itself, so a waiter cannot retire its own entry, and while
// the registry was a plain vector the only code that ever emptied it was
// teardown. Every completed async call therefore parked a finished-but-unjoined
// std::thread for the whole life of the handle — an exited thread whose stack
// and pthread struct are not reclaimed until somebody joins it, measured at
// ~16KB resident per call on 16KiB-page arm64 (one page; expect less on 4KiB
// Linux, but the growth is the platform-independent part). The production shape
// makes that unbounded rather than academic: LogosAPIConsumer caches ONE handle
// per module and reuses it for every async call, releasing it only on eviction
// or teardown (cpp/logos_api_consumer.cpp), so 30k calls on one handle cost
// ~470MB that never comes back.
//
// What is pinned here:
//
//   1. THE REGISTRY DOES NOT GROW WITH CALL COUNT. Counting threads, not bytes:
//      RSS is a noisy proxy and its per-call constant is platform-specific,
//      whereas "m_waiters.size() rises 1:1 with completed calls and only ever
//      falls in teardown" is the defect itself, exactly and portably. So a
//      sequential caller keeps ~1 and NOT ~N.
//
//   1b. AND IT DRAINS WITHOUT ANOTHER CALL. Reaping on the spawn path alone
//      leaves the tail of a burst parked until the next call, which for a
//      module that bursts and then goes quiet may never come: 2000 completed
//      calls kept 1428 waiters and 24MiB once the handle went idle, and one
//      further call dropped that to 1. Waiters therefore reap each other on
//      their way out, and this pins the IDLE bound with no further spawn.
//
//   2. THE DEADLOCK THE FIX COULD INTRODUCE. Reaping means joining, and a
//      reaper that joined while holding the lock a waiter needs in order to
//      announce itself would wedge the process. Hammered here with reaps and
//      publishes deliberately overlapped, under a watchdog so a regression is a
//      named failure rather than a CI job that hangs until its timeout.
//
//   3. REAPING DOES NOT BREAK TEARDOWN. Completed calls being retired early
//      must not lose the join for the one still outstanding, and the callback
//      contract stays exactly-once across a run where both happen.
//
// m_waiters is private and stays private: the test reads it through the
// explicit-instantiation access hole ([temp.spec] does not check access on the
// template arguments of an explicit instantiation), so the code under test is
// observed exactly as it ships — no `friend`, no test-only accessor, no
// #define private public.

#include <gtest/gtest.h>

#include "logos_call_error.h"
#include "logos_object.h"
#include "logos_provider_interface.h"
#include "logos_transport_config.h"
#include "module_proxy.h"

#include "plain_transport_connection.h"
#include "plain_transport_host.h"

#include "plain_logos_object.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QString>
#include <QThread>
#include <QVariant>
#include <QVariantList>

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

using namespace logos::plain;

namespace {

// ── reading m_waiters without touching the production header ────────────────
template <typename Tag, typename Tag::type Member>
struct Rob {
    friend typename Tag::type get(Tag) { return Member; }
};

struct WaitersTag {
    using type = std::map<std::uint64_t, std::thread> PlainLogosObject::*;
    friend type get(WaitersTag);
};
template struct Rob<WaitersTag, &PlainLogosObject::m_waiters>;

struct WaiterMuTag {
    using type = std::mutex PlainLogosObject::*;
    friend type get(WaiterMuTag);
};
template struct Rob<WaiterMuTag, &PlainLogosObject::m_waiterMu>;

// Taken under the object's OWN mutex — the one the registration path holds —
// so this is a consistent read, not a torn one.
size_t waiterCount(PlainLogosObject* obj)
{
    auto& mu = obj->*get(WaiterMuTag{});
    auto& m  = obj->*get(WaitersTag{});
    std::lock_guard<std::mutex> g(mu);
    return m.size();
}

// Answers `ping` immediately — every call in the retention tests COMPLETES,
// which is the case that leaks — and parks on `block` until the test lets go,
// for the one place that needs a call genuinely still in flight.
class EchoProvider : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList& args) override
    {
        if (method == QLatin1String("ping")) return args.value(0, QVariant(1));
        if (method == QLatin1String("block")) {
            std::unique_lock<std::mutex> lk(m_mu);
            m_cv.wait(lk, [this] { return m_released; });
            return QVariant(42);
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
    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("echo"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }

private:
    std::mutex              m_mu;
    std::condition_variable m_cv;
    bool                    m_released = false;
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

        m_published = m_host->publishObject("echo_module", m_proxy);
        const QString endpoint = m_host->endpoint();
        m_port = endpoint.mid(endpoint.lastIndexOf(':') + 1).toUShort();
    }

    ~LiveHost()
    {
        // Anything still parked in the provider would deadlock the thread quit
        // below; let every blocked (and queued) call finish first.
        m_provider.letGo();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        m_host.reset();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        m_thread->quit();
        m_thread->wait();
        delete m_proxy;
        delete m_thread;
    }

    bool ok() const { return m_started && m_published && m_port != 0; }
    uint16_t port() const { return m_port; }

private:
    EchoProvider m_provider;
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

// Per-call delivery counts, so "exactly once" is checked per call and not just
// in aggregate — a double delivery on one call plus a dropped one on another
// would balance out in a total.
struct Deliveries {
    explicit Deliveries(int n) : counts(n) {}
    std::vector<std::atomic<int>> counts;
    std::atomic<int> total{0};
    std::atomic<int> errors{0};

    std::mutex  codeMu;
    std::string lastCode;

    void record(int i, const logos::CallError& e)
    {
        {
            std::lock_guard<std::mutex> g(codeMu);
            lastCode = e.code;
        }
        counts[i].fetch_add(1);
        total.fetch_add(1);
        if (!e.code.empty()) errors.fetch_add(1);
    }
    std::string code()
    {
        std::lock_guard<std::mutex> g(codeMu);
        return lastCode;
    }
    int worst() const   // the largest per-call count seen
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

void pumpUntilTotal(Deliveries& d, int target, int budgetMs)
{
    QElapsedTimer t;
    t.start();
    while (d.total.load() < target && t.elapsed() < budgetMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

const char* kToken = "live-token";

// A deadlock does not fail a test, it hangs it — and a hung gtest binary is a
// CI job that dies on a timeout somewhere far from the cause. This turns that
// into a loud, attributable abort. The budget is ~50x the measured runtime of
// the hammer below, so it can only fire on a genuine wedge.
class Watchdog {
public:
    Watchdog(const char* what, int budgetMs)
        : m_what(what)
    {
        m_thread = std::thread([this, budgetMs] {
            std::unique_lock<std::mutex> lk(m_mu);
            if (!m_cv.wait_for(lk, std::chrono::milliseconds(budgetMs),
                               [this] { return m_done; })) {
                std::fprintf(stderr,
                    "\nWATCHDOG: '%s' made no progress for %dms — the reaper is "
                    "deadlocked against a waiter trying to publish.\n",
                    m_what, budgetMs);
                std::fflush(stderr);
                std::abort();
            }
        });
    }
    ~Watchdog()
    {
        {
            std::lock_guard<std::mutex> g(m_mu);
            m_done = true;
        }
        m_cv.notify_all();
        m_thread.join();
    }

private:
    const char*             m_what;
    std::mutex              m_mu;
    std::condition_variable m_cv;
    bool                    m_done = false;
    std::thread             m_thread;
};

} // namespace

class PlainWaiterReapingTest : public ::testing::Test {
protected:
    void SetUp() override { ensureApp(); }
};

// ── 1. the registry does not grow with call count ───────────────────────────
//
// One handle, N completed calls, issued strictly sequentially: each callback is
// awaited before the next call goes out, which is both the realistic shape and
// the harshest one for the claim — with at most one call ever in flight, a
// correct implementation keeps ~1 waiter no matter how large N is.
//
// Pre-fix this ends at N.
TEST_F(PlainWaiterReapingTest, SequentialCompletedCallsDoNotAccumulateWaiters)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* obj = conn->requestObject(QStringLiteral("echo_module"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);
    auto* plain = dynamic_cast<PlainLogosObject*>(obj);
    ASSERT_NE(plain, nullptr) << "the plain transport must hand back a PlainLogosObject";

    constexpr int kCalls = 200;
    Deliveries d(kCalls);

    size_t peak = 0;
    for (int i = 0; i < kCalls; ++i) {
        ch->callMethodAsyncWithError(kToken, QStringLiteral("ping"),
                                     QVariantList{ QVariant(i) }, 5000,
                                     [&d, i](QVariant, const logos::CallError& e) {
                                         d.record(i, e);
                                     });
        pumpUntilTotal(d, i + 1, 10000);
        ASSERT_EQ(d.total.load(), i + 1) << "call " << i << " never delivered";
        peak = std::max(peak, waiterCount(plain));
    }

    const size_t finalCount = waiterCount(plain);
    std::cout << "  " << kCalls << " sequential completed calls -> m_waiters peak="
              << peak << " final=" << finalCount << std::endl;

    EXPECT_EQ(d.errors.load(), 0) << "a completed call reported an error";
    EXPECT_EQ(d.worst(), 1) << "a callback fired more than once";
    EXPECT_EQ(d.missing(), 0) << "a callback never fired";

    // The bound that matters is "does not scale with kCalls". 8 is generous
    // headroom over the 1-2 this actually keeps (the current call's waiter, and
    // at most the previous one if it published after the current spawn reaped),
    // while still being 25x below the kCalls this fails at when nothing prunes.
    EXPECT_LE(finalCount, 8u)
        << "finished waiters are accumulating: " << finalCount << " left after "
        << kCalls << " completed calls";
    EXPECT_LE(peak, 8u) << "the registry grew during the run";

    obj->release();
    pump(50);
}

// The same claim with calls in flight concurrently: the bound is then peak
// concurrency, since a waiter can only be reaped once it has finished. What must
// still hold is that it does not scale with the number of CALLS.
TEST_F(PlainWaiterReapingTest, ConcurrentCompletedCallsStayBoundedByInFlight)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* obj = conn->requestObject(QStringLiteral("echo_module"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);
    auto* plain = dynamic_cast<PlainLogosObject*>(obj);
    ASSERT_NE(plain, nullptr);

    constexpr int kCalls    = 600;
    constexpr int kInflight = 8;
    Deliveries d(kCalls);

    size_t peak = 0;
    int issued = 0;
    while (issued < kCalls) {
        while (issued < kCalls && (issued - d.total.load()) < kInflight) {
            const int i = issued++;
            ch->callMethodAsyncWithError(kToken, QStringLiteral("ping"),
                                         QVariantList{ QVariant(i) }, 5000,
                                         [&d, i](QVariant, const logos::CallError& e) {
                                             d.record(i, e);
                                         });
        }
        peak = std::max(peak, waiterCount(plain));
        pumpUntilTotal(d, issued - kInflight + 1, 10000);
    }
    pumpUntilTotal(d, kCalls, 20000);
    pump(200);   // a duplicate delivery would land here

    const size_t finalCount = waiterCount(plain);
    std::cout << "  " << kCalls << " calls at " << kInflight
              << " in flight -> m_waiters peak=" << peak
              << " final=" << finalCount << std::endl;

    EXPECT_EQ(d.total.load(), kCalls);
    EXPECT_EQ(d.worst(), 1);
    EXPECT_EQ(d.missing(), 0);
    EXPECT_EQ(d.errors.load(), 0);

    // Bounded by the in-flight window plus the slack of one reap cycle — not by
    // kCalls, which is what it reaches when finished waiters are never dropped.
    EXPECT_LE(finalCount, size_t(4 * kInflight))
        << "finished waiters accumulated past the in-flight window";
    EXPECT_LE(peak, size_t(4 * kInflight));

    obj->release();
    pump(50);
}

// ── 1b. a burst that goes idle drains itself ────────────────────────────────
//
// The test above always has another call coming, which hides the case that
// actually shows up in production: a module bursts, every call completes, and
// then the handle goes quiet. If reaping only ever happened on the spawn path,
// everything that finished after the LAST spawn would stay parked for the life
// of the handle — measured at 1428 waiters and +24MiB after 2000 completed
// calls, collapsing to 1 the moment one further call was issued.
//
// So the bound is read here with NO further call: the burst has to have drained
// itself. What remains is what published after the last reap — at minimum the
// last waiter to finish, which has nobody behind it to collect it (1 in almost
// every run, 2 when a waiter's publish slips past the final reap). The bound
// below is generous against that and still ~100x under the pre-fix number.
TEST_F(PlainWaiterReapingTest, BurstThatGoesIdleDrainsWithoutAnotherCall)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* obj = conn->requestObject(QStringLiteral("echo_module"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);
    auto* plain = dynamic_cast<PlainLogosObject*>(obj);
    ASSERT_NE(plain, nullptr);

    // Issued in one go, with no pumping in between, so they really are
    // concurrent and the tail of the burst is large.
    constexpr int kBurst = 800;
    Deliveries d(kBurst);
    for (int i = 0; i < kBurst; ++i) {
        ch->callMethodAsyncWithError(kToken, QStringLiteral("ping"),
                                     QVariantList{ QVariant(i) }, 9000,
                                     [&d, i](QVariant, const logos::CallError& e) {
                                         d.record(i, e);
                                     });
    }
    pumpUntilTotal(d, kBurst, 60000);
    ASSERT_EQ(d.total.load(), kBurst) << "the burst did not all complete";

    // Every callback has landed; now let the waiters that delivered them finish
    // and retire each other. No call is issued in this window — that is the
    // whole point — so anything still registered is retained, not in flight.
    for (int i = 0; i < 40; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(10);
    }

    const size_t idle = waiterCount(plain);
    std::cout << "  " << kBurst << " completed calls then IDLE -> m_waiters="
              << idle << std::endl;

    EXPECT_EQ(d.worst(), 1);
    EXPECT_EQ(d.missing(), 0);
    EXPECT_EQ(d.errors.load(), 0);
    EXPECT_LE(idle, 8u)
        << "a burst that went idle left " << idle << " of " << kBurst
        << " waiters parked: they are only being reaped on the spawn path";

    // And the handle still works afterwards — draining from inside the waiters
    // must not have disturbed the object they are draining.
    Deliveries after(1);
    ch->callMethodAsyncWithError(kToken, QStringLiteral("ping"),
                                 QVariantList{ QVariant(7) }, 5000,
                                 [&after](QVariant, const logos::CallError& e) {
                                     after.record(0, e);
                                 });
    pumpUntilTotal(after, 1, 10000);
    EXPECT_EQ(after.total.load(), 1);
    EXPECT_EQ(after.errors.load(), 0);
    EXPECT_LE(waiterCount(plain), 8u);

    obj->release();
    pump(50);
}

// ── 2. the deadlock the fix could introduce ─────────────────────────────────
//
// A waiter announces itself as finished under m_waiterMu; the reaper takes that
// list under the same lock and then JOINS. If it joined while still holding the
// lock, a waiter blocked on that lock trying to announce itself would never
// return and the join would never complete — a two-thread deadlock, taking out
// the caller's thread (in production, usually the Qt event loop).
//
// So the two are deliberately overlapped: every spawn reaps, and the calls are
// short enough that waiters are finishing while later ones are being registered.
// The watchdog turns a wedge into an abort that names the cause.
TEST_F(PlainWaiterReapingTest, ReapingRacesPublishingWithoutDeadlocking)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    constexpr int kRounds       = 40;
    constexpr int kPerRound     = 40;
    constexpr int kCalls        = kRounds * kPerRound;
    // ~2s in practice; 60s can only be reached by a genuine wedge.
    Watchdog watchdog("ReapingRacesPublishingWithoutDeadlocking", 60000);

    LogosObject* obj = conn->requestObject(QStringLiteral("echo_module"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);
    auto* plain = dynamic_cast<PlainLogosObject*>(obj);
    ASSERT_NE(plain, nullptr);

    Deliveries d(kCalls);
    QElapsedTimer total;
    total.start();

    int issued = 0;
    for (int r = 0; r < kRounds; ++r) {
        // A burst with no pumping between the calls: the earlier waiters of the
        // burst finish (and publish) while the later ones are still being
        // registered and reaping, so publish and reap collide inside the burst.
        for (int k = 0; k < kPerRound; ++k) {
            const int i = issued++;
            ch->callMethodAsyncWithError(kToken, QStringLiteral("ping"),
                                         QVariantList{ QVariant(i) }, 5000,
                                         [&d, i](QVariant, const logos::CallError& e) {
                                             d.record(i, e);
                                         });
        }
        // Varying drift, so the burst boundary lands at different points of the
        // previous burst's completion — sometimes reaping nothing, sometimes
        // reaping a batch that is still growing under it.
        if (r % 4 != 0)
            QThread::usleep(static_cast<unsigned long>((r % 17) * 60));
        pumpUntilTotal(d, issued - kPerRound, 20000);
    }
    pumpUntilTotal(d, kCalls, 30000);
    pump(200);

    const qint64 elapsed = total.elapsed();
    std::cout << "  " << kCalls << " calls across " << kRounds
              << " bursts in " << elapsed << "ms, m_waiters="
              << waiterCount(plain) << std::endl;

    EXPECT_EQ(d.total.load(), kCalls) << "callbacks went missing under the race";
    EXPECT_EQ(d.worst(), 1) << "a callback fired more than once under the race";
    EXPECT_EQ(d.missing(), 0);
    EXPECT_LE(waiterCount(plain), size_t(4 * kPerRound))
        << "the registry grew across the bursts";

    obj->release();
    pump(50);
}

// ── 3. reaping must not cost teardown its join ──────────────────────────────
//
// Completed calls being retired early must not disturb the outstanding one: the
// object is released while a call is still in flight, after many others have
// already been reaped. release() must stay fast (it cancels rather than waiting
// the timeout out) and the abandoned call must still deliver, once.
TEST_F(PlainWaiterReapingTest, TeardownAfterReapingStillJoinsAndDeliversOnce)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* obj = conn->requestObject(QStringLiteral("echo_module"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);
    auto* plain = dynamic_cast<PlainLogosObject*>(obj);
    ASSERT_NE(plain, nullptr);

    constexpr int kWarm = 100;
    Deliveries warm(kWarm);
    for (int i = 0; i < kWarm; ++i) {
        ch->callMethodAsyncWithError(kToken, QStringLiteral("ping"),
                                     QVariantList{ QVariant(i) }, 5000,
                                     [&warm, i](QVariant, const logos::CallError& e) {
                                         warm.record(i, e);
                                     });
        pumpUntilTotal(warm, i + 1, 10000);
    }
    ASSERT_EQ(warm.total.load(), kWarm);
    ASSERT_LE(waiterCount(plain), 8u) << "the warmup calls were not reaped";

    // Now release with a call that REALLY is outstanding: `block` parks in the
    // provider until the host is torn down, so the waiter is unambiguously
    // mid-wait when release() lands, rather than racing a `ping` that may have
    // already answered.
    Deliveries last(1);
    ch->callMethodAsyncWithError(kToken, QStringLiteral("block"), {}, 8000,
                                 [&last](QVariant, const logos::CallError& e) {
                                     last.record(0, e);
                                 });
    pump(200);
    ASSERT_EQ(last.total.load(), 0) << "the provider answered; nothing was in flight";

    QElapsedTimer timer;
    timer.start();
    obj->release();
    const qint64 releaseMs = timer.elapsed();

    pumpUntilTotal(last, 1, 3000);
    pump(300);   // a second delivery would land here

    std::cout << "  release() after " << kWarm << " reaped calls took "
              << releaseMs << "ms, in-flight call delivered "
              << last.total.load() << " time(s) code='" << last.code() << "'"
              << std::endl;

    EXPECT_LT(releaseMs, 750)
        << "release() waited out the in-flight call instead of cancelling it";
    // The join that keeps `this` alive under the waiter is still there, and the
    // abandoned call is still told — once. Reaping the finished waiters must
    // change neither.
    EXPECT_EQ(last.total.load(), 1) << "the in-flight call did not deliver exactly once";
    EXPECT_EQ(last.code(), "transport_error");
    EXPECT_EQ(warm.worst(), 1);
}
