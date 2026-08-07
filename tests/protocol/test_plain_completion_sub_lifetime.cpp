// The completion subscription outliving the object that owns it.
//
// PlainLogosObject::ensureCompletionSub() registers a handler for the deferred
// ("multi") completion channel. That handler used to capture RAW `this`:
//
//     onEvent(logos::callCompleteEvent(), [this](const QString&, const QVariantList& d) {
//         ... m_completionMu / m_completions / m_completionCv ...
//     });
//
// The handler is stored INSIDE the RpcConnection (rpc_connection.h,
// m_eventCallbacks), which is SHARED by every PlainLogosObject a single
// PlainTransportConnection hands out and outlives all of them — release() says
// so itself, and ends in `delete this`.
//
// RpcConnection::dispatchIncoming() copies the handler out of that map UNDER
// its mutex and then invokes it with the mutex RELEASED:
//
//     { lock(m_mu); ... cb = it->second; }        // copy
//     if (cb) cb(m);                              // invoke, unlocked
//
// So the unsubscribe release() sends — which does erase the map entry, under
// that same mutex — cannot reach a handler that has ALREADY been copied out and
// is mid-flight on the io thread. Between the copy and the handler's first touch
// of `this` sit the EventMessage copy and rpcListToQVariantList(): real work, on
// a real payload. A release() landing in that gap frees the object under a
// handler that is about to write to m_completions.
//
// None of that is caller-side misuse: a "multi" provider pushing a completion is
// ordinary traffic, the io thread is the transport's OWN thread, and release()
// on the consumer thread is the supported way to drop a handle. #41's waiter
// JOIN does not help here — it covers the per-call waiter threads; nothing joins
// or otherwise waits for the io thread.
//
// HOW THIS IS TESTED. The window is a race, so it is widened and then AIMED AT
// rather than slept towards:
//
//   * WIDENED. Each completion event carries a large payload, so the conversion
//     that sits between the handler copy and the handler's touch of `this` takes
//     milliseconds instead of nanoseconds.
//
//   * AIMED. A wildcard subscriber on a SECOND handle observes the connection's
//     dispatch loop: dispatchIncoming copies the named handler and the wildcard
//     handler out together and invokes the named one FIRST, so an observation is
//     the trailing edge of one completion dispatch and the io thread starts the
//     next one immediately after. The round waits for an observation and only
//     then releases, at an offset swept across rounds — so release() lands
//     INSIDE a dispatch instead of before the burst has even been read.
//
//   * MEASURED. The round records the dispatch cadence and the observations that
//     land after release() returned; both are printed and asserted on, so a
//     timing change that stops exercising the window fails the test instead of
//     passing it vacuously.
//
// The detector is macOS Guard Malloc
// (DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib): it unmaps freed pages, so
// the dangling write faults instead of silently corrupting. ASan/TSan are
// unusable on this toolchain (libclang_rt livelocks in its own initializer
// before main) — the same note is on test_plain_object_teardown.cpp.
// NoReleaseIsCleanUnderTheSameStorm runs the identical storm with nothing
// released; it must stay clean under the same detector, which is what makes a
// fault here a lifetime bug rather than an objection to the load.

#include <gtest/gtest.h>

#include "logos_async_dispatch.h"
#include "logos_call_error.h"
#include "logos_object.h"
#include "logos_provider_interface.h"
#include "logos_transport_config.h"
#include "module_proxy.h"

#include "plain_transport_connection.h"
#include "plain_transport_host.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QString>
#include <QThread>
#include <QVariant>
#include <QVariantList>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace logos::plain;

