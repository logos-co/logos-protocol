#include "plain_logos_object.h"

#include "logos_async_dispatch.h"
#include "qvariant_rpc_value.h"

#include <QCoreApplication>
#include <QDebug>
#include <QMetaObject>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <utility>

namespace logos::plain {

namespace {

// How long a waiter sleeps before it looks at the stop flag again.
//
// A std::future wait cannot be interrupted, so the only way to make one
// abandonable is to wait in slices against the same overall deadline and check
// the flag between them. That slice IS the teardown-latency bound: releasing a
// handle with a call in flight costs at most one of these, instead of whatever
// is left of the call's timeout (20s on the protocol default, logos_mode.h).
//
// 25ms is chosen off both ends of the trade:
//   * latency — a module unloading mid-call should feel instantaneous. 25ms is
//     under two 60Hz frames and well below the ~100ms at which a stall becomes
//     perceptible, so even a shutdown releasing handles back to back stays
//     invisible.
//   * cost — one timed wakeup per slice per IN-FLIGHT call: 40/s, i.e. 800
//     spread over a full 20s default timeout, and paid only while a call is
//     actually outstanding. That is far below the wakeup rate of the Qt event
//     loop these waiters already sit beside.
// Below ~5ms the extra wakeups buy latency nobody can perceive; at 100-250ms
// the teardown hitch starts to show.
constexpr std::chrono::milliseconds kWaitSlice(25);

enum class WaitOutcome { Ready, TimedOut, Cancelled };

// The interruptible form of `fut.wait_for(milliseconds(timeoutMs))`.
//
// The overall deadline is computed once, so slicing does not stretch the
// timeout the caller asked for: the last slice ends exactly on it.
WaitOutcome waitForResult(std::future<ResultMessage>& fut, int timeoutMs,
                          const std::atomic<bool>& stopping)
{
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        // Checked BEFORE sleeping, so a stop that already happened costs
        // nothing, and after every slice, so one that arrives mid-wait costs at
        // most kWaitSlice. A pending stop beats a result that landed in the
        // same tick on purpose: the caller has released the handle and is no
        // longer interested in the answer.
        if (stopping.load(std::memory_order_acquire))
            return WaitOutcome::Cancelled;

        const auto remaining = deadline - clock::now();
        const bool expired = remaining <= clock::duration::zero();
        // At (or past) the deadline, poll once with a zero wait rather than
        // giving up blind — that is what the single unsliced wait_for did for a
        // non-positive timeout, and a future that is already ready must still
        // be reported as ready.
        const auto slice = expired
            ? clock::duration::zero()
            : std::min<clock::duration>(kWaitSlice, remaining);
        if (fut.wait_for(slice) == std::future_status::ready)
            return WaitOutcome::Ready;
        if (expired)
            return WaitOutcome::TimedOut;
    }
}

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

} // anonymous namespace

PlainLogosObject::PlainLogosObject(std::string objectName,
                                   std::shared_ptr<RpcConnectionBase> conn)
    : m_objectName(std::move(objectName))
    , m_conn(std::move(conn))
{
}

PlainLogosObject::~PlainLogosObject()
{
    disconnectEvents();
    stopAndJoinWaiters();
}

void PlainLogosObject::stopWaiters()
{
    {
        // Published under m_completionMu — the mutex awaitCompletion evaluates
        // its predicate under — so a waiter cannot read `false`, decide to
        // sleep, and only then miss the notify_all below. The sliced future
        // wait reads the same flag lock-free, which is why it is an atomic
        // rather than a plain bool guarded by this mutex.
        std::lock_guard<std::mutex> g(m_completionMu);
        m_stopping.store(true, std::memory_order_release);
    }
    m_completionCv.notify_all();
}

