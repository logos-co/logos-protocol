// release() racing a call that is still running on another thread.
//
// THE DEFECT, and it is older than the two changes this file arrived with.
// release() used to end in `delete this`. A synchronous callMethod parks its
// caller's thread in a future wait for up to timeoutMs, and when it wakes it
// goes on to touch m_conn, m_objectName and m_state — members of an object
// another thread may have deleted in the meantime. Reproduced deterministically on
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
// WHAT THIS FILE IS: the pin on the FIX, plus the pin on the two shapes the fix
// deliberately does not cover.
//
// IT IS FIXED, and the argument that said it could not be is worth recording
// because it was nearly right. That argument ran: every mechanism that could save
// the racing call is a member, so the racing thread's first act would be to read
// freed storage. True of a call that ENTERS after destruction — and false of a
// call ALREADY INSIDE the object, which is the defect above. That call published
// to the member (took a live reference) on its way in, while the object was
// provably alive, so release() cannot fail to see it. So PlainLogosObject now
// carries a live-reference count: release() drops the OWNER's reference instead
// of deleting, and whoever drops the last one — here, the parked call's own
// thread on its way out — destroys the object. release() still returns
// immediately and still waits for nothing.
//
//   FIXED:      release() concurrent with a call that entered first, sync or
//               async, from any number of threads; and release() re-entered from
//               inside a call or an event callback on the same thread.
//   DIAGNOSED:  a call that ENTERS at or after release() (its first act is to
//               increment a counter that may already be freed), and `delete obj`
//               instead of release() while a call is in flight (destruction NOW,
//               nothing left to defer). Both are reported and abort in debug
//               builds whenever the object still exists to notice.
//
// WHAT THIS FILE PINS, therefore:
//
//   1. THE RACE IS SURVIVED. The reproduction above runs to completion, the
//      parked call returns its own error, and the object is destroyed EXACTLY
//      ONCE — after the call leaves, not before.
//   2. THE TWO REMAINING SHAPES STILL FAIL LOUDLY, as named diagnostics rather
//      than as a SIGSEGV somewhere else.
//   3. A CORRECT PROGRAM IS NEVER ACCUSED. The diagnostic is wired into every
//      public entry point of a class whose teardown is called from inside event
//      callbacks, and one that can fire on a correct program is worse than none.
//      So: every entry point, every early-return path, calls from many threads
//      at once, nested entries, and the shipped
//      release()-from-an-io-thread-event-callback shape — all followed by a
//      release() that must stay silent.
//
// HOW (1) WAS VALIDATED, since a test that passes proves less than one that has
// been seen to fail: the same tests run against the pre-fix tree (cf1b9b0) die —
// SIGSEGV in the parked caller, with and without Guard Malloc. Numbers in the PR.
//
// THE DETERMINISTIC HALF USES A CONNECTION DOUBLE rather than the live host: a
// StalledConnection that accepts a call and never answers it parks the caller in
// its future wait with no timing assumptions at all, and lets the test own the
// object (and count its destructions) instead of receiving it from
// requestObject. The live-host reproduction is kept as well, because a double
// cannot show that the shape occurs in the real stack.

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

// ── the connection double ───────────────────────────────────────────────────
//
// A connection that is open, accepts calls, and answers them in exactly one of
// two ways:
//
//   * STALLED (default): the reply never comes. The synchronous caller parks in
//     `fut.wait_for(timeoutMs)` for the whole timeout and then runs its post-wait
//     epilogue — m_conn->cancelPending(id), then m_objectName to build the error
//     — which is the sequence that used to touch a freed object. No sleeps in the
//     test, no dependence on a provider's scheduling: the park is the double's
//     doing.
//   * DEFERRING: the reply comes back immediately carrying a "multi" pending
//     sentinel, so the caller parks in awaitCompletion() on the state block's
//     condition variable instead. release() NOTIFIES that wait, so the caller
//     wakes inside the window rather than after a timeout — which makes the race
//     tight enough to run hundreds of times in a couple of seconds.
//
// Everything else is a no-op. cancelPending is counted, because "the parked
// caller got as far as its post-wait member access" is what the test is really
// asserting.
class StalledConnection : public RpcConnectionBase {
public:
    void start() override {}
    void stop(const std::string& = "stopped") override {}
    bool isOpen() const override { return true; }

