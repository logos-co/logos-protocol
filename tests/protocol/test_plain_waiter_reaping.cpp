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
//      HOW NOISY, since two commit messages on this branch have now quoted a
//      "bytes per call" figure off a single sample. 10k lp_invoke_async on one
//      lp_client, run ten times, gave 0, 5, 5, 5, 7, 7, 8, 10, 13, 10 bytes per
//      call (mean 7.0); ten more gave 3, 11, 8, 5, 8, 10, 13, 8, 3, 10 (mean
//      7.9). One distribution, range 0-13, and the "+0.09 MiB / 10 B per call"
//      of 8f0c60f and the "~6 B/call" offered as its correction are both draws
//      from it — neither arithmetic was wrong. THE HONEST STATEMENT IS THAT
//      RETENTION IS FLAT: indistinguishable from zero, RSS noise rather than a
//      per-call rate. Quote a number off one run of this and the next run will
//      correct you.
//
//   1b. AND IT DRAINS WITHOUT ANOTHER CALL. Reaping on the spawn path alone
//      leaves the tail of a burst parked until the next call, which for a
//      module that bursts and then goes quiet may never come: 2000 completed
//      calls kept ~1400 waiters and 24MiB once the handle went idle, and one
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
// which is the case that leaks — and parks on `gate` / `block` until the test
// lets go. The two parked flavours differ in WHAT releases them: `block` waits
// for a single all-or-nothing release, for the one place that needs a call
// genuinely still in flight, while `gate` waits for its TURN, which is what lets
// the burst below hold every reply until the whole burst is outstanding and then
// hand them back at a rate the test controls.
class EchoProvider : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList& args) override
    {
        if (method == QLatin1String("ping")) {
            m_answered.fetch_add(1);
            return args.value(0, QVariant(1));
        }
        if (method == QLatin1String("gate")) {
            if (!awaitTurn()) m_gateExpired.store(true);
            m_answered.fetch_add(1);
            return args.value(0, QVariant(1));
        }
        if (method == QLatin1String("block")) {
            await();
            m_answered.fetch_add(1);
            return QVariant(42);
        }
        return QVariant();
    }

    // Release everything, parked and future, `gate` and `block` alike.
    void letGo()
    {
        {
            std::lock_guard<std::mutex> g(m_mu);
            m_released = true;
            m_allowed  = kAll;
        }
        m_cv.notify_all();
    }

    // Let the first `n` gated calls through — the burst's pacing knob. Dispatch
    // is single-threaded, so arrival order is call order and this is exactly
    // "answer calls 0..n-1".
    void allow(int n)
    {
        {
            std::lock_guard<std::mutex> g(m_mu);
            if (n > m_allowed) m_allowed = n;
        }
        m_cv.notify_all();
    }

    // Replies PRODUCED so far, counted immediately before each one is handed
    // back to the host. This is the direct measure of "has anything answered
    // yet", direct in the sense that it does not go through the client at all —
    // so unlike a delivery count it cannot be fooled by a callback that has been
    // posted to the Qt event loop but not yet pumped, and unlike a registry size
    // it does not depend on who reaps what.
    int  answered() const { return m_answered.load(); }
    // True if a parked call gave up waiting instead of being released. A test
    // that forgets to open the gate must FAIL on this, not hang; see await().
    bool gateExpired() const { return m_gateExpired.load(); }

    QJsonArray getMethods() override { return QJsonArray{}; }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("echo"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }

