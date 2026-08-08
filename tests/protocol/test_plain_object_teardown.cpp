// Tearing down a PlainLogosObject with a call still in flight.
//
// The branch this sits on stopped callMethodAsync from DETACHING its waiter
// thread: the waiter captures `this` (it reads m_objectName and calls
// awaitCompletion), and release() used to `delete this` underneath it. Waiters
// are now registered in m_waiters and joined before the object dies.
//
// Joining alone only trades one bug for a stall. The join-only joinWaiters()
// had no way to ASK a waiter to stop, so both of the waiter's blocking sites
// ran to their deadline:
//
//   * the std::future wait in callMethodAsyncWithError, and
//   * the completion-event wait in awaitCompletion (a "multi" provider's
//     deferred result).
//
// Destroying a handle with an in-flight call therefore blocked for the
// remainder of that call's timeout — up to 20s on the protocol default
// (logos_mode.h Timeout). A module unloading mid-call stalls the unload for
// that long, on whichever thread called release().
//
// What these tests pin, and why each one is here:
//
//   1. TEARDOWN LATENCY. release() with an in-flight call must return in about
//      one wait slice, not one call timeout.
//
//   2. THE CALLBACK CONTRACT, which is the part a naive fix breaks.
//      callMethodAsyncWithError (and lp_invoke_async above it) promise the
//      callback fires EXACTLY ONCE. A waiter that simply RETURNS when asked to
//      stop silently drops it — trading a bounded stall for an unbounded hang
//      in any caller that awaits that callback. So the three outcomes are
//      counted, not just observed: normal completion, timeout, and
//      cancellation-by-teardown must each deliver exactly one callback. Zero
//      and two are both failures.
//
//   3. THE UAF THAT MUST NOT COME BACK. Cancellation must not become "let the
//      waiter go"; the join still has to happen. The release-during-call race
//      is hammered here so an ASan/TSan build has something to catch.
//
// Everything runs against a live in-process PlainTransportHost over real TCP,
// and drives PlainLogosObject directly (PlainTransportConnection::requestObject)
// so release() is measured on its own rather than through lp_client_destroy.

#include <gtest/gtest.h>

#include "logos_async_dispatch.h"
#include "logos_call_error.h"
#include "logos_object.h"
#include "logos_provider_interface.h"
#include "logos_transport_config.h"
#include "module_proxy.h"

#include "plain_transport_connection.h"
#include "plain_transport_host.h"

#include "live_host_teardown.h"

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
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>

using namespace logos::plain;

namespace {

// A provider with a method that parks until the test lets it go. The existing
// suites use a fixed sleep, which cannot express "in flight for as long as the
// test needs": a sleep that is shorter than the call timeout makes the future
// ready on its own and the teardown measurement then times the sleep instead of
// the timeout.
class BlockingProvider : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList& args) override
    {
        if (method == QLatin1String("ping")) return args.value(0, QVariant(1));
        if (method == QLatin1String("block")) {
            std::unique_lock<std::mutex> lk(m_mu);
            m_cv.wait(lk, [this] { return m_released; });
            return QVariant(42);
        }
        // The other in-flight shape: a "multi" provider that answers the
        // pending sentinel straight away and then never pushes the completion
        // event, so the consumer parks in awaitCompletion instead of on the
        // future. Returns immediately, so unlike `block` it holds no thread.
        if (method == QLatin1String("defer")) {
            QVariantMap sentinel;
            sentinel[logos::pendingCallKey()] = QStringLiteral("never-completes");
            return sentinel;
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
    QString providerName() const override { return QStringLiteral("blocker"); }
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

// A live host publishing `blocker_module` through a ModuleProxy on its own
// thread — the provider blocks, so it must not be the thread the consumer needs
// to deliver its callbacks on.
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

        m_published = m_host->publishObject("blocker_module", m_proxy);

        const QString endpoint = m_host->endpoint();
        m_port = endpoint.mid(endpoint.lastIndexOf(':') + 1).toUShort();
    }

    ~LiveHost()
    {
        // Anything still parked in the provider would deadlock the thread quit
        // below; let every blocked (and queued) call finish first.
        m_provider.letGo();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
        // On the PROXY's thread, not this one — see live_host_teardown.h.
        logos::testing::destroyHostOnProxyThread(m_host, m_proxy);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        m_thread->quit();
        m_thread->wait();
        delete m_proxy;
        delete m_thread;
    }

    bool ok() const { return m_started && m_published && m_port != 0; }
    uint16_t port() const { return m_port; }
    BlockingProvider& provider() { return m_provider; }

private:
    BlockingProvider m_provider;
    std::unique_ptr<PlainTransportHost> m_host;
    ModuleProxy* m_proxy = nullptr;
    QThread* m_thread = nullptr;
    bool m_started = false;
    bool m_published = false;
    uint16_t m_port = 0;
};