    void setDeferring(bool on) { m_deferring.store(on); }

    std::future<ResultMessage> sendCall(CallMessage msg) override
    {
        auto p = std::make_shared<std::promise<ResultMessage>>();
        {
            std::lock_guard<std::mutex> g(m_mu);
            // Kept alive for the life of the double: destroying the promise would
            // break the future and turn the parked wait into an exception, which
            // is a different test.
            m_promises.push_back(p);
        }
        if (m_deferring.load()) {
            ResultMessage res;
            res.id = msg.id;
            res.ok = true;
            RpcMap pending;
            pending.emplace(logos::pendingCallKey().toStdString(),
                            RpcValue("never-" + std::to_string(msg.id)));
            res.value = RpcValue(std::move(pending));
            p->set_value(std::move(res));
        }
        m_sent.fetch_add(1);
        return p->get_future();
    }

    void sendCallAsync(CallMessage, ResultHandler handler) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_handlers.push_back(std::move(handler));   // never invoked
    }

    std::future<MethodsResultMessage> sendMethods(MethodsMessage) override
    {
        auto p = std::make_shared<std::promise<MethodsResultMessage>>();
        std::lock_guard<std::mutex> g(m_mu);
        m_methodPromises.push_back(p);
        return p->get_future();
    }

    void cancelPending(uint64_t id) override
    {
        m_lastCancelled.store(id);
        m_cancelled.fetch_add(1);
    }

    // A DISTINCT id per registration, not a constant: the handle records what it
    // gets back and hands exactly that to sendUnsubscribe, so a double that
    // returned the same token for every subscribe would let a bug that withdraws
    // the wrong registration pass unnoticed here.
    SubscriptionId sendSubscribe(SubscribeMessage,
                                 std::function<void(EventMessage)>) override
    {
        return m_nextSub.fetch_add(1);
    }
    void sendUnsubscribe(SubscriptionId) override {}
    void sendEvent(EventMessage) override {}
    void sendToken(TokenMessage) override {}
    void setErrorHandler(ErrorHandler) override {}
    uint64_t nextId() override { return m_nextId.fetch_add(1); }

    int sent() const      { return m_sent.load(); }
    int cancelled() const { return m_cancelled.load(); }

private:
    std::mutex m_mu;
    std::vector<std::shared_ptr<std::promise<ResultMessage>>>       m_promises;
    std::vector<std::shared_ptr<std::promise<MethodsResultMessage>>> m_methodPromises;
    std::vector<ResultHandler> m_handlers;
    std::atomic<bool>          m_deferring{false};
    std::atomic<int>           m_sent{0};
    std::atomic<int>           m_cancelled{0};
    std::atomic<uint64_t>      m_lastCancelled{0};
    std::atomic<uint64_t>      m_nextId{1};
    std::atomic<SubscriptionId> m_nextSub{1};
};

// PlainLogosObject that says when it is destroyed. "Exactly once, and not before
// the parked call left" is the whole claim of the fix, and nothing observable
// from outside the class can show it — the destructor is the only witness.
class CountedPlainObject : public PlainLogosObject {
public:
    CountedPlainObject(std::string name,
                       std::shared_ptr<RpcConnectionBase> conn,
                       std::atomic<int>* destructions)
        : PlainLogosObject(std::move(name), std::move(conn))
        , m_destructions(destructions)
    {}

    ~CountedPlainObject() override { m_destructions->fetch_add(1); }

private:
    std::atomic<int>* m_destructions;
};

// Spin until `pred` or the budget runs out. Used only for happens-before edges
// the double publishes (a call has reached the connection), never as a stand-in
// for one.
template <typename Pred>
bool spinUntil(Pred pred, int budgetMs)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(budgetMs);
    while (!pred() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return pred();
}

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