namespace {

uint64_t nowUs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// A provider that exists only to push completion events on demand. The
// EventCallback handed to setEventListener is ModuleProxy's, so emitting through
// it takes exactly the route a real "multi" provider's deferred result takes:
// queued to the proxy's thread -> eventResponse -> PlainTransportHost::fanOutEvent
// -> the subscribed connection -> the consumer's io thread.
class CompletionPusher : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList& args) override
    {
        if (method == QLatin1String("ping")) return args.value(0, QVariant(1));
        return QVariant();
    }

    QJsonArray getMethods() override { return QJsonArray{}; }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    void setEventListener(EventCallback callback) override { m_emit = std::move(callback); }
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("pusher"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }

    // `data` must be a two-element list: PlainLogosObject's completion handler
    // returns before touching the object on anything else, and the touch is the
    // whole point. See completionData() for why the bulk sits where it does.
    void pushCompletion(const QVariantList& data)
    {
        if (!m_emit) return;
        m_emit(logos::callCompleteEvent(), data);
    }

private:
    EventCallback m_emit;
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
        cfg.port = 0;
        m_host = std::make_unique<PlainTransportHost>(cfg);
        m_started = m_host->start();

        m_proxy = new ModuleProxy(&m_provider);
        m_proxy->saveToken(QStringLiteral("origin"), QStringLiteral("live-token"));
        m_thread = new QThread;
        m_proxy->moveToThread(m_thread);
        m_thread->start();

        m_published = m_host->publishObject("pusher_module", m_proxy);

        const QString endpoint = m_host->endpoint();
        m_port = endpoint.mid(endpoint.lastIndexOf(':') + 1).toUShort();
    }

    ~LiveHost()
    {
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
    CompletionPusher& provider() { return m_provider; }

private:
    CompletionPusher m_provider;
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

const char* kToken = "live-token";

// Payload size, in elements, of one completion event. This IS the width of the
// window: rpcListToQVariantList runs on it after dispatchIncoming has copied the
// handler out and dropped its mutex, and before the handler touches the object.
constexpr int kPayloadElems = 8000;

// Completion events pushed per round. Only a couple are needed once the release
// is aimed; the rest keep the io thread busy so the aim has something to hit.
constexpr int kBurst = 8;

QVariantList makePayload()
{
    QVariantList payload;
    payload.reserve(kPayloadElems);
    // Strings, not ints: a QString element allocates on conversion, so the work
    // between the handler copy and the handler's touch of `this` is allocation
    // bound and stays wide under an allocator-based detector.
    for (int i = 0; i < kPayloadElems; ++i)
        payload.append(QVariant(QStringLiteral("payload-element-%1").arg(i)));
    return payload;
}

// A completion event is [id, value] and the handler RETAINS value, in
// m_completions. The bulk therefore goes in the id slot, not the value slot, and
// the id is a list rather than a string (the handler's `.toString()` on it is a
// cheap empty QString, and every event reuses that one key).
//
// That is a concession to the DETECTOR, not a dodge of the bug. What has to be
// straddled is release(), and under Guard Malloc release() spends its time
// destroying whatever m_completions still holds: with the bulk in the value slot
// release costs ~54ms against a ~27ms conversion, so the window is negative and
// nothing can land in it. With the bulk in the id slot release costs ~0.3ms
// against the same ~27ms conversion. Both numbers are Guard Malloc's; without it
// release() is microseconds either way, which is exactly why the shape only
// matters when the detector is on.
QVariantList completionData(const QVariantList& bulk)
{
    return QVariantList{ QVariant(bulk), QVariant(1) };
}

// Wildcard ("" event name) subscriber on a second handle, used as an OBSERVER of
// the connection's dispatch loop. dispatchIncoming copies both the named handler
// and the wildcard handler out under one lock and invokes the named one FIRST,
// so an observation at time T is the trailing edge of a completion dispatch, and
// the io thread begins the next one immediately after T.
struct DispatchObserver {
    std::atomic<int>      total{0};
    std::atomic<int>      straddles{0};
    std::atomic<uint64_t> releasedAtUs{0};   // 0 = release() has not returned yet
    // Longest interval between two observations that still counts as "the io
    // thread went straight from one dispatch into the next" rather than "the io
    // thread went idle between rounds". Set from the measured cadence.
    std::atomic<uint64_t> backToBackUs{0};

    std::mutex            mu;
    std::vector<uint64_t> stamps;
    uint64_t              lastObsUs = 0;     // io thread only

    void observe()
    {
        const uint64_t t = nowUs();
        const uint64_t prev = lastObsUs;
        lastObsUs = t;
        {
            std::lock_guard<std::mutex> g(mu);
            stamps.push_back(t);
        }
        total.fetch_add(1, std::memory_order_relaxed);

        // THE measurement this test turns on: release() returned strictly
        // between two back-to-back dispatches, i.e. while the io thread was
        // inside the dispatch that ended here. That dispatch copied the handler
        // out of the connection's map, converted its payload, and only then
        // touched the object — with release() landing somewhere inside it.
        const uint64_t released = releasedAtUs.load(std::memory_order_acquire);
        const uint64_t window = backToBackUs.load(std::memory_order_relaxed);
        if (released != 0 && prev != 0 && prev < released && t > released
            && (t - prev) <= window) {
            // Count each round once: the round's release time is cleared during
            // the drain that follows.
            uint64_t expected = released;
            if (releasedAtUs.compare_exchange_strong(expected, 0))
                straddles.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void markReleaseDone() { releasedAtUs.store(nowUs(), std::memory_order_release); }
    void clearRelease()    { releasedAtUs.store(0, std::memory_order_release); }

    // Spin until `n` observations have been made, or the budget runs out.
    bool waitFor(int n, int budgetMs)
    {
        const uint64_t deadline = nowUs() + static_cast<uint64_t>(budgetMs) * 1000;
        while (total.load(std::memory_order_relaxed) < n) {
            if (nowUs() > deadline) return false;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        return true;
    }

    // Wait until the io thread has gone quiet — no new dispatch for `quietMs`.
    // Used instead of a fixed drain because one dispatch costs ~2ms plain and
    // ~90ms under Guard Malloc, and a sleep sized for the latter makes the
    // former forty times slower than it needs to be.
    void waitQuiet(int quietMs, int capMs)
    {
        const uint64_t cap = nowUs() + static_cast<uint64_t>(capMs) * 1000;
        uint64_t lastChange = nowUs();
        int seen = total.load(std::memory_order_relaxed);
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const int now = total.load(std::memory_order_relaxed);
            const uint64_t t = nowUs();
            if (now != seen) { seen = now; lastChange = t; }
            else if (t - lastChange >= static_cast<uint64_t>(quietMs) * 1000) return;
            if (t > cap) return;
        }
    }

    // Median interval between consecutive observations, in microseconds: how
    // long one completion dispatch costs the io thread end to end.
    uint64_t medianGapUs()
    {
        std::vector<uint64_t> gaps;
        {
            std::lock_guard<std::mutex> g(mu);
            for (size_t i = 1; i < stamps.size(); ++i)
                gaps.push_back(stamps[i] - stamps[i - 1]);
        }
        if (gaps.empty()) return 0;
        std::sort(gaps.begin(), gaps.end());
        return gaps[gaps.size() / 2];
    }

    void forgetStamps()
    {
        std::lock_guard<std::mutex> g(mu);
        stamps.clear();
    }
};

// Drive one burst through the connection and report how long a single
// completion dispatch costs the io thread. It is measured rather than assumed
// because it moves by a factor of ~40 between a plain run and a Guard Malloc
// one, and the release has to be aimed in units of it.
uint64_t calibrate(DispatchObserver& observer, CompletionPusher& pusher,
                   const QVariantList& data, int burst)
{
    const int before = observer.total.load();
    for (int i = 0; i < burst; ++i)
        pusher.pushCompletion(data);
    observer.waitFor(before + burst, 60000);
    const uint64_t gap = observer.medianGapUs();
    observer.forgetStamps();
    return gap;
}

LogosObjectErrorChannel* channelFor(LogosObject* obj)
{
    return dynamic_cast<LogosObjectErrorChannel*>(obj);
}

} // namespace

class PlainCompletionSubLifetimeTest : public ::testing::Test {
protected:
    void SetUp() override { ensureApp(); }
};

// ── the repro ───────────────────────────────────────────────────────────────
//
// Pre-fix this is a use-after-free: the completion handler the connection is
// running captured `this`, and release() frees it mid-handler. Under Guard
// Malloc that is a fault; without a detector it is a silent write into freed
// memory, which is why the assertions below claim only what they can see — that
// the window was aimed at and hit.
TEST_F(PlainCompletionSubLifetimeTest, CompletionEventDispatchedAcrossReleaseIsSafe)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    // A second handle on the SAME connection, holding the wildcard
    // subscription. Never released inside the loop, so the observer survives
    // every round — and, being a different key, release() does not erase it.
    LogosObject* watcher = conn->requestObject(QStringLiteral("pusher_module"), 5000);
    ASSERT_NE(watcher, nullptr);
    DispatchObserver observer;
    watcher->onEvent(QString(), [&observer](const QString&, const QVariantList&) {
        observer.observe();
    });

    const QVariantList data = completionData(makePayload());

    const uint64_t gapUs = calibrate(observer, host.provider(), data, kBurst);
    ASSERT_GT(gapUs, 0u) << "no completion events reached the connection";
    // "Back to back" means the io thread went straight from one dispatch into
    // the next. Three cadences of slack absorbs jitter without admitting the
    // idle gap between rounds, which is two orders of magnitude larger.
    observer.backToBackUs.store(gapUs * 3);

    // ── aiming ──────────────────────────────────────────────────────────────
    //
    // One dispatch of a completion event has three phases, in this order:
    //
    //     [ read + decode ][ named handler ][ wildcard handler ]
    //                      ^ copy           ^ object touched in here
    //
    // Only the named-handler phase is dangerous, and the only edge this test can
    // see is the END of the wildcard phase. Measured with a temporary probe
    // inside dispatchIncoming, under Guard Malloc: read+decode 62ms, named 59ms,
    // wildcard 59ms, and the touch 27ms into the named phase. So the target is
    // roughly 34-50% of a dispatch past an observation — but the split moves with
    // the allocator, and sizing it live from two calibration bursts proved too
    // noisy to trust (gap1 < gap0 on 2 plain runs in 5).
    //
    // So: a COMB, at 8% of a dispatch, swept across two whole dispatches. Blunt,
    // but it needs no model of the split, and the straddle counter below reports
    // what it actually achieved.
    constexpr int kRounds = 24;

    int roundsAimed = 0;
    for (int round = 0; round < kRounds; ++round) {
        const uint64_t offsetUs = gapUs * static_cast<uint64_t>(round + 1) * 8 / 100;

        // THE ASSERTION THAT FIRES WITHOUT A DETECTOR. A completion handler that
        // reaches a freed PlainLogosObject locks a std::mutex in freed memory;
        // when that memory has been recycled rather than unmapped,
        // pthread_mutex_lock returns EINVAL, std::mutex::lock() throws, and the
        // exception unwinds out of the handler into RpcConnection::doRead()'s
        // catch — which calls fail() and tears the whole connection down. So a
        // dead connection HERE is the use-after-free landing on recycled memory,
        // not a flaky socket. Measured on master: reached within ~5 rounds.
        ASSERT_TRUE(conn->isConnected())
            << "round " << round << ": the connection died mid-run — a completion "
               "handler threw out of the io thread, which is what a released "
               "object's mutex does when the memory has been reused";

        LogosObject* obj = conn->requestObject(QStringLiteral("pusher_module"), 5000);
        ASSERT_NE(obj, nullptr);
        auto* ch = channelFor(obj);
        ASSERT_NE(ch, nullptr);

        // A plain sync call, only so ensureCompletionSub() registers the
        // completion handler. Its reply proves the Subscribe frame that preceded
        // it has already been processed by the host.
        logos::CallError err;
        ch->callMethodWithError(kToken, QStringLiteral("ping"),
                                QVariantList{ QVariant(1) }, 30000, &err);
        ASSERT_TRUE(err.code.empty()) << "round " << round << ": " << err.message;

        const int before = observer.total.load();
        for (int i = 0; i < kBurst; ++i)
            host.provider().pushCompletion(data);

        // Wait until the io thread is demonstrably inside the burst, then aim.
        const bool aimed = observer.waitFor(before + 1, 60000);
        if (aimed) ++roundsAimed;
        std::this_thread::sleep_for(std::chrono::microseconds(offsetUs));

        obj->release();
        observer.markReleaseDone();

        // Let the io thread finish draining the burst with the object gone.
        observer.waitQuiet(static_cast<int>(std::max<uint64_t>(50, gapUs * 3 / 1000)),
                           20000);
        observer.clearRelease();
    }

    std::cout << "  dispatch=" << gapUs << "us"
              << "  dispatches observed=" << observer.total.load()
              << "  rounds aimed=" << roundsAimed << "/" << kRounds
              << "  releases that landed INSIDE a dispatch="
              << observer.straddles.load() << "/" << kRounds << std::endl;

    // The harness assertions. Without them a timing change could make this file
    // green while never putting a handler in flight across the free.
    EXPECT_EQ(roundsAimed, kRounds)
        << "the burst never reached the io thread — release() was not aimed at "
           "anything, so this run is vacuous rather than green";
    EXPECT_GT(observer.straddles.load(), 0)
        << "no release() landed between two back-to-back dispatches: the object "
           "was never freed while the connection was inside a handler, so this "
           "run does not exercise the window";
    EXPECT_TRUE(conn->isConnected())
        << "the connection died on the last round — see the per-round assertion";

    watcher->release();
}

// ── the control ─────────────────────────────────────────────────────────────
//
// Same storm, same payload, same connection — but nothing is released, so no
// handler can be holding a freed pointer. This is what makes the detector
// meaningful: Guard Malloc must be CLEAN here on the very code where the test
// above faults, otherwise the fault is an artefact of the load rather than of
// the lifetime bug.
TEST_F(PlainCompletionSubLifetimeTest, NoReleaseIsCleanUnderTheSameStorm)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* watcher = conn->requestObject(QStringLiteral("pusher_module"), 5000);
    ASSERT_NE(watcher, nullptr);
    DispatchObserver observer;
    watcher->onEvent(QString(), [&observer](const QString&, const QVariantList&) {
        observer.observe();
    });

    LogosObject* obj = conn->requestObject(QStringLiteral("pusher_module"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);

    logos::CallError err;
    ch->callMethodWithError(kToken, QStringLiteral("ping"), QVariantList{ QVariant(1) },
                            20000, &err);
    ASSERT_TRUE(err.code.empty()) << err.message;

    const QVariantList data = completionData(makePayload());
    const uint64_t gapUs = calibrate(observer, host.provider(), data, kBurst);
    ASSERT_GT(gapUs, 0u) << "no completion events reached the connection";

    // Same payload, same burst, same aiming arithmetic as the repro — fewer
    // rounds only because nothing here has to be hit, just survived.
    for (int round = 0; round < 8; ++round) {
        const int before = observer.total.load();
        for (int i = 0; i < kBurst; ++i)
            host.provider().pushCompletion(data);
        observer.waitFor(before + 1, 60000);
        std::this_thread::sleep_for(
            std::chrono::microseconds(gapUs * static_cast<uint64_t>(round + 1) * 8 / 100));
        observer.waitQuiet(static_cast<int>(std::max<uint64_t>(50, gapUs * 3 / 1000)),
                           20000);
    }

    std::cout << "  control: dispatch=" << gapUs << "us"
              << "  dispatches observed=" << observer.total.load() << std::endl;
    EXPECT_GT(observer.total.load(), 0) << "no events reached the connection at all";

    // Still usable after the storm: the completion channel is still wired to a
    // live object rather than to a handler that quietly stopped working.
    logos::CallError err2;
    const QVariant v = ch->callMethodWithError(kToken, QStringLiteral("ping"),
                                               QVariantList{ QVariant(9) }, 20000, &err2);
    EXPECT_TRUE(err2.code.empty()) << err2.message;
    EXPECT_EQ(v.toInt(), 9);

    obj->release();
    watcher->release();
}
