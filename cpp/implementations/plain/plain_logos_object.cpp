#include "plain_logos_object.h"

#include "logos_async_dispatch.h"
#include "qvariant_rpc_value.h"

#include <QCoreApplication>
#include <QDebug>
#include <QMetaObject>
#include <QTimer>
#include <QVariantMap>

#include <boost/asio/error.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace logos::plain {

namespace {

// The default the deferred half of a call falls back to when the caller gave a
// non-positive timeout — the value awaitCompletion has always used, kept so the
// async path and the sync path give up at the same moment.
constexpr int kDeferredFallbackMs = 30000;

// The honest code for "the object was released while your call was in flight".
//
// logos_call_error.h's vocabulary is part of the wire contract, so this reuses
// it rather than minting a code. "transport_error" is defined there as "the
// connection failed or was torn down mid-call", which is exactly what happened:
// the consumer tore its own end of the call channel down. Every alternative in
// that set misattributes the failure — "object_unavailable" says the module is
// not there (it is, and it is very likely about to answer; callers re-acquire
// on that code), "call_failed" blames the peer for a dispatch it performed
// perfectly well, and "timeout" — what this used to report, after waiting the
// deadline out — claims a deadline elapsed that did not. It is also already the
// code the wire produces for the same event seen from the other end:
// callErrorFromWire maps TRANSPORT_CLOSED / TRANSPORT_ERROR to transport_error.
logos::CallError callErrorReleased(const std::string& objectName,
                                   const std::string& method)
{
    return logos::callErrorTransport(
        objectName,
        "call to '" + objectName + "." + method + "' was abandoned: the object "
        "was released while the call was in flight");
}

// -----------------------------------------------------------------------------
// DeadlineService — the clock the per-call deadlines hang off. ONE thread for
// the whole process, and deliberately NOT the one the connections run on.
//
// WHY IT IS SEPARATE, which is the single most important decision in this file.
// Folding the per-call waiter thread away means the deadline has to live
// somewhere else, and the obvious somewhere — the connection's own strand, on
// IoContextPool::shared() — makes every deadline in the process hostage to that
// one io thread. It is not a theoretical hostage: this transport delivers user
// onEvent callbacks INLINE on the io thread (rpc_connection.h dispatchIncoming),
// and an event handler that calls another module is ordinary module code. A
// handler making a 2000ms synchronous call while a 200ms deadline is outstanding
// on a COMPLETELY DIFFERENT connection made that deadline fire at 2003ms;
// measured, and the reason this class exists. With the ambient timeouts in this
// stack — 5s in getMethods, 30s in the deferred fallback — a 300ms deadline
// becomes multi-second, and a handler that blocks forever means the deadline
// never fires at all. The whole point of a timeout is that it is the thing that
// still works when everything else is stuck.
//
// The three alternatives, and why not:
//
//   * A SECOND io thread in IoContextPool. Does not fix it — user handlers are
//     unbounded, so N simultaneously-blocked handlers need N+1 threads, and the
//     count is not knowable. It would also quietly break every "serialized by
//     there being one thread" assumption in the transport, which is a far larger
//     blast radius than this class.
//   * MOVING INLINE EVENT DELIVERY OFF THE STRAND (post user callbacks to the
//     Qt loop). Correct direction, much bigger change: it alters event ordering
//     and re-entrancy for every existing consumer of this transport, and it does
//     not help a deadline while the Qt loop itself is blocked.
//   * A Qt TIMER on the Qt event loop. Strictly worse than either: a
//     synchronous callMethod issued from the Qt thread — the most ordinary thing
//     a module does — blocks that loop for the whole call, so the deadline would
//     be hostage to exactly the calls it is supposed to bound.
//
// So: one dedicated thread, process-wide, that does nothing but arm, cancel and
// fire timers. It restores the independence the per-call waiter threads had, at
// one thread instead of one per pending call, which is the entire point of the
// fold. Nothing else may ever be posted here; user code reaches the Qt loop via
// postToQtEventLoop, and AsyncCall::deliver() is a flag, two map erases and a
// post.
// -----------------------------------------------------------------------------
class DeadlineService {
public:
    static DeadlineService& shared()
    {
        // Lazy, like IoContextPool::shared(): a process that never makes an
        // async plain call never starts this thread.
        static DeadlineService svc;
        return svc;
    }

    boost::asio::io_context& context() { return m_ioc; }

    DeadlineService(const DeadlineService&) = delete;
    DeadlineService& operator=(const DeadlineService&) = delete;

private:
    DeadlineService()
        : m_guard(boost::asio::make_work_guard(m_ioc))
        , m_thread([this] { m_ioc.run(); })
    {}

