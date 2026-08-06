#ifndef LOGOS_PLAIN_LOGOS_OBJECT_H
#define LOGOS_PLAIN_LOGOS_OBJECT_H

#include "logos_object.h"

#include "rpc_connection.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
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

    // Join and drop the waiters that have already FINISHED, so a handle that
    // outlives its calls does not accumulate them. TWO call sites, which
    // between them cover both shapes of traffic:
    //
    //   * every async spawn — a call pays for the corpses of earlier ones;
    //   * every waiter as it finishes, BEFORE it publishes its own id — so a
    //     burst drains itself instead of parking until the next call, which for
    //     a module that bursts and goes quiet may never come.
    //
    // NOT called from stopAndJoinWaiters(): teardown joins by id-independent
    // brute force and needs no published list. (It used to say otherwise here;
    // it never did.) Cheap either way: a join on an already-returned thread is
    // a couple of syscalls, and only ids a waiter itself published are touched.
    void reapFinishedWaiters();
    // A waiter's FINAL act — see the scope guard in callMethodAsyncWithError.
    // After this returns, that thread never touches the object again, which is
    // what makes it safe for someone else to join and drop it. Nothing the
    // waiter does may follow it, its own reap least of all.
    void publishFinishedWaiter(std::uint64_t id);

    std::string                          m_objectName;
    std::shared_ptr<RpcConnectionBase>   m_conn;
    std::mutex                           m_mu;
    std::vector<std::pair<QString, EventCallback>> m_subs;

    std::mutex                           m_completionMu;
    std::condition_variable              m_completionCv;
    std::map<QString, QVariant>          m_completions;
    bool                                 m_completionSubscribed = false;

    // The waiter registry. KEYED, not a plain vector, because a thread cannot
    // join itself: a waiter can therefore never retire its own entry, and a
    // vector left only one moment to clear it — teardown — so every completed
    // call parked a finished-but-unjoined thread (~one page of resident memory
    // each) for the whole life of the handle. The production shape is one
    // cached handle per module reused for every call (logos_api_consumer.cpp),
    // so that grew without bound. Now a waiter publishes its id into
    // m_finishedWaiters as its last act, and both the next spawn and every
    // OTHER waiter on its way out join and erase it: see reapFinishedWaiters().
    //
    // Retention tracks neither call count nor peak concurrency. A burst drains
    // as it completes, because each waiter reaps the ones that finished before
    // it. What survives an idle handle is only what published after the last
    // reap — at minimum the last waiter to finish, which by construction has
    // nobody behind it to collect it (measured: 1-2 after a 2000-call burst).
    // The next call, or teardown, takes those.
    //
    // The real fix is still the TODO in callMethodAsyncWithError — fold the
    // wait into the shared Asio io_context and have no thread per pending RPC
    // at all. This makes the interim honest, it does not replace that.
    std::mutex                           m_waiterMu;
    std::map<std::uint64_t, std::thread> m_waiters;
    std::vector<std::uint64_t>           m_finishedWaiters;
    std::uint64_t                        m_nextWaiterId = 0;
    // Read lock-free by the sliced future wait and under m_completionMu by
    // awaitCompletion's predicate; written under m_completionMu so the
    // condition-variable side cannot miss it. Never cleared — an object that
    // has begun tearing down does not come back.
    std::atomic<bool>                    m_stopping{false};
};

} // namespace logos::plain

#endif // LOGOS_PLAIN_LOGOS_OBJECT_H