void PlainLogosObject::stopAndJoinWaiters()
{
    stopWaiters();

    std::vector<std::thread> waiters;
    {
        std::lock_guard<std::mutex> g(m_waiterMu);
        waiters.swap(m_waiters);
    }
    // Joined with NO lock held: a waiter on its way out still takes
    // m_completionMu (awaitCompletion) and m_waiterMu is what a concurrent
    // callMethodAsyncWithError needs to see the stop flag.
    for (auto& t : waiters) {
        if (t.joinable())
            t.join();
    }
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

    auto fut = m_conn->sendCall(std::move(msg));

    if (fut.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready) {
        qWarning() << "PlainLogosObject::callMethod: timeout for" << methodName;
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
    {
        std::lock_guard<std::mutex> g(m_completionMu);
        if (m_completionSubscribed) return;
        m_completionSubscribed = true;
    }
    // Reuse the normal event subscription path (tracked in m_subs, so
    // disconnectEvents() tears it down). The handler fires on the connection's
    // IO thread; it buffers the result and wakes any waiter.
    onEvent(logos::callCompleteEvent(), [this](const QString&, const QVariantList& data) {
        if (data.size() != 2) return;
        const QString callId = data.at(0).toString();
        {
            std::lock_guard<std::mutex> g(m_completionMu);
            m_completions[callId] = data.at(1);
        }
        m_completionCv.notify_all();
    });
}

QVariant PlainLogosObject::awaitCompletion(const QString& callId, int timeoutMs,
                                           const QString& methodName,
                                           logos::CallError* err)
{
    std::unique_lock<std::mutex> lk(m_completionMu);
    const auto effectiveMs = timeoutMs > 0 ? timeoutMs : 30000;
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(effectiveMs);
    // Unlike the future wait this one is interruptible by construction: widen
    // the predicate, and stopWaiters()' notify_all does the rest. No slicing, so
    // no latency floor at all here — a stop wakes this wait immediately.
    m_completionCv.wait_until(lk, deadline, [&] {
        return m_completions.count(callId) > 0
            || m_stopping.load(std::memory_order_relaxed);
    });

    // A completion that actually landed beats a concurrent stop: there is a real
    // answer in hand, so hand it over rather than manufacture an error.
    const auto it = m_completions.find(callId);
    if (it != m_completions.end()) {
        const QVariant result = it->second;
        m_completions.erase(it);
        return result;
    }
    if (m_stopping.load(std::memory_order_relaxed)) {
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

namespace {

// Hand `callback(result)` over to the Qt event loop so PlainLogosObject's
// async path matches LogosObject's interface contract: callbacks are
// always delivered on a subsequent event-loop iteration, on the Qt
// thread, never synchronously and never racing with QObjects/UI code.
//
// Using QCoreApplication::instance() as the anchor means the queued
// invocation lands on whichever thread runs the Qt event loop in this
// process, regardless of which worker thread completed the future.
// If the application has shut down (instance() is null), we drop the
// callback rather than invoke it from an arbitrary thread.
//
// Deliberately a FREE function taking everything BY VALUE, and deliberately not
// a member: the queued lambda runs on a later event-loop iteration, which for a
// waiter cancelled by teardown is after the PlainLogosObject is already gone.
// Nothing it touches may belong to the object — which is why the waiter copies
// objectName/method up front instead of reading m_objectName from inside here.
// Do not give this a `this`; delivering during teardown would become the
// use-after-free that joining the waiters exists to prevent.
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

    auto fut = std::make_shared<std::future<ResultMessage>>(
        m_conn->sendCall(std::move(msg)));

    // Waiter thread is per-call but the callback hops back to the Qt
    // event loop before running, so it never races with Qt objects. A
    // future iteration can fold this wait into the shared Asio
    // io_context (the connection already runs on it) so we don't spin
    // up a thread per pending RPC.
    //
    // The thread is JOINed in stopAndJoinWaiters() (destructor / release), not
    // detached: capturing `this` for awaitCompletion / m_stopping is
    // only safe while the object is alive, and release() used to
    // `delete this` while a waiter could still be mid-flight.
    const std::string objectName = m_objectName;
    const std::string method = methodName.toStdString();
    // Register under the lock BEFORE the thread can outrun release(): a
    // detach-then-push left a window where delete this raced the waiter.
    {
        std::lock_guard<std::mutex> g(m_waiterMu);
        if (m_stopping.load(std::memory_order_acquire)) {
            // Teardown has already swapped m_waiters out, so a thread pushed
            // now would never be joined — exactly the dangling waiter this
            // whole mechanism exists to prevent. Answer as a cancelled call
            // instead, which keeps the exactly-once contract either way.
            postToQtEventLoop(std::move(callback), QVariant(),
                              callErrorReleased(objectName, method));
            return;
        }
        m_waiters.emplace_back([this, objectName, fut, timeoutMs, methodName, method,
                                callback = std::move(callback)]() mutable {
            // Everything reached through `this` below (m_stopping,
            // awaitCompletion's m_completionMu / m_completions) is safe only
            // because stopAndJoinWaiters() joins this thread before the object
            // dies. Everything handed to postToQtEventLoop is a COPY, because
            // that delivery happens after this thread has returned — i.e.
            // possibly after the object is gone. Keep it that way.
            const WaitOutcome outcome = waitForResult(*fut, timeoutMs, m_stopping);
            if (outcome == WaitOutcome::Cancelled) {
                // A cancelled call still DELIVERS, exactly once. Returning
                // silently here would honour the "stop fast" half and break the
                // half that matters more: callMethodAsyncWithError (and
                // lp_invoke_async above it) promise the callback fires exactly
                // once, so a dropped one turns a bounded stall into an
                // unbounded hang in every caller that awaits it.
                postToQtEventLoop(std::move(callback), QVariant(),
                                  callErrorReleased(objectName, method));
                return;
            }
            if (outcome == WaitOutcome::TimedOut) {
                postToQtEventLoop(std::move(callback), QVariant(),
                                  logos::callErrorTimeout(objectName, method,
                                                          timeoutMs));
                return;
            }
            auto res = fut->get();
            if (!res.ok) {
                postToQtEventLoop(std::move(callback), QVariant(),
                                  logos::callErrorFromWire(objectName, res.errCode,
                                                           res.err));
                return;
            }
            QVariant value = rpcValueToQVariant(res.value);
            // Resolve a "multi" provider's deferred completion (sentinel → wait for
            // the completion event) right here on the waiter thread. This is the
            // second interruptible site: a stop lands it on callErrorReleased,
            // which still falls through to the single post below — one callback,
            // whichever way this went.
            logos::CallError err;
            {
                QString callId;
                if (logos::isPendingCallSentinel(value, &callId))
                    value = awaitCompletion(callId, timeoutMs, methodName, &err);
            }
            postToQtEventLoop(std::move(callback), std::move(value), std::move(err));
        });
    }
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

    auto fut = m_conn->sendMethods(std::move(msg));
    if (fut.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
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
    // stopAndJoinWaiters() before delete: in-flight async waiters capture `this`
    // (for awaitCompletion). Detaching them used to let release() free the
    // object under a still-running waiter — and merely joining them made
    // release() block for the rest of the call's timeout, so they are asked to
    // stop first. Each abandoned call still delivers its callback, once, with
    // callErrorReleased.
    disconnectEvents();
    stopAndJoinWaiters();
    m_conn.reset();
    delete this;
}

quintptr PlainLogosObject::id() const
{
    return reinterpret_cast<quintptr>(m_conn.get());
}

} // namespace logos::plain
