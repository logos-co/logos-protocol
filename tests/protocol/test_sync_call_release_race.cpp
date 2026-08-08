// release() racing a call that is still running on another thread.
//
// THE DEFECT, and it is older than the two changes this file arrived with.
// release() ends in `delete this`. A synchronous callMethod parks its caller's
// thread in a future wait for up to timeoutMs, and when it wakes it goes on to
// touch m_conn, m_objectName and m_state — members of an object another thread
// may have deleted in the meantime. Reproduced deterministically on
// feat/plain-async-io-fold (cf1b9b0), on fix/plain-completion-sub-lifetime and
// on master, with AND without Guard Malloc:
//
//   thread #7, stop reason = EXC_BAD_ACCESS (code=1, address=0x0)
//   frame #0: PlainLogosObject::callMethodWithError(...) + 1700
//     ->  ldr x8, [x0]              ; loading m_conn's vptr, one line after the
//                                   ; future wait timed out
//
// exit 139, three runs out of three, under and without libgmalloc. WHICH member
// gets touched first differs by tree — cf1b9b0 dereferences m_conn to withdraw
// the call's pending registration, master reads m_objectName to build the
// timeout error — so the reproduction below goes through the error channel,
// which has a post-wait member access on every one of them.
//
// WHAT THIS FILE IS, which is not "the fix". The race cannot be closed from
// inside the object: every mechanism that could — a refcount, a flag, a lock —
// is a member, so the racing thread's first act would be to read it out of
// storage that has just been freed, and there is no synchronising with a
// destruction you can only learn about by reading the destroyed object. The
// designs that do work keep the state outside the object and the ABI forbids
// them (requestObject hands out a raw LogosObject*; release() is defined as
// deleting it). The long note over PlainLogosObject::release() lists the three
// alternatives that were considered and why each was rejected — including the
// one that genuinely works and costs unbounded retention.
//
// So the object documents a CONTRACT (calls must have returned before release()
// is entered) and carries a DETECTOR that makes breaking it loud: entries into
// the public methods are counted, and release() reports — and in a debug build
// aborts — when the count is not zero. This file pins BOTH halves, and the
// second is the one that matters more:
//
//   1. THE MISUSE IS CAUGHT. The reproduction above, run under the detector,
//      must die with the named diagnostic instead of a SIGSEGV somewhere else.
//   2. A CORRECT PROGRAM IS NEVER ACCUSED. A detector that can fire on a
//      correct program is worse than no detector, and this one is wired into
//      every public entry point of a class whose teardown is called from inside
//      event callbacks. So: every entry point, every early-return path, calls
//      from many threads at once, nested entries, and the shipped
//      release()-from-an-io-thread-event-callback shape — all followed by a
//      release() that must stay silent.
//
// HOW (1) WAS VALIDATED, since a death test that passes proves less than one
// that has been seen to fail: on the pre-fix tree the same test reports
// "died but not with the expected error" — release() returns normally there,
// the racing thread then faults, and the diagnostic it is matching for does not
// exist. Numbers in the PR.

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

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QString>
#include <QThread>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace logos::plain;

namespace {

// ping   — answers immediately
// block  — parks until letGo(), and reports when the call actually arrived, so
//          the race can be built from a happens-before edge rather than a sleep
// defer  — "multi": pending sentinel, completed from a worker
// sink   — "multi": pending sentinel, never completed
class RaceProvider : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList& args) override
    {
        if (method == QLatin1String("ping")) return args.value(0, QVariant(1));
        if (method == QLatin1String("block")) {
            m_blockedCalls.fetch_add(1);
            std::unique_lock<std::mutex> lk(m_mu);
            m_cv.wait(lk, [this] { return m_released; });
            return QVariant(42);
        }
        if (method == QLatin1String("sink")) {
            QVariantMap pending;
            pending[logos::pendingCallKey()] = QStringLiteral("never-%1").arg(
                static_cast<qulonglong>(m_counter.fetch_add(1)));
            return pending;
        }
        if (method == QLatin1String("defer")) {
            const QString callId = QStringLiteral("cid-%1").arg(
                static_cast<qulonglong>(m_counter.fetch_add(1)));
            auto cb = m_eventCb;
            std::thread([cb, callId] {
                if (cb) cb(logos::callCompleteEvent(),
                           QVariantList{ callId, QVariant(7) });
            }).detach();
            QVariantMap pending;
            pending[logos::pendingCallKey()] = callId;
            return pending;
        }
        if (method == QLatin1String("fire")) {
            if (m_eventCb) m_eventCb(QStringLiteral("tick"), QVariantList{ QVariant(1) });
            return QVariant(true);
        }
        return QVariant();
    }

    int blockedCalls() const { return m_blockedCalls.load(); }

    void letGo()
    {
        { std::lock_guard<std::mutex> g(m_mu); m_released = true; }
        m_cv.notify_all();
    }

    QJsonArray getMethods() override { return QJsonArray{}; }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    void setEventListener(EventCallback cb) override { m_eventCb = std::move(cb); }
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("racer"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }

private:
    std::mutex                 m_mu;
    std::condition_variable    m_cv;
    bool                       m_released = false;
    EventCallback              m_eventCb;
    std::atomic<int>           m_blockedCalls{0};
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
        cfg.port = 0;
        m_host = std::make_unique<PlainTransportHost>(cfg);
        m_started = m_host->start();

        m_proxy = new ModuleProxy(&m_provider);
        m_proxy->saveToken(QStringLiteral("origin"), QStringLiteral("live-token"));
        m_thread = new QThread;
        m_proxy->moveToThread(m_thread);
        m_thread->start();

        m_published = m_host->publishObject("racer_module", m_proxy);
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
    RaceProvider& provider() { return m_provider; }

private:
    RaceProvider m_provider;
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

void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

const char* kToken = "live-token";

// The reproduction, as a callable, so the death test and the description of the
// bug are literally the same code.
//
// The window is opened by a happens-before edge and not by a sleep: the
// provider counts the calls that have reached `block`, so when that count moves
// the consumer's thread is provably inside callMethodWithError, parked in its
// future wait. Then — and only then — the handle is released.
[[noreturn]] void releaseWhileASyncCallIsParked()
{
    ensureApp();
    LiveHost host;
    if (!host.ok()) {
        std::fprintf(stderr, "harness failed to start\n");
        std::fflush(stderr);
        std::_Exit(9);
    }
    auto conn = connectTo(host.port());
    if (!conn) {
        std::fprintf(stderr, "harness failed to connect\n");
        std::fflush(stderr);
        std::_Exit(9);
    }
    LogosObject* obj = conn->requestObject(QStringLiteral("racer_module"), 5000);
    auto* ch = obj ? channelFor(obj) : nullptr;
    if (!ch) {
        // Distinguished from the failure this test is looking for: a broken
        // harness must not read as "the diagnostic never happened".
        std::fprintf(stderr, "harness failed to acquire the object\n");
        std::fflush(stderr);
        std::_Exit(9);
    }

    std::atomic<bool> callReturned{false};
    std::thread caller([ch, &callReturned] {
        // Through the ERROR CHANNEL, with a real CallError*, because that is
        // what every caller above this uses now (lp_invoke, the generated
        // wrappers) — and because it is the shape whose post-wait member access
        // exists on every tree: cf1b9b0 dereferences m_conn unconditionally to
        // withdraw the pending registration, master reads m_objectName to build
        // the timeout error. The bare callMethod() overload passes err=nullptr
        // and on master touches no member after the wait at all, which is how a
        // first cut of this test came back clean there — silent UB, not absence.
        //
        // 300ms, so that on a tree with no detector the wait DOES elapse and the
        // thread goes on to dereference the freed handle while this process is
        // still alive to notice.
        logos::CallError err;
        ch->callMethodWithError(kToken, QStringLiteral("block"), {}, 300, &err);
        callReturned.store(true);
    });

    QElapsedTimer t;
    t.start();
    while (host.provider().blockedCalls() == 0 && t.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    if (host.provider().blockedCalls() == 0) {
        std::fprintf(stderr, "the call never reached the provider\n");
        std::fflush(stderr);
        std::_Exit(9);
    }

    // THE MISUSE. One line, and it is the whole bug.
    obj->release();

    // Only reached on a tree without the detector. Wait out the call's deadline
    // so the racing thread wakes into freed memory, then leave with a code that
    // is unambiguously "the diagnostic never happened".
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    caller.detach();
    std::fprintf(stderr, "release() returned and nothing was reported\n");
    std::fflush(stderr);
    std::_Exit(7);
}

} // namespace

