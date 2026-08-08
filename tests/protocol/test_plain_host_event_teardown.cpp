// Tearing down a live PlainTransportHost while its proxy thread is emitting.
//
// This is a test of the TEST HARNESS. Every LiveHost fixture in this directory
// (test_iofold, test_plain_object_teardown, test_plain_waiter_reaping,
// test_plain_completion_sub_lifetime, test_call_error_after_acquire) puts a
// ModuleProxy on a worker QThread and publishes it through a
// PlainTransportHost, and all of them used to free that host from the TEST
// thread. That is a use-after-free with a wide window, in shared fixture code,
// so it could corrupt any run in this binary — which makes it a candidate for
// the unexplained SIGSEGVs this suite has seen. live_host_teardown.h carries
// the mechanism and the reasoning; this file is the specimen that proves the
// mechanism is needed and that it works.
//
// THE PATH, which is three threads deep and none of them obvious:
//
//   provider emits  ->  ModuleProxy queues the emission to ITS OWN thread
//                       (module_proxy.cpp:38 — it must, or QtRO source
//                       serialization races the reply socket)
//                   ->  worker thread: emit eventResponse
//                   ->  the lambda publishObject() connected with NO context
//                       object, so it is a DIRECT connection and runs right
//                       there on the worker
//                   ->  qvariantListToRpcList(payload)      <-- the window
//                   ->  PlainTransportHost::fanOutEvent -> lock m_mu
//
// Free the host on the test thread and the last step locks a destroyed mutex.
// ~PlainTransportHost does disconnect the connection, which covers an emission
// that has not started; it does nothing for a worker already inside the lambda,
// and the payload conversion in front of the lock is milliseconds wide.
//
// HOW THIS TEST AIMS AT THAT WINDOW rather than sleeping towards it:
//
//   * WIDENED. Each event carries kElems elements, so the conversion between
//     lambda entry and the m_mu lock is real work rather than nanoseconds.
//
//   * AIMED. The round queues a burst and then waits for the FIRST emission to
//     complete before tearing down. Emissions run back to back on the worker,
//     so an observation is the trailing edge of one and the worker is inside
//     the next — i.e. inside the host's lambda — when teardown starts.
//
//   * MEASURED, so it cannot pass vacuously. pendingRounds counts rounds that
//     began teardown with emissions still queued; if a timing change ever makes
//     that zero the test fails instead of silently stopping to test anything.
//
// WHAT IT ASSERTS, and why each is structural on fixed code:
//
//   * afterHostGone == 0. No emission may run on the worker at or after the
//     moment the host is freed. With the free ON the worker (a QMetaCallEvent)
//     the worker is by definition not inside any other slot while it happens.
//
//   * emitted == every event pushed. Qt dispatches equal-priority events FIFO,
//     so a teardown queued behind a burst runs after the whole burst — against
//     a host that is still alive. Nothing is dropped and nothing is late.
//
// NO SUBSCRIBER is connected, and that is deliberate: the fault is the m_mu
// lock at the TOP of fanOutEvent, which precedes the sink lookup, so a consumer
// would add a second connection's teardown to a teardown test without widening
// what is being tested.
//
// PRE-FIX EVIDENCE (this file with logos::testing::destroyHostOnProxyThread
// replaced by a plain m_host.reset(), everything else identical): a hard
// SIGSEGV on the worker thread, on the very first round, with or without a
// memory detector.
//
//   EXC_BAD_ACCESS (SIGSEGV) KERN_INVALID_ADDRESS at 0x71de13fa0, thread "QThread"
//     pthread_mutex_lock
//     std::mutex::lock
//     logos::plain::PlainTransportHost::fanOutEvent   plain_transport_host.cpp:354
//     PlainTransportHost::publishObject(...)::$_0     plain_transport_host.cpp:334
//     doActivate<false>
//     ModuleProxy::eventResponse
//     ModuleProxy::ModuleProxy(...)::$_0              module_proxy.cpp:38
//
// Undetected, the same run reports `mutex lock failed: Invalid argument` out of
// a Qt event handler and aborts. The detector used here is macOS Guard Malloc
// (DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib), which unmaps freed pages so
// the dangling lock faults every time; ASan/TSan are unusable on this toolchain
// (libclang_rt livelocks in its own initializer before main), the same note that
// sits on test_plain_object_teardown.cpp. Fixed, this file is clean under Guard
// Malloc with pendingRounds == kRounds.
//
// THE THIRD ORDER, and why `emitted` is asserted rather than just printed. The
// obvious repair — quit(), wait(), THEN reset() — is memory-clean: wait() joins
// the worker, so an in-flight lambda has finished, and Guard Malloc says nothing
// (afterHostGone 0, twice). It is clean because QThread::quit() reaches
// QEventLoop::exit(), which sets the exit flag synchronously from the CALLING
// thread rather than posting an event, so the worker's loop stops at its next
// iteration and discards every emission still queued behind it. Same specimen,
// same load: 273-383 of 960 delivered. Nothing reports that on its own — which
// is why the count is an assertion here.