// The reproduction, as a callable, so the test and the description of the bug are
// literally the same code. It runs in a CHILD PROCESS (see the test) for one
// reason: on a tree without the fix it does not fail an assertion, it dies — and
// a death in the middle of protocol_tests takes the other thirty files' results
// with it, while a death in a child is a legible test failure.
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

    // THE RACE. One line, and it used to be the whole bug.
    obj->release();

    // The parked caller must now wake, run its post-wait epilogue against an
    // object that is still there, and return. On a tree without the fix it
    // faults here instead (SIGSEGV in callMethodWithError, one line after the
    // wait) and this process dies without printing the marker below.
    if (!spinUntil([&] { return callReturned.load(); }, 8000)) {
        std::fprintf(stderr, "the parked call never returned\n");
        std::fflush(stderr);
        std::_Exit(8);
    }
    caller.join();
    host.provider().letGo();

    std::fprintf(stderr, "PARKED SYNC CALL SURVIVED THE RELEASE\n");
    std::fflush(stderr);
    std::_Exit(0);
}

// ── the two shapes that are still caller errors ─────────────────────────────
//
// Both use the double, so both are deterministic: the call is parked because the
// connection never answers, not because a provider was slow.

// `delete obj` instead of release(), with a call inside the object. Destruction
// NOW, so there is nothing to defer and nothing the reference count can do.
[[noreturn]] void deleteTheHandleWithACallInFlight()
{
    auto conn = std::make_shared<StalledConnection>();
    std::atomic<int> destroyed{0};
    auto* obj = new CountedPlainObject("racer", conn, &destroyed);

    std::thread caller([obj] {
        logos::CallError err;
        obj->callMethodWithError(kToken, QStringLiteral("block"), {}, 5000, &err);
    });
    if (!spinUntil([&] { return conn->sent() >= 1; }, 5000)) {
        std::fprintf(stderr, "harness: the call never reached the connection\n");
        std::fflush(stderr);
        std::_Exit(9);
    }

    delete obj;                 // <- reported by ~PlainLogosObject, aborts in debug

    caller.detach();
    std::fprintf(stderr, "delete with a call in flight was not reported\n");
    std::fflush(stderr);
    std::_Exit(7);
}

// A call STARTING after release(). Only observable at all because the parked call
// is holding the object alive — which is precisely why it is worth reporting: the
// same program with no parked call is a use-after-free with nothing left to look
// at.
[[noreturn]] void enterAfterRelease()
{
    auto conn = std::make_shared<StalledConnection>();
    std::atomic<int> destroyed{0};
    auto* obj = new CountedPlainObject("racer", conn, &destroyed);

    std::thread caller([obj] {
        logos::CallError err;
        obj->callMethodWithError(kToken, QStringLiteral("block"), {}, 5000, &err);
    });
    if (!spinUntil([&] { return conn->sent() >= 1; }, 5000)) {
        std::fprintf(stderr, "harness: the call never reached the connection\n");
        std::fflush(stderr);
        std::_Exit(9);
    }

    obj->release();             // safe, and returns while the call is still inside
    obj->getMethods();          // <- reported by EntryGuard, aborts in debug

    caller.detach();
    std::fprintf(stderr, "a call entering after release() was not reported\n");
    std::fflush(stderr);
    std::_Exit(7);
}

} // namespace

class SyncCallReleaseRaceTest : public ::testing::Test {
protected:
    void SetUp() override { ensureApp(); }
};

// ── 1. the race is survived, through the real stack ─────────────────────────
//
// The reproduction, run in a child process and required to EXIT ZERO. On the
// pre-fix tree the child dies by signal instead and this reports it as such,
// which is the same code path gtest uses for a death test that does not die.
// "threadsafe" style, so the child is re-executed rather than forked out of a
// process that already runs an io thread, a deadline thread and a Qt worker —
// forking that would inherit locks held by threads that do not exist in the
// child.
TEST_F(SyncCallReleaseRaceTest, AParkedSyncCallSurvivesReleaseFromAnotherThread)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(releaseWhileASyncCallIsParked(),
                ::testing::ExitedWithCode(0),
                "PARKED SYNC CALL SURVIVED THE RELEASE");
}