    ~DeadlineService()
    {
        m_guard.reset();
        m_ioc.stop();
        if (m_thread.joinable())
            m_thread.join();
    }

    boost::asio::io_context m_ioc;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> m_guard;
    std::thread m_thread;
};

// The one place the choice above is made. Every per-call deadline in the process
// is armed on this context and nothing else is ever posted to it.
//
// The rejected design is one token different — IoContextPool::shared()
// .ioContext(), the connections' own thread — which is what makes the two tests
// in test_iofold.cpp that measure deadline accuracy under io-thread load worth
// having, and how they were validated. See that file for the numbers.
boost::asio::io_context& deadlineContext()
{
    return DeadlineService::shared().context();
}

// Hand `callback(result)` over to the Qt event loop so PlainLogosObject's
// async path matches LogosObject's interface contract: callbacks are
// always delivered on a subsequent event-loop iteration, on the Qt
// thread, never synchronously and never racing with QObjects/UI code.
//
// Using QCoreApplication::instance() as the anchor means the queued
// invocation lands on whichever thread runs the Qt event loop in this
// process, regardless of which worker thread completed the call.
// If the application has shut down (instance() is null), we drop the
// callback rather than invoke it from an arbitrary thread.
//
// Deliberately a FREE function taking everything BY VALUE, and deliberately not
// a member: the queued lambda runs on a later event-loop iteration, which for a
// call cancelled by teardown is after the PlainLogosObject is already gone.
// Nothing it touches may belong to the object — which is why AsyncCall copies
// objectName/method up front instead of reading m_objectName from inside here.
// Do not give this a `this`.
//
// THE FOLD MADE THIS LOAD-BEARING TWICE OVER. It was already the reason a
// delivery could outlive the handle. It is now also the answer to the
// re-entrancy hazard: every one of the four places that can complete a call —
// the Asio read handler on the connection's strand, fail()'s sweep on an
// arbitrary thread, the deadline handler on the timer thread, and teardown on
// the caller's thread — routes its delivery through here, so NO user callback
// ever runs on an Asio handler stack. That is the class of bug that produced the
// deferred-multi SIGSEGV on the QtRO twin, whose fix (remote_transport.cpp) is
// the same move by a different vehicle: QTimer::singleShot(0).
void postToQtEventLoop(PlainLogosObject::AsyncResultErrorCallback callback,
                       QVariant result, logos::CallError err)
{
    QCoreApplication* app = QCoreApplication::instance();
    if (!app) return;
    QMetaObject::invokeMethod(app,
        [callback = std::move(callback), result = std::move(result),
         err = std::move(err)]() mutable {
            callback(result, err);
        },
        Qt::QueuedConnection);
}

} // anonymous namespace

// -----------------------------------------------------------------------------
// AsyncCall — one in-flight asynchronous call. THIS IS WHAT REPLACED THE THREAD.
//
// The old design gave every pending RPC an OS thread whose only job was to be
// blockable: std::future cannot be waited on with a deadline AND a cancel, so
// the waiter polled it in 25ms slices, then parked on a condition variable for
// the deferred half, then delivered. Three costs came with that — a thread per
// pending call, a 25ms floor on teardown, and a registry-plus-reaping protocol
// to stop finished threads accumulating (a thread cannot join itself).
//
// Here the call is a piece of STATE that three events race to finish:
//
//   * the reply, delivered by RpcConnection as a handler (its strand, or any
//     thread via fail(), or inline when the connection is already stopped);
//   * the deadline, an asio::steady_timer on the DeadlineService's own thread;
//   * cancellation, from teardown on an arbitrary thread.
//
// EXACTLY ONCE is the `claim()` CAS below, and nothing else. That is a real
// change of mechanism and the thing most worth distrusting: the old guarantee
// was structural (one thread, one function, four returns, and a join proving it
// had finished), whereas three independent callers can all arrive here. The CAS
// is what makes the first one win and the other two no-ops, on every path.
//
// LIFETIME is ownership, not a barrier. Each of those three holds a shared_ptr
// to this object; the state it needs on the HANDLE is reached through a weak_ptr
// to CallState, and the connection through a weak_ptr too. Nothing here
// dereferences the PlainLogosObject, so release()'s `delete this` is none of its
// business and teardown has nothing to wait for.
// -----------------------------------------------------------------------------
struct AsyncCall : std::enable_shared_from_this<AsyncCall> {
    using clock = std::chrono::steady_clock;