// A consumer-side connection to that host. requestObject() hands back a bare
// PlainLogosObject, so release() is exercised directly.
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

// Counts callbacks. `count` is the assertion that matters: the contract is
// EXACTLY ONE, so both 0 and 2 must fail.
struct Sink {
    std::atomic<int> count{0};
    std::mutex mu;
    QVariant value;
    logos::CallError err;

    std::string code()
    {
        std::lock_guard<std::mutex> g(mu);
        return err.code;
    }
    std::string message()
    {
        std::lock_guard<std::mutex> g(mu);
        return err.message;
    }
};

// The callback CO-OWNS its sink. A test that fails its "the callback fired"
// assertion returns with the delivery still queued on the Qt event loop, and a
// sink captured by reference would be a dead stack frame by then — a genuine
// regression would surface as a crash in the harness instead of the clean
// assertion failure that names it.
LogosObjectErrorChannel::AsyncResultErrorCallback cbFor(std::shared_ptr<Sink> sink)
{
    return [sink](QVariant v, const logos::CallError& e) {
        std::lock_guard<std::mutex> g(sink->mu);
        sink->value = std::move(v);
        sink->err = e;
        sink->count.fetch_add(1);
    };
}

void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

bool pumpUntilFired(Sink& s, int budgetMs)
{
    QElapsedTimer t;
    t.start();
    while (s.count.load() == 0 && t.elapsed() < budgetMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return s.count.load() > 0;
}

LogosObjectErrorChannel* channelFor(LogosObject* obj)
{
    return dynamic_cast<LogosObjectErrorChannel*>(obj);
}

const char* kToken = "live-token";

// Long enough that a full-timeout teardown is unmistakable next to a
// one-slice one, short enough that a regression doesn't wedge CI for 20s.
constexpr int kLongTimeoutMs = 8000;

// The budget release() must fit in. One wait slice is 25ms; this leaves an
// order of magnitude of headroom for a loaded CI box while still being ~10x
// below the call timeout above.
constexpr int kTeardownBudgetMs = 750;

} // namespace

class PlainObjectTeardownTest : public ::testing::Test {
protected:
    void SetUp() override { ensureApp(); }
};

// ── 1. teardown latency ─────────────────────────────────────────────────────
//
// The provider parks forever, the call is given 8s, and then the handle is
// released. Pre-fix release() sat inside joinWaiters() until the waiter's own
// future wait hit 8000ms, because nothing could tell it to stop.
TEST_F(PlainObjectTeardownTest, ReleaseWithACallInFlightDoesNotWaitOutTheTimeout)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* obj = conn->requestObject(QStringLiteral("blocker_module"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);

    auto sink = std::make_shared<Sink>();
    ch->callMethodAsyncWithError(kToken, QStringLiteral("block"), {},
                                 kLongTimeoutMs, cbFor(sink));
    // Let the call reach the provider and park there, so the waiter really is
    // mid-wait when release() lands.
    pump(200);
    EXPECT_EQ(sink->count.load(), 0) << "the provider answered; nothing was in flight";

    QElapsedTimer timer;
    timer.start();
    obj->release();
    const qint64 releaseMs = timer.elapsed();

    std::cout << "  release() with an in-flight " << kLongTimeoutMs
              << "ms call took " << releaseMs << "ms" << std::endl;

    EXPECT_LT(releaseMs, kTeardownBudgetMs)
        << "release() waited out the call timeout instead of cancelling the waiter";

    pumpUntilFired(*sink, 2000);
    host.provider().letGo();
    pump(200);
}

// ── 2. the callback contract, all three outcomes ────────────────────────────

