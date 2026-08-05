#ifndef LOGOS_PLAIN_LOGOS_OBJECT_H
#define LOGOS_PLAIN_LOGOS_OBJECT_H

#include "logos_object.h"

#include "rpc_connection.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace logos::plain {

// -----------------------------------------------------------------------------
// PlainLogosObject — consumer-side LogosObject backed by the plain-C++
// RPC runtime. Identical public shape to LocalLogosObject / RemoteLogosObject
// so LogosAPIConsumer doesn't care which backend it's talking to.
//
// Owns a shared_ptr<RpcConnectionBase>; the transport layer hands the
// connection over after opening the socket. release() stops the connection.
// -----------------------------------------------------------------------------
class PlainLogosObject : public LogosObject, public LogosObjectErrorChannel {
public:
    PlainLogosObject(std::string objectName,
                     std::shared_ptr<RpcConnectionBase> conn);
    ~PlainLogosObject() override;

    QVariant callMethod(const QString& authToken,
                        const QString& methodName,
                        const QVariantList& args,
                        int timeoutMs) override;

    void callMethodAsync(const QString& authToken,
                         const QString& methodName,
                         const QVariantList& args,
                         int timeoutMs,
                         AsyncResultCallback callback) override;

    // LogosObjectErrorChannel — the real implementations. The two LogosObject
    // entry points above are thin adapters that discard the error, so there is
    // exactly ONE call path per direction and the two front doors cannot drift.
    QVariant callMethodWithError(const QString& authToken,
                                 const QString& methodName,
                                 const QVariantList& args,
                                 int timeoutMs,
                                 logos::CallError* err) override;

    void callMethodAsyncWithError(const QString& authToken,
                                  const QString& methodName,
                                  const QVariantList& args,
                                  int timeoutMs,
                                  AsyncResultErrorCallback callback) override;

    bool informModuleToken(const QString& authToken,
                           const QString& moduleName,
                           const QString& token,
                           int timeoutMs) override;

    void onEvent(const QString& eventName, EventCallback callback) override;
    void disconnectEvents() override;
    void emitEvent(const QString& eventName, const QVariantList& data) override;
    QJsonArray getMethods() override;
    void release() override;
    quintptr id() const override;

private:
    // Deferred ("multi") completion rendezvous. A multi provider returns a
    // pending sentinel (logos::pendingCallKey) from callMethod and later pushes
    // the real result as a logos::callCompleteEvent event keyed by callId. We
    // subscribe to that event EAGERLY (before any call can defer) so a completion
    // racing ahead of the waiter is buffered, then block the caller until the
    // matching callId lands. The completion arrives on the connection's IO
    // thread; the caller waits on another thread — m_completionMu/Cv bridge them.
    void ensureCompletionSub();
    // `err` (optional) receives the reason when no completion lands: the
    // timeout when the deadline elapses (a deferred call that gives up is a
    // timeout like any other, and used to be reported as a null result), or a
    // transport error when the object is released out from under the wait.
    QVariant awaitCompletion(const QString& callId, int timeoutMs,
                             const QString& methodName = QString(),
                             logos::CallError* err = nullptr);

    // Ask every in-flight waiter to give up, then join them, then return.
    //
    // The JOIN is what makes the waiters safe at all: they capture `this` (they
    // read m_stopping and call awaitCompletion), and callMethodAsync used to
    // DETACH them, so release()/delete racing an in-flight wait was a
    // use-after-free. But joining alone means teardown blocks for whatever is
    // left of the call's timeout — up to 20s on the protocol default — because
    // a waiter has no reason to return early. Hence the stop first: it costs
    // one wait slice instead, and a cancelled call still delivers its callback
    // exactly once (with an error), because dropping it would turn the stall
    // into a permanent hang in the caller awaiting it.
    void stopAndJoinWaiters();
    // Raise the stop flag and wake anything parked on m_completionCv. Split out
    // because the flag has to be published under m_completionMu (see the .cpp).
    void stopWaiters();

    std::string                          m_objectName;
    std::shared_ptr<RpcConnectionBase>   m_conn;
    std::mutex                           m_mu;
    std::vector<std::pair<QString, EventCallback>> m_subs;

    std::mutex                           m_completionMu;
    std::condition_variable              m_completionCv;
    std::map<QString, QVariant>          m_completions;
    bool                                 m_completionSubscribed = false;

    std::mutex                           m_waiterMu;
    std::vector<std::thread>             m_waiters;
    // Read lock-free by the sliced future wait and under m_completionMu by
    // awaitCompletion's predicate; written under m_completionMu so the
    // condition-variable side cannot miss it. Never cleared — an object that
    // has begun tearing down does not come back.
    std::atomic<bool>                    m_stopping{false};
};

} // namespace logos::plain

#endif // LOGOS_PLAIN_LOGOS_OBJECT_H