    AsyncCall(std::weak_ptr<PlainLogosObject::CallState> st,
              std::weak_ptr<RpcConnectionBase> cn,
              std::uint64_t callNumber,
              std::string obj, std::string meth, int timeout,
              PlainLogosObject::AsyncResultErrorCallback cb)
        // A strand of its own over the shared deadline thread. With one thread
        // in that service the strand is redundant today; it is here so that
        // "every touch of this timer is serialized" stays a property of the
        // code rather than of the thread count, because asio timers are
        // "Shared objects: Unsafe" and a second service thread would otherwise
        // turn a re-arm racing its own handler into undefined behaviour.
        : timer(boost::asio::make_strand(deadlineContext()))
        , state(std::move(st))
        , conn(std::move(cn))
        , id(callNumber)
        , objectName(std::move(obj))
        , method(std::move(meth))
        , timeoutMs(timeout)
        , callback(std::move(cb))
    {}

    boost::asio::steady_timer                  timer;
    std::weak_ptr<PlainLogosObject::CallState> state;
    // Only ever used to withdraw this call's registration from the connection's
    // pending map — see cancelPending(). weak, because the connection outlives
    // the handle but not necessarily this call's last handler.
    std::weak_ptr<RpcConnectionBase>           conn;
    const std::uint64_t                        id;
    const std::string                          objectName;
    const std::string                          method;
    const int                                  timeoutMs;

    // Set under CallState::mu when a "multi" provider defers, read under it in
    // deliver() — the one field two threads can reach.
    QString                                    callId;

    std::mutex                                          cbMu;
    PlainLogosObject::AsyncResultErrorCallback          callback;
    std::atomic<bool>                                   delivered{false};

    // ── the exactly-once gate ────────────────────────────────────────────────
    //
    // Three independent callers race for the right to resolve a call — the
    // reply handler, the deadline and teardown — and exactly one may reach the
    // user's callback. Two halves, INDEPENDENTLY SUFFICIENT, which is worth
    // recording because it means neither is redundant: the CAS is the one that
    // also skips the registry erase, the pending withdrawal and the timer
    // cancel, and the swap is what makes the callback itself unrepeatable.
    //
    // This replaced a structural guarantee — one waiter thread, one function
    // body, and a join proving it had finished — so it is the guarantee in this
    // file most worth distrusting, and the two tests that actually detect its
    // absence are named in tests/protocol/CMakeLists.txt. The per-path
    // exactly-once assertions are NOT among them: a call resolved once calls
    // deliver() once whatever guards it.
    bool claim()
    {
        bool expected = false;
        return delivered.compare_exchange_strong(expected, true,
                                                 std::memory_order_acq_rel);
    }

    PlainLogosObject::AsyncResultErrorCallback takeCallback()
    {
        std::lock_guard<std::mutex> g(cbMu);
        PlainLogosObject::AsyncResultErrorCallback cb;
        cb.swap(callback);
        return cb;
    }

    // Idempotent by construction: every later caller returns without touching
    // the callback, the connection or the timer.
    void deliver(QVariant value, logos::CallError err)
    {
        // LEAVE THE HANDLE'S REGISTRIES FIRST — before the exactly-once gate,
        // and unconditionally, which is not where it reads most naturally.
        //
        // A duplicate deliver() has something to clean up. A provider can
        // answer the pending sentinel AFTER this call's deadline has already
        // passed: the timer resolves the call, and only then does the reply
        // handler arrive, see a sentinel, and file this AsyncCall under
        // CallState::deferred. Behind the gate, that entry would never be taken
        // out again — one leaked map entry per slow-sentinel call, for the life
        // of the handle, which is precisely the retention the fold exists to
        // fix. (The reply handler also checks `delivered` before filing, so this
        // only has to cover the instant between that check and the write; the
        // re-armed deadline is what eventually runs this erase.)
        //
        // Erasing twice is free: ids come from RpcConnection::nextId() and are
        // unique per call, so this can never take out somebody else's entry.
        if (auto st = state.lock()) {
            std::lock_guard<std::mutex> g(st->mu);
            st->inflight.erase(id);
            if (!callId.isEmpty()) st->deferred.erase(callId);
        }

        if (!claim()) return;

        PlainLogosObject::AsyncResultErrorCallback cb = takeCallback();

        // AND LEAVE THE CONNECTION'S. m_pendingCalls is erased by exactly two
        // events on its own — a decoded reply with this id, and fail()'s
        // teardown sweep — so a call resolved by its DEADLINE, or by teardown of
        // the handle rather than of the connection, used to leave its
        // registration there for the whole life of the connection, which
        // outlives every handle it hands out. That was true of the promise this
        // replaced too; it is closed here rather than inherited. When the reply
        // IS what got us here, dispatchIncoming has already erased it and this
        // is a lookup that finds nothing.
        if (auto c = conn.lock()) c->cancelPending(id);

        cancelTimer();
        // Last, and never with a lock held: the delivery hop.
        if (cb) postToQtEventLoop(std::move(cb), std::move(value), std::move(err));
    }