TEST_F(PlainObjectTeardownTest, NormalCompletionFiresTheCallbackExactlyOnce)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* obj = conn->requestObject(QStringLiteral("blocker_module"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);

    auto sink = std::make_shared<Sink>();
    ch->callMethodAsyncWithError(kToken, QStringLiteral("ping"),
                                 QVariantList{ QVariant(7) }, 5000, cbFor(sink));
    ASSERT_TRUE(pumpUntilFired(*sink, 10000)) << "the callback never fired";
    // Keep pumping: a second delivery would arrive here.
    pump(300);

    std::cout << "  normal completion    -> callbacks=" << sink->count.load()
              << " code='" << sink->code() << "'" << std::endl;

    EXPECT_EQ(sink->count.load(), 1);
    EXPECT_TRUE(sink->code().empty()) << "a successful call reported an error";
    {
        std::lock_guard<std::mutex> g(sink->mu);
        EXPECT_EQ(sink->value.toInt(), 7);
    }

    obj->release();
}

TEST_F(PlainObjectTeardownTest, TimeoutFiresTheCallbackExactlyOnce)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* obj = conn->requestObject(QStringLiteral("blocker_module"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);

    auto sink = std::make_shared<Sink>();
    ch->callMethodAsyncWithError(kToken, QStringLiteral("block"), {}, 400, cbFor(sink));
    ASSERT_TRUE(pumpUntilFired(*sink, 10000)) << "the callback never fired";
    pump(400);

    std::cout << "  timeout              -> callbacks=" << sink->count.load()
              << " code='" << sink->code() << "'" << std::endl;

    EXPECT_EQ(sink->count.load(), 1);
    EXPECT_EQ(sink->code(), "timeout")
        << "slicing the wait must not change what a real timeout reports";

    obj->release();
    host.provider().letGo();
    pump(200);
}

// The one a "just return on stop" fix breaks: the call is abandoned, and the
// caller must still be told — once — and told the truth.
//
// "transport_error" is the honest code. logos_call_error.h defines it as "the
// connection failed or was torn down mid-call", which is exactly this: the
// consumer tore its own end of the call channel down while the call was in
// flight. The alternatives lie about who failed — "object_unavailable" means
// the module is not there (it is, and is very likely about to answer, and
// callers re-acquire on that code), and "call_failed" blames the peer for a
// dispatch it performed perfectly well. It is also what the wire already
// reports for the same event seen from the other side: callErrorFromWire maps
// TRANSPORT_CLOSED to transport_error.
TEST_F(PlainObjectTeardownTest, CancellationByTeardownFiresTheCallbackExactlyOnce)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* obj = conn->requestObject(QStringLiteral("blocker_module"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);

    auto sink = std::make_shared<Sink>();
    ch->callMethodAsyncWithError(kToken, QStringLiteral("block"), {},
                                 kLongTimeoutMs, cbFor(sink));
    pump(200);
    ASSERT_EQ(sink->count.load(), 0);

    obj->release();

    ASSERT_TRUE(pumpUntilFired(*sink, 2000))
        << "the cancelled call dropped its callback — the caller waits forever";
    pump(400);   // a second delivery would land here

    std::cout << "  cancelled by release -> callbacks=" << sink->count.load()
              << " code='" << sink->code() << "' message='" << sink->message()
              << "'" << std::endl;

    EXPECT_EQ(sink->count.load(), 1);
    EXPECT_EQ(sink->code(), "transport_error");
    EXPECT_FALSE(sink->message().empty());

    host.provider().letGo();
    pump(200);
}

// ── the SECOND blocking site: the deferred-completion wait ──────────────────
//
// A waiter has two places it can be parked, and cancelling only the first would
// be half a fix. Once a "multi" provider answers the pending sentinel, the
// waiter leaves the future wait entirely and blocks in awaitCompletion on
// m_completionCv until the completion event lands or the deadline passes. The
// provider here answers the sentinel and never completes, so release() lands
// while the waiter is in that second wait — not the first.
//
// This one interrupts with no latency floor at all: it is a condition variable,
// so the stop wakes it immediately rather than at the next slice boundary.
TEST_F(PlainObjectTeardownTest, ReleaseDuringADeferredCompletionCancelsThatWaitToo)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* obj = conn->requestObject(QStringLiteral("blocker_module"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);

    auto sink = std::make_shared<Sink>();
    ch->callMethodAsyncWithError(kToken, QStringLiteral("defer"), {},
                                 kLongTimeoutMs, cbFor(sink));
    // The sentinel comes back fast; this is long enough for the waiter to have
    // left the future wait and be sitting in awaitCompletion.
    pump(300);
    ASSERT_EQ(sink->count.load(), 0) << "the deferred call completed on its own";

    QElapsedTimer timer;
    timer.start();
    obj->release();
    const qint64 releaseMs = timer.elapsed();

    ASSERT_TRUE(pumpUntilFired(*sink, 2000)) << "the deferred call dropped its callback";
    pump(300);

    std::cout << "  cancelled mid-defer  -> release=" << releaseMs
              << "ms callbacks=" << sink->count.load()
              << " code='" << sink->code() << "'" << std::endl;

    EXPECT_LT(releaseMs, kTeardownBudgetMs)
        << "release() waited out the deferred-completion deadline";
    EXPECT_EQ(sink->count.load(), 1);
    EXPECT_EQ(sink->code(), "transport_error");
}