// ── 2. and the accounting is exact ──────────────────────────────────────────
//
// The same race with the double, so every claim is checkable rather than merely
// survived: release() returns while the call is parked, the object is NOT
// destroyed at that moment, the parked caller completes its post-wait epilogue
// (proved by the cancelPending the double counts), and the object is destroyed
// exactly once, by the caller's thread, on its way out.
TEST_F(SyncCallReleaseRaceTest, ReleaseDefersDestructionToTheLastCallInFlight)
{
    auto conn = std::make_shared<StalledConnection>();
    std::atomic<int> destroyed{0};
    auto* obj = new CountedPlainObject("racer", conn, &destroyed);

    logos::CallError err;
    std::atomic<bool> returned{false};
    std::thread caller([&] {
        // 600ms, and the double never answers, so this thread is parked in
        // `fut.wait_for` for 600ms of wall clock. No provider, no scheduling
        // assumption.
        obj->callMethodWithError(kToken, QStringLiteral("block"), {}, 600, &err);
        returned.store(true);
    });
    ASSERT_TRUE(spinUntil([&] { return conn->sent() >= 1; }, 5000))
        << "the call never reached the connection";

    QElapsedTimer t;
    t.start();
    obj->release();
    const qint64 releaseMs = t.elapsed();

    // Teardown still waits for nothing. If release() had grown a barrier — the
    // "just block until in-flight calls finish" answer — this would be ~600.
    EXPECT_LT(releaseMs, 150) << "release() blocked for " << releaseMs
                              << "ms waiting for the parked call";
    EXPECT_EQ(destroyed.load(), 0)
        << "the object was destroyed while a call was still inside it — which is "
           "the use-after-free this test exists for";
    EXPECT_FALSE(returned.load()) << "the parked call was not parked";

    caller.join();

    std::cout << "  release() returned in " << releaseMs
              << "ms with a call parked; destroyed=" << destroyed.load()
              << " after the call left (code='" << err.code
              << "', cancelPending calls=" << conn->cancelled() << ")"
              << std::endl;

    EXPECT_EQ(destroyed.load(), 1)
        << "the object was destroyed " << destroyed.load()
        << " times; the last caller out must destroy it exactly once";
    EXPECT_EQ(err.code, "timeout")
        << "the parked call did not complete its own error path";
    EXPECT_GE(conn->cancelled(), 1)
        << "the parked caller never reached its post-wait member access — the "
           "very access that used to fault";
}

// The tight version of the same thing, run enough times to sweep the window.
//
// A DIFFERENT PARK: the double answers with a "multi" pending sentinel, so the
// caller parks in awaitCompletion() on the state block's condition variable, and
// release() NOTIFIES that wait — so the caller wakes INSIDE the release rather
// than after a timeout, which is as close as the two threads can be brought
// together. Under Guard Malloc this is the detector for the use-after-free;
// without it, for a double delete or a leaked object (the destruction count is
// checked every round).
TEST_F(SyncCallReleaseRaceTest, TheReleaseWakeRaceIsSafeEveryTime)
{
    constexpr int kRounds = 400;
    std::atomic<int> destroyed{0};
    int woken = 0;

    for (int r = 0; r < kRounds; ++r) {
        auto conn = std::make_shared<StalledConnection>();
        conn->setDeferring(true);
        auto* obj = new CountedPlainObject("racer", conn, &destroyed);

        logos::CallError err;
        std::thread caller([&] {
            obj->callMethodWithError(kToken, QStringLiteral("defer"), {}, 4000, &err);
        });
        ASSERT_TRUE(spinUntil([&] { return conn->sent() >= 1; }, 5000))
            << "round " << r << ": the call never reached the connection";
        // Jittered, so the release lands at a different point of the caller's
        // approach to the condition variable on different rounds.
        if (r % 4) std::this_thread::sleep_for(std::chrono::microseconds((r % 4) * 25));

        obj->release();
        caller.join();

        if (err.code == "transport_error") ++woken;
        ASSERT_EQ(destroyed.load(), r + 1)
            << "round " << r << ": the object was destroyed "
            << destroyed.load() << " times in " << (r + 1) << " rounds";
    }

    std::cout << "  " << kRounds << " release-wake races: destroyed="
              << destroyed.load() << " (one per round), woken by teardown="
              << woken << std::endl;
    EXPECT_EQ(destroyed.load(), kRounds);
    // Not all rounds have to be woken BY the release — some callers reach the
    // predicate after `stopping` is already up — but if none were, the race
    // never happened and this test is decoration.
    EXPECT_GT(woken, 0) << "no round was woken by teardown; the release never "
                           "landed inside the wait";
}