private:
    // Park until the test lets go / until this call's turn comes. Returns false
    // if it gave up first.
    //
    // BOUNDED, and SELF-RELEASING on the way out, because both halves are what
    // keep a mistake here from becoming a hung binary. Provider dispatch is
    // single-threaded (PlainTransportHost posts each call to the proxy's thread
    // with a QueuedConnection), so a call parked here holds up every call behind
    // it: an unbounded wait would wedge the whole host if the test never opened
    // the gate, and a bounded wait that did NOT open it on the way out would
    // wedge it just as thoroughly, one budget at a time, 800 times over. Opening
    // it here means one budget is the WHOLE cost — after which every held call
    // answers, every callback fires, and the run ends in a named assertion
    // failure (gateExpired) instead of a timeout somewhere in CI. Measured: with
    // the release deleted, this test FAILS in 20.2s naming the gate, where the
    // unbounded version would have hung.
    bool await()
    {
        std::unique_lock<std::mutex> lk(m_mu);
        if (m_cv.wait_for(lk, kGateBudget, [this] { return m_released; }))
            return true;
        forceOpen();
        return false;
    }

    bool awaitTurn()
    {
        std::unique_lock<std::mutex> lk(m_mu);
        const int mine = m_arrived++;
        if (m_cv.wait_for(lk, kGateBudget, [this, mine] { return mine < m_allowed; }))
            return true;
        forceOpen();
        return false;
    }

    void forceOpen()   // called with m_mu held
    {
        m_released = true;
        m_allowed  = kAll;
    }

    // ~40x the slowest complete run of the burst below measured at any load, so
    // it can only be reached by a gate that is never opened at all.
    static constexpr std::chrono::seconds kGateBudget{20};
    static constexpr int                  kAll = 1 << 30;

    std::mutex              m_mu;
    std::condition_variable m_cv;
    bool                    m_released = false;
    int                     m_arrived  = 0;   // gated calls seen, under m_mu
    int                     m_allowed  = 0;   // gated calls permitted, under m_mu
    std::atomic<int>        m_answered{0};
    std::atomic<bool>       m_gateExpired{false};
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

    // Let every call parked in the provider's gate — and every one that arrives
    // after this — answer.
    void openGate() { m_provider.letGo(); }
    // Let the first `n` gated calls answer, and no more.
    void allow(int n) { m_provider.allow(n); }
    int  answered() const { return m_provider.answered(); }
    bool gateExpired() const { return m_provider.gateExpired(); }

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
// calls, collapsing to 1 the moment one further call was issued. Those figures
// came off a burst that was merely issued in a loop, so they are race-dependent
// and not constants: a re-measure of the same build gave 1421. The burst below
// is gated instead, and that is what turns the defect's side from "some large
// number" into kBurst exactly — see WHY THE PROVIDER IS GATED.
//
// So the bound is read here with NO further call: the burst has to have drained
// itself. What remains is whatever published after the FINAL reap — at minimum
// the last waiter to finish, which by construction has nobody behind it to
// collect it.
//
// WHY THE PROVIDER IS GATED, which is the whole design of this test. "Issue 800
// calls in a loop and hope they overlap" is not an experiment, it is a race
// between two rates: how fast the caller can spawn a std::thread, and how fast a
// loopback RPC can come back. On macOS the first is much cheaper than the second
// and the burst really is concurrent. On Linux they are comparable, so most of
// the burst COMPLETES WHILE IT IS STILL BEING ISSUED and gets collected by the
// SPAWN-path reaper — the very reaper this test is supposed to be doing without.
// Measured on the ungated version, calls still outstanding when the last one of
// the burst went out (n=25 per platform):
//
//                   in flight at the end of the issue loop, of 800
//   macOS           544-612      (68-77% of the burst)
//   Linux           0-237        (0-30%, median 85)
//
// On Linux, then, the thing being measured was ~85 concurrent calls with a spawn
// reaper running throughout — one run in 25 had the ENTIRE burst answered before
// the last call was issued — and not an 800-call burst going quiet. The residue
// it left was a draw from the scheduler rather than a property of the code, both
// arms drew from overlapping distributions, and NO bound could separate them:
// correct code reached 152 unloaded and 713 under load, the defect fell as low
// as 5. The bound before this commit (400) scored 25/25 on macOS and 0/40 on an
// idle Linux box; the bound before THAT (8) failed correct code 96 times in 160.
// Neither number was the problem.
//
// The gate removes the race instead of arbitrating it. Every call in the burst
// invokes `gate`, which parks in the provider; provider dispatch is
// single-threaded, so the first one to arrive holds up all 800 and NOT ONE REPLY
// EXISTS until the test opens the gate — asserted below off the provider's own
// counter, not inferred. The burst is then concurrent by construction on every
// platform: 800/800 in flight, measured, on both, at every load level tried.
//
// AND THE DRAIN IS PACED, which is the other half and is NOT the same thing. The
// first version of this released all 800 at once, and on Linux that replaced one
// scheduling artefact with another: 800 threads become runnable on 6 cores, and
// because a waiter reaps and only THEN publishes, a reaper that collects a large
// batch sits in its join loop while everyone behind it publishes — so the last
// exit batch is enormous. Correct code, 800 released together, 20 runs:
//
//   macOS   1 every run          Linux   2-389 (median ~190)
//
// That is not a defect, it is what the exit guard can and cannot do against a
// thundering herd, and it is recorded in plain_logos_object.h next to the
// mechanism. It is useless as a DETECTOR, because the defect's value (800) is
// only ~2x it. So the gate is opened kRelease at a time, each step awaited: the
// waiters then retire each other the way a real drain does, and the residue is
// the last step's exit batch instead of the burst's. Measured across release
// steps, correct code on Linux, idle: step 1 → 1, step 8 → 1-3, step 16 → 1-2,
// step 32 → 1-3, step 64 → 1-55, step 200 → 1-142, step 800 → 2-389. The knee is
// well above the step chosen below, and a smaller step buys nothing but runtime
// (step 8 costs 3x step 32 on a 64x-oversubscribed box).
//
// MEASURED, this gated burst, m_waiters when it goes idle. Numbers, n and
// margins are at the assertion below; the shape is:
//
//                     this code    reaping only on the spawn path
//   macOS             1-2          800   (exactly, every run)
//   Linux, any load   1-7          800   (exactly, every run)
//
// The defect's 800 is not "high", it is the whole burst: with no exit-guard reap
// and no further spawn, nothing on any platform ever removes an entry, so the
// count is kBurst exactly and the separation is not statistical. 800 live waiter
// threads at once is also the peak this now reaches where the ungated version
// reached ~150 on Linux, and that is affordable: the stacks are lazily faulted,
// so peak RSS for the whole run is 29MiB on Linux and 32MiB on macOS.
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

    // ~1s in practice. Every wait in this test is already bounded — the gate
    // self-releases, the pumps have budgets, the calls have timeouts — so this
    // is the backstop for a wedge INSIDE the code under test (a reaper deadlocked
    // against a publisher), which none of those would catch.
    Watchdog watchdog("BurstThatGoesIdleDrainsWithoutAnotherCall", 120000);

    // Issued in one go, with no pumping in between, against a CLOSED gate: no
    // reply can be produced until every one of them is outstanding.
    constexpr int kBurst = 800;
    // Gated calls released per step of the drain, each step awaited before the
    // next. See the header comment: this is what keeps the residue the last
    // step's exit batch rather than the burst's.
    constexpr int kRelease = 16;
    // The residue cannot exceed what the last release step can leave behind, so
    // the bound is stated against THAT and not against kBurst — 4x the step,
    // sized at the assertion below.
    constexpr size_t kDrained = 4 * kRelease;
    Deliveries d(kBurst);
    // Declared AFTER `d`, so it runs BEFORE it. The tail of this test used to be
    // a bare `obj->release(); pump(50);` on the happy path, which a FATAL
    // assertion skips — and skipping it is not merely untidy here. Every
    // outstanding callback holds a reference to `d`, and LiveHost's destructor
    // pumps the event loop, so bailing out with deliveries still in flight would
    // run them against a destroyed Deliveries. That is the one way this test
    // could answer a failure with a crash instead of a verdict, and the
    // assertions below (a gate that never opened, a burst that did not all
    // complete) are exactly the ones that would trigger it. release() cancels
    // and JOINS every waiter, and the pump behind it runs what they posted —
    // both while `d` is still alive.
    struct ReleaseOnExit {
        LogosObject* obj;
        ~ReleaseOnExit() { obj->release(); pump(50); }
    } releaseOnExit{obj};
    for (int i = 0; i < kBurst; ++i) {
        ch->callMethodAsyncWithError(kToken, QStringLiteral("gate"),
                                     QVariantList{ QVariant(i) }, 45000,
                                     [&d, i](QVariant, const logos::CallError& e) {
                                         d.record(i, e);
                                     });
    }

    // THE EXPERIMENT'S OWN PRECONDITION, measured rather than assumed — because
    // an unmeasured one is exactly how this test spent three revisions asserting
    // a bound on a burst that was not a burst. Two independent readings:
    //
    //   * the provider has produced NOTHING. Read from the provider itself, so
    //     it does not depend on the client, on who reaps what, or on the Qt
    //     event loop having been pumped (it has not been — a delivery count
    //     would read 0 here whether or not replies existed).
    //   * all 800 waiters are registered. A call that completed during the loop
    //     would have been reaped by one of the spawns behind it, so this number
    //     falling short is the ungated behaviour coming back.
    const int    answeredAtIssueEnd = host.answered();
    const size_t inflight           = waiterCount(plain);
    std::cout << "  burst issued: provider replies=" << answeredAtIssueEnd
              << " waiters registered=" << inflight << "/" << kBurst << std::endl;
    ASSERT_EQ(answeredAtIssueEnd, 0)
        << "the gate leaked: replies were produced while the burst was still "
           "being issued, so what this test measures below is not a burst drain";
    EXPECT_EQ(inflight, size_t(kBurst))
        << "only " << inflight << " of " << kBurst << " calls were in flight when "
           "the burst finished issuing";

    // Drain it, kRelease at a time and NEVER issuing another call — which is the
    // claim. Every entry that leaves m_waiters from here leaves by the exit
    // guard: the spawn-path reaper has already run 800 times against an empty
    // publish list and will not run again.
    for (int done = 0; done < kBurst; done += kRelease) {
        const int upto = std::min(done + kRelease, kBurst);
        host.allow(upto);
        pumpUntilTotal(d, upto, 30000);
        // A step that does not complete is a failure, and carrying on would turn
        // one stuck call into fifty budgets back to back. Break; the assertion
        // below reports it.
        if (d.total.load() < upto) break;
    }
    // Anything the loop left behind (it broke early, or the provider saw calls
    // the client never counted) answers now, so nothing is parked in the host
    // while the assertions run.
    host.openGate();

    pumpUntilTotal(d, kBurst, 60000);
    // A gate that was never opened, a call that errored, a callback that went
    // missing: all of them arrive here as a FAILED assertion on a bounded run,
    // never as a hang. See EchoProvider::await() for why that is true of the
    // gate in particular.
    ASSERT_FALSE(host.gateExpired())
        << "a gated call gave up waiting: the gate was never opened";
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
    // kDrained is 64 and it is a CONSTANT again, which the ungated version could
    // not afford. It is 4 * kRelease rather than a fraction of kBurst because
    // that is what the quantity is: the residue is what published after the last
    // reap, the last reap is inside the last release step, and so the residue is
    // bounded by a step and not by the burst. It does not grow with kBurst and it
    // does not grow with load.
    //
    // MEASURED, correct code, 380 runs, worst value per cell:
    //
    //   macOS  idle 1 (n=100)   4x 1 (n=40)   16x 2 (n=40)
    //   Linux  idle 7 (n=40)    4x 5 (n=60)   16x 6 (n=60)   64x 5 (n=40)
    //
    // The same seven cells with the exit-guard reap removed, 380 more runs, give
    // 800 — kBurst, not "several hundred" — in every single one, on both
    // platforms, at every load level: with no reap on the exit path and no
    // further spawn there is no code left that can remove an entry. All 380 of
    // those runs FAILED this assertion and all 380 correct-code runs passed it.
    //
    // MARGINS: 9.1x above the worst correct-code value ever seen (7, Linux
    // idle), 12.5x below the defect's 800. There is no overlap anywhere to
    // report — the two arms are 114x apart at their closest and the defect's
    // side is not a distribution at all. The load levels do not move the
    // correct-code side either, which is the point of pacing the drain: the
    // worst value on Linux came from the UNLOADED cell.
    EXPECT_LE(idle, kDrained)
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
    // Same claim, same bound — but this one also had a spawn to help it, so it
    // lands at 1-2 in practice. It is held to the same expression on purpose: a
    // residue that the follow-up call did NOT collect is the same retention bug,
    // and hard-coding a tighter number here would put the magic constant back in
    // a quieter place.
    EXPECT_LE(waiterCount(plain), kDrained);

    // release() + pump: see ReleaseOnExit above. It runs on every exit path from
    // here, not just this one.
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