// A call started on an already-released... there is no such thing (release
// deletes), but a handle CAN be torn down between the call being issued and the
// waiter starting. Same contract: one callback.
TEST_F(PlainObjectTeardownTest, ReleaseImmediatelyAfterTheCallStillDeliversOnce)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    LogosObject* obj = conn->requestObject(QStringLiteral("blocker_module"), 5000);
    ASSERT_NE(obj, nullptr);
    auto* ch = channelFor(obj);
    ASSERT_NE(ch, nullptr);

    auto sink = std::make_shared<Sink>();
    ch->callMethodAsyncWithError(kToken, QStringLiteral("block"), {},
                                 kLongTimeoutMs, cbFor(sink));
    obj->release();   // no pump: the waiter may not even have started

    ASSERT_TRUE(pumpUntilFired(*sink, 2000)) << "callback dropped";
    pump(300);

    std::cout << "  released instantly   -> callbacks=" << sink->count.load()
              << " code='" << sink->code() << "'" << std::endl;

    EXPECT_EQ(sink->count.load(), 1);

    host.provider().letGo();
    pump(200);
}

// ── 3. the UAF must stay closed ─────────────────────────────────────────────
//
// The waiter must never outlive the object: it reads m_stopping and may call
// awaitCompletion (m_completionMu, m_completions) after the stop, so the join
// is what keeps `this` alive underneath it. Cancelling must not turn into
// detaching.
//
// Hammered with a varying gap between issuing the call and releasing, so the
// release lands at different points of the waiter's startup. Plain, this
// catches a dropped or doubled callback; run under a UAF detector it catches
// the freed `this` directly. Verified with macOS Guard Malloc
// (DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib): clean as written, SIGSEGV
// the moment the join is turned back into a detach. ASan/TSan are not usable
// on this toolchain — libclang_rt livelocks in its own init before main.
TEST_F(PlainObjectTeardownTest, ReleaseRacingTheWaiterIsSafeAndDeliversOnce)
{
    LiveHost host;
    ASSERT_TRUE(host.ok());
    auto conn = connectTo(host.port());
    ASSERT_NE(conn, nullptr);

    constexpr int kRounds = 60;
    int delivered = 0;
    QElapsedTimer total;
    total.start();

    for (int i = 0; i < kRounds; ++i) {
        LogosObject* obj = conn->requestObject(QStringLiteral("blocker_module"), 5000);
        ASSERT_NE(obj, nullptr);
        auto* ch = channelFor(obj);
        ASSERT_NE(ch, nullptr);

        auto sink = std::make_shared<Sink>();
        ch->callMethodAsyncWithError(kToken, QStringLiteral("block"), {},
                                     kLongTimeoutMs, cbFor(sink));
        // 0..~1.5ms of drift across the rounds, sweeping the window between
        // registering the waiter and the waiter reaching its first wait.
        if (i % 3 != 0)
            QThread::usleep(static_cast<unsigned long>((i % 30) * 50));

        obj->release();

        ASSERT_TRUE(pumpUntilFired(*sink, 3000)) << "round " << i << ": callback dropped";
        pump(20);
        ASSERT_EQ(sink->count.load(), 1) << "round " << i << ": callback fired twice";
        ++delivered;
    }

    const qint64 elapsed = total.elapsed();
    std::cout << "  " << delivered << "/" << kRounds
              << " release-during-call rounds delivered exactly once in "
              << elapsed << "ms" << std::endl;
    EXPECT_EQ(delivered, kRounds);
    // 60 rounds x kLongTimeoutMs is 8 minutes if the stop stops working, which
    // would otherwise show up only as a suite that got mysteriously slower.
    // Post-fix a round costs a slice plus a round trip (~40ms), so this is an
    // order of magnitude of headroom.
    EXPECT_LT(elapsed, 30000)
        << "rounds are waiting out call timeouts again, not cancelling";

    host.provider().letGo();
    pump(200);
}