    // Callable from ANY thread: the arm is POSTED onto the timer's own strand,
    // so the timer object itself is only ever touched from the deadline thread.
    //
    // `when` is an ABSOLUTE deadline computed by the caller, not a duration, so
    // the post hop cannot stretch it — the timer fires when the caller said it
    // would even if the deadline thread is momentarily busy. `reportMs` is only
    // what the timeout REPORTS; it diverges from the wall time for the deferred
    // half of a non-positive-timeout call, which falls back to 30s the way
    // awaitCompletion always has.
    void armTimer(clock::time_point when, int reportMs)
    {
        auto self = shared_from_this();
        boost::asio::post(timer.get_executor(), [self, when, reportMs] {
            self->timer.expires_at(when);
            self->timer.async_wait([self, reportMs](const boost::system::error_code& ec) {
                if (ec == boost::asio::error::operation_aborted) return;
                self->deliver(QVariant(),
                              logos::callErrorTimeout(self->objectName,
                                                      self->method, reportMs));
            });
        });
    }

    // Also posted, for the same reason. Cancelling matters for retention rather
    // than correctness (the CAS already makes a late timeout a no-op): without
    // it, a call answered in 1ms with a 20s timeout would keep this object alive
    // for the remaining 19.999s.
    void cancelTimer()
    {
        auto self = shared_from_this();
        boost::asio::post(timer.get_executor(), [self] {
            try { self->timer.cancel(); } catch (...) {}
        });
    }
};

PlainLogosObject::PlainLogosObject(std::string objectName,
                                   std::shared_ptr<RpcConnectionBase> conn)
    : m_objectName(std::move(objectName))
    , m_conn(std::move(conn))
{
}

PlainLogosObject::~PlainLogosObject()
{
    disconnectEvents();
    stopAndCancelCalls();
}

void PlainLogosObject::stopAndCancelCalls()
{
    std::vector<std::shared_ptr<AsyncCall>> outstanding;
    {
        // The flag is published under the same mutex awaitCompletion evaluates
        // its predicate under, so the parked synchronous caller cannot read
        // `false`, decide to sleep, and only then miss the notify_all below.
        std::lock_guard<std::mutex> g(m_state->mu);
        m_state->stopping.store(true, std::memory_order_release);
        outstanding.reserve(m_state->inflight.size());
        for (auto& entry : m_state->inflight)
            outstanding.push_back(entry.second);
        m_state->inflight.clear();
        m_state->deferred.clear();
        // Buffered completions nobody can claim any more. They are dropped here
        // rather than left to the state block's own destruction so that a
        // handler still holding a share of it does not keep them alive.
        m_state->completions.clear();
    }
    m_state->cv.notify_all();

    // Cancelled OUTSIDE the lock, because deliver() takes it to erase its own
    // registry entries. (It will find nothing — they were just cleared — which
    // is fine and is why this cannot deadlock either way.)
    //
    // A cancelled call still DELIVERS, exactly once. Returning silently would
    // honour the "stop fast" half and break the half that matters more:
    // callMethodAsyncWithError (and lp_invoke_async above it) promise the
    // callback fires exactly once, so a dropped one turns a bounded stall into
    // an unbounded hang in every caller that awaits it.
    for (auto& call : outstanding)
        call->deliver(QVariant(), callErrorReleased(m_objectName, call->method));

    // And that is the whole of teardown. NOTHING IS WAITED FOR: no thread to
    // join, no io-thread barrier. The reply handler and the deadline handler for
    // a cancelled call may still be queued; each holds its own shared_ptr to the
    // AsyncCall, finds the gate already taken, and drops its share. None of them
    // can reach this object, so it may be deleted the instant this returns.
    //
    // THE BARRIER THAT LOOKS RIGHT AND ISN'T: "post a no-op onto the connection
    // strand and wait for it" would prove no handler is mid-flight, and it
    // wedges the process — IoContextPool runs exactly ONE thread, this transport
    // delivers user event callbacks inline on it, and release()-from-an-event-
    // callback is shipped behaviour (remote_transport.cpp), so the caller can BE
    // the only thread that could drain the barrier. Tried, deadlocked,
    // discarded; ownership is what replaced the join, not a barrier.
    // test_iofold.cpp pins the reentrant case with a watchdog.
}