class SyncCallReleaseRaceTest : public ::testing::Test {
protected:
    void SetUp() override { ensureApp(); }
};

// ── 1. the misuse is caught ─────────────────────────────────────────────────
//
// A death test because the detector's whole job is to end the process at the
// offending line. "threadsafe" style, so the child is re-executed rather than
// forked out of a process that already runs an io thread, a deadline thread and
// a Qt worker — forking that would inherit locks held by threads that do not
// exist in the child.
TEST_F(SyncCallReleaseRaceTest, ReleaseWithASyncCallInFlightIsReportedAndFatal)
{
#ifdef NDEBUG
    GTEST_SKIP() << "the abort is debug-only by design: turning a shipped app's "
                    "latent misuse into a hard crash is not a decision a "
                    "bug-fix release makes for its consumers. The report itself "
                    "is emitted in every build.";
#else
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(releaseWhileASyncCallIsParked(),
                 "call\\(s\\) from other threads are still inside this object");
#endif
}

// ── 2. no false positives ───────────────────────────────────────────────────
//
// The half that decides whether the detector is shippable. Everything a correct
// caller does, in one handle's life, followed by the release that must stay
// silent. If any entry point leaks its count — an early return, a nested entry,
// an exception path — this aborts, which is exactly how it should fail.
TEST_F(SyncCallReleaseRaceTest, EveryEntryPointLeavesTheCountAtZero)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* obj = conn->requestObject(QStringLiteral("racer_module"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);

    // The happy paths.
    EXPECT_EQ(obj->callMethod(kToken, QStringLiteral("ping"),
                              QVariantList{ QVariant(5) }, 5000).toInt(), 5);
    logos::CallError err;
    ch->callMethodWithError(kToken, QStringLiteral("ping"),
                            QVariantList{ QVariant(6) }, 5000, &err);
    EXPECT_TRUE(err.ok());

    // A WIRE ERROR: an object nobody published, which the host answers with
    // MODULE_NOT_LOADED. (An unknown METHOD is not usable here — every provider
    // answers that with a bare null, which is indistinguishable from success.)
    LogosObject* missing = conn->requestObject(QStringLiteral("no_such_module"), 5000);
    ASSERT_NE(missing, nullptr);
    auto* missingCh = channelFor(missing);
    ASSERT_NE(missingCh, nullptr);
    missingCh->callMethodWithError(kToken, QStringLiteral("ping"), {}, 2000, &err);
    EXPECT_FALSE(err.ok());
    missing->release();          // and this must be silent too

    const QVariant deferred =
        ch->callMethodWithError(kToken, QStringLiteral("defer"), {}, 5000, &err);
    EXPECT_TRUE(err.ok());
    EXPECT_EQ(deferred.toInt(), 7);

    // A deferred call that gives up: awaitCompletion's timeout return.
    ch->callMethodWithError(kToken, QStringLiteral("sink"), {}, 200, &err);
    EXPECT_EQ(err.code, "timeout");

    // The async front doors, including the one that returns before doing
    // anything (a null callback) and the one that fails at the front door.
    obj->callMethodAsync(kToken, QStringLiteral("ping"), {}, 5000, nullptr);
    std::atomic<int> asyncDone{0};
    ch->callMethodAsyncWithError(kToken, QStringLiteral("ping"),
                                 QVariantList{ QVariant(1) }, 5000,
                                 [&asyncDone](QVariant, const logos::CallError&) {
                                     asyncDone.fetch_add(1);
                                 });

    // And the rest of the surface.
    EXPECT_TRUE(obj->informModuleToken(kToken, QStringLiteral("racer_module"),
                                       QStringLiteral("t"), 1000));
    obj->onEvent(QStringLiteral("tick"), [](const QString&, const QVariantList&) {});
    obj->emitEvent(QStringLiteral("noise"), QVariantList{ QVariant(1) });
    obj->getMethods();
    obj->disconnectEvents();
    (void)obj->id();

    QElapsedTimer t;
    t.start();
    while (asyncDone.load() == 0 && t.elapsed() < 8000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    EXPECT_EQ(asyncDone.load(), 1);

    // LAST, and that ordering is load-bearing: ModuleProxy dispatches this
    // provider on ONE thread, so a call parked in `block` blocks every call
    // behind it. This is the TIMEOUT early return — the reason the whole file
    // exists — and it has to be the final call on the handle.
    ch->callMethodWithError(kToken, QStringLiteral("block"), {}, 150, &err);
    EXPECT_EQ(err.code, "timeout");

    std::cout << "  every entry point exercised, including 4 early-return paths"
              << std::endl;

    // The assertion IS this line not aborting.
    obj->release();
    host.provider().letGo();
    pump(200);
}

// Calls from MANY threads at once, all joined, then released. The count is
// shared across threads, so an off-by-one on any path shows up here as an abort
// even when the single-threaded walk above is clean.
TEST_F(SyncCallReleaseRaceTest, ConcurrentCallersThatHaveReturnedAreNotAccused)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* obj = conn->requestObject(QStringLiteral("racer_module"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);

    constexpr int kThreads = 6;
    constexpr int kPerThread = 40;
    std::atomic<int> ok{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([ch, obj, &ok] {
            for (int i = 0; i < kPerThread; ++i) {
                logos::CallError err;
                ch->callMethodWithError(kToken, QStringLiteral("ping"),
                                        QVariantList{ QVariant(i) }, 5000, &err);
                if (err.ok()) ok.fetch_add(1);
                obj->emitEvent(QStringLiteral("noise"), {});
            }
        });
    }
    // The Qt loop has to keep turning: ModuleProxy dispatches on its own thread
    // but the host's reply path posts through this one. BOUNDED, because an
    // unbounded wait in a test does not fail — it hangs the CI job until the
    // job timeout, and a hang reports nothing about what broke.
    {
        QElapsedTimer t;
        t.start();
        while (ok.load() < kThreads * kPerThread && t.elapsed() < 60000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    for (auto& th : threads) th.join();

    std::cout << "  " << kThreads << " threads x " << kPerThread
              << " synchronous calls, all joined -> " << ok.load() << " ok"
              << std::endl;
    EXPECT_EQ(ok.load(), kThreads * kPerThread);

    // Released from THIS thread, which made none of those calls. Must be silent.
    obj->release();
    pump(100);
}

// The shipped reentrant shape: an event callback, running on the io thread,
// releasing the handle it was delivered through. test_iofold.cpp pins that this
// does not wedge; what it has to also not do now is trip the detector, because
// the count is read from the io thread while the main thread is nowhere near a
// call. (It is also the shape the thread-local depth in the detector exists to
// tolerate if a future guarded method ever does invoke user code.)
TEST_F(SyncCallReleaseRaceTest, ReleaseFromInsideAnEventCallbackIsNotAccused)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* obj = conn->requestObject(QStringLiteral("racer_module"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);

    std::atomic<bool> released{false};
    std::atomic<bool> onIoThread{false};
    const std::thread::id mainThread = std::this_thread::get_id();

    obj->onEvent(QStringLiteral("tick"), [&](const QString&, const QVariantList&) {
        onIoThread.store(std::this_thread::get_id() != mainThread);
        obj->release();
        released.store(true);
    });

    std::atomic<int> fired{0};
    ch->callMethodAsyncWithError(kToken, QStringLiteral("fire"), {}, 5000,
                                 [&fired](QVariant, const logos::CallError&) {
                                     fired.fetch_add(1);
                                 });

    QElapsedTimer t;
    t.start();
    while (!released.load() && t.elapsed() < 8000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);

    std::cout << "  reentrant release from an event callback (io thread: "
              << onIoThread.load() << ") was not reported" << std::endl;

    EXPECT_TRUE(released.load()) << "the reentrant release never happened";
    EXPECT_TRUE(onIoThread.load())
        << "the event did not arrive on the io thread — this test is not "
           "exercising the reentrancy it claims to";
    host.provider().letGo();
    pump(200);
}
