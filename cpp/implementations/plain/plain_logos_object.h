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
    // The deferred ("multi") completion rendezvous, in a block that is OWNED by
    // the object but does not DIE with it.
    //
    // A multi provider returns a pending sentinel (logos::pendingCallKey) from
    // callMethod and later pushes the real result as a logos::callCompleteEvent
    // event keyed by callId. We subscribe to that event EAGERLY (before any
    // call can defer) so a completion racing ahead of the waiter is buffered,
    // then block the caller until the matching callId lands. The completion
    // arrives on the connection's IO thread; the caller waits on another
    // thread — mu/cv bridge them.
    //
    // WHY IT IS A SEPARATE BLOCK. The subscription handler lives in the
    // RpcConnection, which is SHARED by every PlainLogosObject the connection
    // hands out and outlives all of them (see release()). RpcConnection copies
    // a handler out of its map under its own mutex and then invokes it with
    // that mutex RELEASED — so the unsubscribe release() sends cannot reach a
    // handler already in flight on the io thread, and nothing joins that
    // thread. When the handler captured `this`, a completion arriving across a
    // release() wrote to a freed object; reproduced as a SIGSEGV under Guard
    // Malloc, in test_plain_completion_sub_lifetime.cpp.
    //
    // Putting the rendezvous behind a shared_ptr and handing the handler a
    // weak_ptr makes "no handler touches a destroyed object" true by
    // construction: a handler that locks it keeps it alive for the length of
    // one callback, and one that cannot lock it does nothing. Nothing else in
    // this object is reachable from the handler, which is what keeps the fix
    // this small.
    struct CompletionRendezvous {
        std::mutex                  mu;
        std::condition_variable     cv;
        std::map<QString, QVariant> completions;
    };

    // Bring the completion subscription up, ONCE, and — the part that is not
    // the same thing — make every other caller wait until it is actually up.
    //
    // The window this closes is a LOST COMPLETION, not a crash. The flag used to
    // be raised under the rendezvous mutex and the mutex DROPPED before the
    // subscribe, so a second caller could read "subscribed", build its Call and
    // put it on the wire while the Subscribe frame had not been enqueued yet. A
    // "multi" provider that answers such a call quickly emits its completion
    // into a subscription the host has not registered — PlainTransportHost::
    // fanOutEvent finds no sink for that connection and DROPS it — and the
    // caller then waits out its full timeout for a result that was computed and
    // thrown away. Measured on pristine master, four runs: 18 to 28 of 250
    // two-thread first-call rounds inverted on the wire, every one of them a
    // dropped completion and a timed-out caller; 6 to 10 of 500 calls through
    // the real host stack.
    //
    // Ordering, once the two are serialized, is a property of asio and not of
    // luck: handlers posted to a strand run in the order they were posted when
    // the posts are ordered by a happens-before edge, and the release/acquire
    // pair below (or call_once's own edge) is that edge.
    void ensureCompletionSub();
    // The subscribe itself, run by exactly one caller — the one that wins
    // m_completionSubOnce.
    void subscribeToCompletions();
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
    // Raise the stop flag and wake anything parked on the rendezvous cv. Split
    // out because the flag has to be published under the rendezvous mutex (see
    // the .cpp).
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

    // Never null and never reseated: the object owns exactly one rendezvous for
    // its whole life, and the only other references are the weak_ptr the
    // subscription handler holds and whatever a handler has momentarily locked.
    std::shared_ptr<CompletionRendezvous> m_completion =
        std::make_shared<CompletionRendezvous>();
    // "The Subscribe frame is on the strand." Stored with RELEASE after
    // subscribeToCompletions() returns and read with ACQUIRE on the fast path,
    // so a caller that skips the once_flag still inherits the edge that orders
    // its own Call behind that Subscribe.
    std::atomic<bool>                    m_completionSubscribed{false};
    // What makes a concurrent first caller WAIT rather than sail past a flag
    // that has been raised but not yet honoured. The whole fix is this member.
    std::once_flag                       m_completionSubOnce;

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
    // nobody behind it to collect it. The next call, or teardown, takes those.
    //
    // That remainder is the size of the last exit batch, NOT a small constant,
    // and nothing collects it while the handle stays idle: sampled out to 25.6s
    // it does not move. What sets it is how SERIALIZED the exits are, because a
    // waiter reaps and only then publishes: exits that interleave one after
    // another leave 1, while a batch that becomes runnable together leaves most
    // of itself, since the reaper that collects a large batch sits in its join
    // loop (no lock held) while everyone behind it publishes and finds nobody to
    // collect them. Measured on the drain of an 800-call burst that was fully
    // outstanding before any reply — the worst case for this, and the shape
    // test_plain_waiter_reaping.cpp builds deliberately — with all 800 answered
    // at once: 1 every run on 10-core macOS, but 2-389 on a 6-core Linux box. It
    // does not scale with the CALL COUNT, which is the claim; "1-2" was a
    // one-platform reading of it, and a burst answered at any pace at all (16 at
    // a time is enough, measured) leaves 1-6 on both.
    //
    // The real fix is still the TODO in callMethodAsyncWithError — fold the
    // wait into the shared Asio io_context and have no thread per pending RPC
    // at all. This makes the interim honest, it does not replace that.
    std::mutex                           m_waiterMu;
    std::map<std::uint64_t, std::thread> m_waiters;
    std::vector<std::uint64_t>           m_finishedWaiters;
    std::uint64_t                        m_nextWaiterId = 0;
    // Read lock-free by the sliced future wait and under the rendezvous mutex
    // by awaitCompletion's predicate; written under that same mutex so the
    // condition-variable side cannot miss it. Never cleared — an object that
    // has begun tearing down does not come back.
    std::atomic<bool>                    m_stopping{false};
};

} // namespace logos::plain

#endif // LOGOS_PLAIN_LOGOS_OBJECT_H