QVariant PlainLogosObject::callMethod(const QString& authToken,
                                      const QString& methodName,
                                      const QVariantList& args,
                                      int timeoutMs)
{
    // Adapter over the error-carrying implementation: discards the diagnosis,
    // which is exactly what this entry point has always done.
    return callMethodWithError(authToken, methodName, args, timeoutMs, nullptr);
}

QVariant PlainLogosObject::callMethodWithError(const QString& authToken,
                                               const QString& methodName,
                                               const QVariantList& args,
                                               int timeoutMs,
                                               logos::CallError* err)
{
    if (err) err->clear();
    if (!m_conn || !m_conn->isOpen()) {
        if (err)
            *err = logos::callErrorTransport(
                m_objectName, "connection to '" + m_objectName + "' is not open");
        return QVariant();
    }

    // Subscribe to the completion channel BEFORE sending, so a "multi" provider's
    // completion can't race ahead of the waiter (it's buffered either way).
    ensureCompletionSub();

    CallMessage msg;
    msg.id        = m_conn->nextId();
    msg.authToken = authToken.toStdString();
    msg.object    = m_objectName;
    msg.method    = methodName.toStdString();
    msg.args      = qvariantListToRpcList(args);

    const std::uint64_t callNumber = msg.id;
    auto fut = m_conn->sendCall(std::move(msg));

    if (fut.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready) {
        qWarning() << "PlainLogosObject::callMethod: timeout for" << methodName;
        // Withdraw the registration this call left in the connection. The sync
        // path has the same orphan the async one does — nothing erases a
        // pending entry whose reply never comes — and the promise behind it
        // holds a future nobody will ever read again.
        m_conn->cancelPending(callNumber);
        if (err)
            *err = logos::callErrorTimeout(m_objectName, methodName.toStdString(),
                                           timeoutMs);
        return QVariant();
    }
    auto res = fut.get();
    if (!res.ok) {
        qWarning() << "PlainLogosObject::callMethod:" << methodName
                   << "failed:" << QString::fromStdString(res.err);
        // res.errCode / res.err have been on the wire since the plain transport
        // existed; this is the first caller to keep them. MODULE_NOT_LOADED in
        // particular is how "the module isn't there" reaches us on this
        // transport — requestObject never checks publication — so without this
        // the single most common failure was reported as a null result.
        if (err)
            *err = logos::callErrorFromWire(m_objectName, res.errCode, res.err);
        return QVariant();
    }
    const QVariant value = rpcValueToQVariant(res.value);
    // A "multi" provider may have deferred: it returned a pending sentinel and
    // pushes the real result as a completion event. Wait for it, keyed by callId.
    {
        QString callId;
        if (logos::isPendingCallSentinel(value, &callId))
            return awaitCompletion(callId, timeoutMs, methodName, err);
    }
    return value;
}

void PlainLogosObject::ensureCompletionSub()
{
    // Fast path. Every call after the first pays one acquire load and nothing
    // else — and inherits, through it, the ordering the first caller
    // established (see the header).
    if (m_completionSubscribed.load(std::memory_order_acquire)) return;

    // Serializing is the whole fix. A second caller that arrives while the
    // first is still inside subscribeToCompletions() BLOCKS here instead of
    // racing ahead with a Call the provider can answer before the Subscribe
    // frame has been enqueued.
    std::call_once(m_completionSubOnce, [this] {
        subscribeToCompletions();
        // Published last, so the fast path above cannot let a caller through on
        // a subscription that is not yet on the strand.
        m_completionSubscribed.store(true, std::memory_order_release);
    });
}