#include <gtest/gtest.h>

#include "logos_provider_interface.h"
#include "logos_transport_config.h"
#include "module_proxy.h"

#include "plain_transport_host.h"

#include "live_host_teardown.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QString>
#include <QThread>
#include <QVariant>
#include <QVariantList>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>

using namespace logos::plain;

namespace {

// Bookkeeping shared between the test thread and the proxy's worker thread.
struct Watch {
    // Emissions that reached the worker's eventResponse.
    std::atomic<int>  emitted{0};
    // Set on the worker, immediately after the host is destroyed there.
    std::atomic<bool> hostGone{false};
    // Emissions that ran with the host already gone. Must stay 0.
    std::atomic<int>  afterHostGone{0};
};

// Pushes events on demand through the real route: the EventCallback handed to
// setEventListener is ModuleProxy's, so every push takes the queued path a real
// provider's event takes.
class Pusher : public LogosProviderObject {
public:
    QVariant callMethod(const QString&, const QVariantList& args) override
    {
        return args.value(0, QVariant(1));
    }
    QJsonArray getMethods() override { return QJsonArray{}; }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    void setEventListener(EventCallback cb) override { m_emit = std::move(cb); }
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("pusher"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }

    void push(const QVariantList& data)
    {
        if (m_emit) m_emit(QStringLiteral("teardown_probe"), data);
    }

private:
    EventCallback m_emit;
};

// The fixture shape this whole file is about, reduced to the parts that matter.
class LiveHost {
public:
    explicit LiveHost(Watch& w) : m_watch(w)
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

        m_published = m_host->publishObject("pusher_module", m_proxy);

        // Connected AFTER publishObject, so on each emission this runs on the
        // worker thread immediately after the host's fan-out lambda — i.e. it
        // observes the same emission that would have touched a freed host.
        QObject::connect(m_proxy, &ModuleProxy::eventResponse,
                         [this](const QString&, const QVariantList&) {
                             if (m_watch.hostGone.load(std::memory_order_acquire))
                                 m_watch.afterHostGone.fetch_add(1);
                             m_watch.emitted.fetch_add(1, std::memory_order_release);
                         });

        const QString endpoint = m_host->endpoint();
        m_port = endpoint.mid(endpoint.lastIndexOf(':') + 1).toUShort();
    }

    ~LiveHost()
    {
        // The host dies on the proxy's thread. hostGone is set THERE, right
        // after the free, so the observer above reads it with no race of its
        // own: both run on that one thread.
        QMetaObject::invokeMethod(m_proxy, [this] {
            m_host.reset();
            m_watch.hostGone.store(true, std::memory_order_release);
        }, Qt::BlockingQueuedConnection);

        m_thread->quit();
        m_thread->wait();
        delete m_proxy;
        delete m_thread;
    }