// ── 3. the shapes that remain caller errors ─────────────────────────────────
//
// Death tests, because the diagnostic's whole job is to end the process at the
// offending line. Both were seen to fail before the report existed — with the
// report removed by hand they run to the "was not reported" exit instead, and on
// the pre-fix tree the delete case is a plain SIGSEGV in the parked thread.
TEST_F(SyncCallReleaseRaceTest, DeletingTheHandleWithACallInFlightIsReportedAndFatal)
{
#ifdef NDEBUG
    GTEST_SKIP() << "the abort is debug-only by design: turning a shipped app's "
                    "latent misuse into a hard crash is not a decision a "
                    "bug-fix release makes for its consumers. The report itself "
                    "is emitted in every build.";
#else
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(deleteTheHandleWithACallInFlight(),
                 "call\\(s\\) from other threads are still inside this object");
#endif
}

TEST_F(SyncCallReleaseRaceTest, StartingACallAfterReleaseIsReportedAndFatal)
{
#ifdef NDEBUG
    GTEST_SKIP() << "the abort is debug-only by design; the report itself is "
                    "emitted in every build.";
#else
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(enterAfterRelease(),
                 "was entered on 'racer' AFTER release\\(\\)");
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
// does not wedge; what it has to also not do is trip the detector, because the
// count is read from the io thread while the main thread is nowhere near a call
// on that handle. (It is also the shape the thread-local depth in the detector
// exists to tolerate if a future guarded method ever does invoke user code.)
//
// THE EVENT IS TRIGGERED THROUGH A SECOND HANDLE, for the reason spelled out in
// test_iofold.cpp: firing through the handle that is about to be released means
// the io thread releases it while the main thread is still inside its
// callMethodAsyncWithError. That shape is now SAFE — it is exactly what the
// reference count covers — but it would make this test's outcome depend on
// winning a race it is not about, and it was a genuine use-after-free on every
// tree before this one.
TEST_F(SyncCallReleaseRaceTest, ReleaseFromInsideAnEventCallbackIsNotAccused)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* obj = conn->requestObject(QStringLiteral("racer_module"), 5000);
    ASSERT_NE(obj, nullptr);

    LogosObject* trigger = conn->requestObject(QStringLiteral("racer_module"), 5000);
    ASSERT_NE(trigger, nullptr);
    auto* triggerCh = channelFor(trigger);
    ASSERT_NE(triggerCh, nullptr);

    std::atomic<bool> released{false};
    std::atomic<bool> onIoThread{false};
    const std::thread::id mainThread = std::this_thread::get_id();

    obj->onEvent(QStringLiteral("tick"), [&](const QString&, const QVariantList&) {
        onIoThread.store(std::this_thread::get_id() != mainThread);
        obj->release();
        released.store(true);
    });

    std::atomic<int> fired{0};
    triggerCh->callMethodAsyncWithError(kToken, QStringLiteral("fire"), {}, 5000,
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
    trigger->release();
    pump(50);
}