void PlainLogosObject::subscribeToCompletions()
{
    // Reuse the normal event subscription path (tracked in m_subs, so
    // disconnectEvents() tears it down). The handler fires on the connection's
    // IO thread.
    //
    // It captures a weak_ptr to CallState and nothing else — in particular NOT
    // `this`. That unsubscribe is real (RpcConnection::sendUnsubscribe erases the
    // entry under the connection's mutex) but it is not enough on its own:
    // dispatchIncoming copies the handler out under that mutex and invokes it
    // with the mutex dropped, so an erase racing an already-copied handler
    // changes nothing about the invocation in flight, and nothing joins the io
    // thread. With `this` captured, a completion arriving across a release()
    // wrote into freed memory — see test_plain_completion_sub_lifetime.cpp.
    //
    // weak, not shared, deliberately: locking is what keeps the block alive for
    // the length of one callback, and failing to lock is what makes a handler
    // that outlives its owner — for this reason or any future one — a no-op
    // instead of an append to a map nobody will ever drain.
    std::weak_ptr<CallState> weak = m_state;
    onEvent(logos::callCompleteEvent(), [weak](const QString&, const QVariantList& data) {
        if (data.size() != 2) return;
        const std::shared_ptr<CallState> st = weak.lock();
        if (!st) return;              // the handle and its state are both gone
        const QString callId = data.at(0).toString();

        std::shared_ptr<AsyncCall> call;
        {
            std::lock_guard<std::mutex> g(st->mu);
            auto it = st->deferred.find(callId);
            if (it != st->deferred.end()) {
                call = it->second;
                st->deferred.erase(it);
            } else {
                // Nobody is waiting on it yet: either a SYNCHRONOUS caller is
                // about to park on it, or it arrived before its own sentinel
                // was recorded. Buffer it, exactly as before.
                st->completions[callId] = data.at(1);
            }
        }
        if (call) {
            // Resolves the async call HERE, on the io thread — but deliver()
            // only takes a flag, drops two map entries and posts, so the user's
            // callback still runs on the Qt loop. Called with st->mu released:
            // deliver() takes it.
            call->deliver(data.at(1), logos::CallError{});
            return;
        }
        st->cv.notify_all();
    });
}

QVariant PlainLogosObject::awaitCompletion(const QString& callId, int timeoutMs,
                                           const QString& methodName,
                                           logos::CallError* err)
{
    // A LOCAL SHARE of the state, held for the whole wait. This function only
    // ever runs on the SYNCHRONOUS caller's own thread, so that caller cannot
    // be releasing the handle underneath it — but taking the share costs one
    // atomic increment and removes the question entirely.
    const std::shared_ptr<CallState> st = m_state;
    std::unique_lock<std::mutex> lk(st->mu);
    const auto effectiveMs = timeoutMs > 0 ? timeoutMs : kDeferredFallbackMs;
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(effectiveMs);
    // Interruptible by construction: widen the predicate, and
    // stopAndCancelCalls()' notify_all does the rest. No slicing, so no latency
    // floor at all here — a stop wakes this wait immediately.
    st->cv.wait_until(lk, deadline, [&] {
        return st->completions.count(callId) > 0
            || st->stopping.load(std::memory_order_relaxed);
    });

    // AN ANSWER ALREADY IN HAND BEATS A CONCURRENT STOP: there is a real result
    // here, so hand it over rather than manufacture a failure that did not
    // happen. Callers re-acquire, retry and log on transport_error.
    const auto it = st->completions.find(callId);
    if (it != st->completions.end()) {
        const QVariant result = it->second;
        st->completions.erase(it);
        return result;
    }
    if (st->stopping.load(std::memory_order_relaxed)) {
        qWarning() << "PlainLogosObject: deferred call" << callId
                   << "abandoned — object released while it was in flight";
        if (err)
            *err = callErrorReleased(m_objectName, methodName.toStdString());
        return QVariant();
    }
    qWarning() << "PlainLogosObject: deferred call" << callId << "timed out";
    if (err)
        *err = logos::callErrorTimeout(m_objectName, methodName.toStdString(),
                                       effectiveMs);
    return QVariant();
}

void PlainLogosObject::callMethodAsync(const QString& authToken,
                                       const QString& methodName,
                                       const QVariantList& args,
                                       int timeoutMs,
                                       AsyncResultCallback callback)
{
    // Adapter over the error-carrying implementation: discards the diagnosis,
    // which is exactly what this entry point has always done.
    if (!callback) return;
    callMethodAsyncWithError(authToken, methodName, args, timeoutMs,
        [cb = std::move(callback)](QVariant v, const logos::CallError&) mutable {
            cb(std::move(v));
        });
}