    bool ok() const { return m_started && m_published && m_port != 0; }
    Pusher& provider() { return m_provider; }

private:
    Watch& m_watch;
    Pusher m_provider;
    std::unique_ptr<PlainTransportHost> m_host;
    ModuleProxy* m_proxy = nullptr;
    QThread* m_thread = nullptr;
    bool m_started = false;
    bool m_published = false;
    uint16_t m_port = 0;
};

// The same teardown, expressed through the shared helper the five real fixtures
// call. Same object, same ordering — this one is here so the helper itself is
// exercised rather than only the shape it encodes.
class LiveHostViaHelper {
public:
    LiveHostViaHelper()
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
    }

    ~LiveHostViaHelper()
    {
        logos::testing::destroyHostOnProxyThread(m_host, m_proxy);
        m_thread->quit();
        m_thread->wait();
        delete m_proxy;
        delete m_thread;
    }

    bool ok() const { return m_started && m_published; }
    Pusher& provider() { return m_provider; }

private:
    Pusher m_provider;
    std::unique_ptr<PlainTransportHost> m_host;
    ModuleProxy* m_proxy = nullptr;
    QThread* m_thread = nullptr;
    bool m_started = false;
    bool m_published = false;
};

// Width of the window: the fan-out lambda runs qvariantListToRpcList() over
// this before it touches the host.
constexpr int kElems  = 8000;
// Events per round. Enough that the burst is still draining when teardown
// starts, which is what pendingRounds checks.
constexpr int kBurst  = 24;
constexpr int kRounds = 40;

QVariantList widePayload()
{
    QVariantList l;
    l.reserve(kElems);
    for (int i = 0; i < kElems; ++i)
        l.append(QVariant(QStringLiteral("xxxxxxxxxxxxxxxx")));
    return l;
}

TEST(PlainHostEventTeardownTest, QueuedEmissionsNeverOutliveTheHost)
{
    const QVariantList payload = widePayload();

    int totalEmitted = 0;
    int totalAfter   = 0;
    int pendingRounds = 0;

    for (int r = 0; r < kRounds; ++r) {
        Watch w;
        auto host = std::make_unique<LiveHost>(w);
        ASSERT_TRUE(host->ok()) << "round " << r;

        for (int i = 0; i < kBurst; ++i) host->provider().push(payload);

        // Aim: tear down on the trailing edge of the first emission, while the
        // worker is inside the next one.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (w.emitted.load(std::memory_order_acquire) < 1 &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        ASSERT_GE(w.emitted.load(), 1) << "round " << r << ": nothing emitted";

        if (w.emitted.load(std::memory_order_acquire) < kBurst) ++pendingRounds;
        host.reset();

        totalEmitted += w.emitted.load();
        totalAfter   += w.afterHostGone.load();
    }

    std::cout << "  rounds=" << kRounds << " burst=" << kBurst
              << " emitted=" << totalEmitted
              << " afterHostGone=" << totalAfter
              << " pendingAtTeardown=" << pendingRounds << " rounds\n";

    // The window was really open — otherwise the two assertions below are free.
    EXPECT_GT(pendingRounds, 0)
        << "no round began teardown with emissions still queued; this test "
           "stopped exercising the race it exists for";
    EXPECT_EQ(totalAfter, 0)
        << "an event emission ran on the proxy thread after the host was freed";
    EXPECT_EQ(totalEmitted, kRounds * kBurst)
        << "teardown swallowed queued emissions instead of running behind them";
}

// The helper on the path the fixtures use it on. Nothing to measure here beyond
// "a burst in flight at teardown is survivable"; the measured version is above.
TEST(PlainHostEventTeardownTest, TheSharedHelperTearsDownUnderTheSameLoad)
{
    const QVariantList payload = widePayload();
    for (int r = 0; r < 10; ++r) {
        LiveHostViaHelper host;
        ASSERT_TRUE(host.ok()) << "round " << r;
        for (int i = 0; i < kBurst; ++i) host.provider().push(payload);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    SUCCEED();
}

} // namespace
