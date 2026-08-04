#include "plain_logos_object.h"

#include "logos_async_dispatch.h"
#include "qvariant_rpc_value.h"

#include <QCoreApplication>
#include <QDebug>
#include <QMetaObject>
#include <QTimer>
#include <QVariantMap>

#include <chrono>
#include <future>
#include <thread>
#include <utility>

namespace logos::plain {

PlainLogosObject::PlainLogosObject(std::string objectName,
                                   std::shared_ptr<RpcConnectionBase> conn)
    : m_objectName(std::move(objectName))
    , m_conn(std::move(conn))
{
}

PlainLogosObject::~PlainLogosObject()
{
    disconnectEvents();
    joinWaiters();
}

void PlainLogosObject::joinWaiters()
{
    std::vector<std::thread> waiters;
    {
        std::lock_guard<std::mutex> g(m_waiterMu);
        waiters.swap(m_waiters);
    }
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
    const bool got = m_completionCv.wait_until(lk, deadline,
        [&] { return m_completions.count(callId) > 0; });
    if (!got) {
        qWarning() << "PlainLogosObject: deferred call" << callId << "timed out";
        if (err)
            *err = logos::callErrorTimeout(m_objectName, methodName.toStdString(),
                                           effectiveMs);
        return QVariant();
    }
    const QVariant result = m_completions[callId];
    m_completions.erase(callId);
    return result;
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
    // The thread is JOINed in joinWaiters() (destructor / release), not
    // detached: capturing `this` for awaitCompletion / m_objectName is
    // only safe while the object is alive, and release() used to
    // `delete this` while a waiter could still be mid-flight.
    const std::string objectName = m_objectName;
    const std::string method = methodName.toStdString();
    // Register under the lock BEFORE the thread can outrun release(): a
    // detach-then-push left a window where delete this raced the waiter.
    {
        std::lock_guard<std::mutex> g(m_waiterMu);
        m_waiters.emplace_back([this, objectName, fut, timeoutMs, methodName, method,
                                callback = std::move(callback)]() mutable {
            if (fut->wait_for(std::chrono::milliseconds(timeoutMs))
                != std::future_status::ready) {
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
            // the completion event) right here on the waiter thread.
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
    // joinWaiters() before delete: in-flight async waiters capture `this`
    // (for awaitCompletion). Detaching them used to let release() free the
    // object under a still-running waiter.
    disconnectEvents();
    joinWaiters();
    m_conn.reset();
    delete this;
}

quintptr PlainLogosObject::id() const
{
    return reinterpret_cast<quintptr>(m_conn.get());
}

} // namespace logos::plain