void PlainLogosObject::callMethodAsyncWithError(const QString& authToken,
                                                const QString& methodName,
                                                const QVariantList& args,
                                                int timeoutMs,
                                                AsyncResultErrorCallback callback)
{
    if (!callback) return;
    if (!m_conn || !m_conn->isOpen()) {
        // Defer even the failure path — LogosObject's contract requires
        // callbacks on a subsequent event-loop iteration, never inline.
        postToQtEventLoop(std::move(callback), QVariant(),
                          logos::callErrorTransport(
                              m_objectName,
                              "connection to '" + m_objectName + "' is not open"));
        return;
    }

    ensureCompletionSub();

    CallMessage msg;
    msg.id        = m_conn->nextId();
    msg.authToken = authToken.toStdString();
    msg.object    = m_objectName;
    msg.method    = methodName.toStdString();
    msg.args      = qvariantListToRpcList(args);

    // Copied, not read from the object later: everything below this line may
    // outlive the handle.
    const std::uint64_t callNumber = msg.id;
    const std::string   objectName = m_objectName;
    const std::string   method     = methodName.toStdString();

    // ── the call, as state rather than as a thread ──────────────────────────
    //
    // There used to be a std::thread here whose entire job was to be blockable,
    // and a TODO saying to fold it into the io_context the connection already
    // runs on. This is that fold. Nothing below spawns, joins, sleeps or polls.
    auto call = std::make_shared<AsyncCall>(m_state, m_conn, callNumber,
                                            objectName, method, timeoutMs,
                                            std::move(callback));

    // The deadline is fixed HERE, before the send, and as an absolute point —
    // so neither the post onto the timer thread nor anything the io thread is
    // doing can stretch what the caller asked for.
    call->armTimer(AsyncCall::clock::now() + std::chrono::milliseconds(timeoutMs),
                   timeoutMs);

    bool refused = false;
    {
        std::lock_guard<std::mutex> g(m_state->mu);
        // Same branch, same honesty as before the fold: stopping is raised only
        // by teardown, so a caller that can read it as true here is already
        // calling a method on an object whose destructor is running — this very
        // load is the use-after-free, and nothing in this function can repair
        // that. It is kept because failing this way — one callback, with the
        // error a cancelled call gets — is strictly better than registering a
        // call nobody will ever cancel.
        //
        // What HAS changed is the blast radius if it ever became reachable: the
        // state it would leak an entry into is shared-owned and would simply
        // outlive the handle, instead of being a thread nobody joins.
        if (m_state->stopping.load(std::memory_order_acquire))
            refused = true;
        else
            m_state->inflight.emplace(callNumber, call);
    }
    // OUTSIDE the lock, and that is not a stylistic preference: deliver() takes
    // CallState::mu to leave the registries, and this mutex is not recursive.
    // Delivering from inside the scope above self-deadlocks — on the one branch
    // whose whole purpose is to fail gracefully.
    if (refused) {
        call->deliver(QVariant(), callErrorReleased(objectName, method));
        return;
    }

    // The reply. Handed over by RpcConnection as it arrives, on its strand for
    // the normal path — and on an arbitrary thread from fail(), or inline right
    // here if the connection is already stopped. All three are fine: every exit
    // below funnels into AsyncCall::deliver, whose CAS makes the first one win
    // and which hops to the Qt loop rather than running the user's callback on
    // whatever stack it happens to be on.
    std::weak_ptr<CallState> weakState = m_state;
    m_conn->sendCallAsync(std::move(msg), [call, weakState](ResultMessage res) {
        if (!res.ok) {
            call->deliver(QVariant(),
                          logos::callErrorFromWire(call->objectName, res.errCode,
                                                   res.err));
            return;
        }
        QVariant value = rpcValueToQVariant(res.value);
        QString callId;
        if (!logos::isPendingCallSentinel(value, &callId)) {
            call->deliver(std::move(value), logos::CallError{});
            return;
        }

        // ── the deferred ("multi") half ─────────────────────────────────────
        auto st = weakState.lock();
        if (!st) {
            call->deliver(QVariant(),
                          callErrorReleased(call->objectName, call->method));
            return;
        }

        // Already resolved — by the deadline, or by teardown — while this reply
        // was in flight. Filing it under `deferred` now would register a call
        // that only the re-armed deadline would ever take out again (see
        // deliver()).
        if (call->delivered.load(std::memory_order_acquire)) return;

        QVariant buffered;
        bool haveBuffered = false;
        bool stopping     = false;
        {
            std::lock_guard<std::mutex> g(st->mu);
            stopping = st->stopping.load(std::memory_order_relaxed);
            // The completion can be buffered ALREADY. On one ordered connection
            // it cannot be — the provider writes the sentinel result before the
            // completion event, and both are decoded on the same strand in order
            // — but checking costs one lookup and removes the assumption.
            const auto it = st->completions.find(callId);
            if (it != st->completions.end()) {
                buffered = it->second;
                st->completions.erase(it);
                haveBuffered = true;
            } else if (!stopping) {
                call->callId = callId;      // written under st->mu, read under it
                st->deferred[callId] = call;
            }
        }
        if (haveBuffered) {
            call->deliver(std::move(buffered), logos::CallError{});
            return;
        }
        if (stopping) {
            call->deliver(QVariant(),
                          callErrorReleased(call->objectName, call->method));
            return;
        }
        // A SECOND full deadline, which is what the waiter thread gave it too:
        // it ran the future wait for timeoutMs and then awaitCompletion for
        // another timeoutMs. Preserved deliberately rather than tightened —
        // changing how long a deferred call is allowed to take is a separate
        // decision from removing the thread it used to take it on.
        const int effective =
            call->timeoutMs > 0 ? call->timeoutMs : kDeferredFallbackMs;
        call->armTimer(AsyncCall::clock::now() + std::chrono::milliseconds(effective),
                       effective);
    });
}

bool PlainLogosObject::informModuleToken(const QString& authToken,
                                         const QString& moduleName,
                                         const QString& token,
                                         int /*timeoutMs*/)
{
    if (!m_conn || !m_conn->isOpen()) return false;
    TokenMessage msg;
    msg.authToken  = authToken.toStdString();
    msg.moduleName = moduleName.toStdString();
    msg.token      = token.toStdString();
    m_conn->sendToken(std::move(msg));
    return true; // fire-and-forget
}

void PlainLogosObject::onEvent(const QString& eventName, EventCallback callback)
{
    if (!m_conn || !m_conn->isOpen() || !callback) return;

    {
        std::lock_guard<std::mutex> g(m_mu);
        m_subs.emplace_back(eventName, callback);
    }

    SubscribeMessage msg;
    msg.object    = m_objectName;
    msg.eventName = eventName.toStdString();

    // Bridge RPC event → Qt-flavored callback.
    m_conn->sendSubscribe(std::move(msg), [callback](EventMessage evt) {
        callback(QString::fromStdString(evt.eventName),
                 rpcListToQVariantList(evt.data));
    });
}

void PlainLogosObject::disconnectEvents()
{
    std::vector<std::pair<QString, EventCallback>> subs;
    {
        std::lock_guard<std::mutex> g(m_mu);
        subs.swap(m_subs);
    }
    if (!m_conn) return;
    for (const auto& [name, _] : subs) {
        UnsubscribeMessage msg;
        msg.object    = m_objectName;
        msg.eventName = name.toStdString();
        m_conn->sendUnsubscribe(std::move(msg));
    }
}

void PlainLogosObject::emitEvent(const QString& eventName, const QVariantList& data)
{
    if (!m_conn || !m_conn->isOpen()) return;
    EventMessage msg;
    msg.object    = m_objectName;
    msg.eventName = eventName.toStdString();
    msg.data      = qvariantListToRpcList(data);
    m_conn->sendEvent(std::move(msg));
}

QJsonArray PlainLogosObject::getMethods()
{
    if (!m_conn || !m_conn->isOpen()) return QJsonArray();

    MethodsMessage msg;
    msg.id     = m_conn->nextId();
    msg.object = m_objectName;

    const std::uint64_t callNumber = msg.id;
    auto fut = m_conn->sendMethods(std::move(msg));
    if (fut.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        // Same orphan as the sync call path, on the map next door.
        m_conn->cancelPending(callNumber);
        return QJsonArray();
    }
    auto res = fut.get();
    if (!res.ok) return QJsonArray();
    return methodsToJsonArray(res.methods);
}

void PlainLogosObject::release()
{
    // The RpcConnection is SHARED across every PlainLogosObject a single
    // PlainTransportConnection hands out. Stopping it here would kill
    // the connection for every other holder too, so just unsubscribe our
    // own events and drop our reference — the connection stays alive
    // until PlainTransportConnection itself is destroyed.
    //
    // stopAndCancelCalls() before delete, and it BLOCKS ON NOTHING. In-flight
    // calls used to be threads that captured `this`, so teardown had to ask them
    // to stop and then join them — a wait slice at best, the rest of the call's
    // timeout before that, and a use-after-free if they were merely detached.
    // Now they are shared-owned state that no longer refers to this object at
    // all, so cancelling is a flag, a sweep of the in-flight map, and one
    // callback per abandoned call.
    //
    // That also makes release() safe to call from inside an event callback
    // running on the single io thread — the reentrant shape remote_transport.cpp
    // documents — which a "post a no-op onto the strand and wait for it" barrier
    // could not have been: it would have deadlocked against itself.
    disconnectEvents();
    stopAndCancelCalls();
    m_conn.reset();
    delete this;
}

quintptr PlainLogosObject::id() const
{
    return reinterpret_cast<quintptr>(m_conn.get());
}

} // namespace logos::plain
