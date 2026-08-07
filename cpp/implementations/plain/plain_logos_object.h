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
#include <utility>
#include <vector>

namespace logos::plain {

// One in-flight async call. Defined in the .cpp — nothing outside needs its
// shape, and keeping it there keeps Boost.Asio out of this header.
struct AsyncCall;

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

public:
    // ── the shared call state ────────────────────────────────────────────────
    //
    // Everything an OFF-THREAD handler can reach lives here rather than on the
    // handle, and the handle's own lifetime stops mattering to those handlers.
    //
    // That split is not decoration. release() ends in `delete this`, and
    // LogosObject's ABI is frozen (logos_object.h) — the handle crosses module
    // boundaries as a raw pointer, so it cannot itself become shared-owned.
    // But nothing a handler touches goes through the handle: no virtual, no
    // id(), not even its address. So the STATE becomes shared-owned and the
    // facade stays exactly as it was. Handlers hold a shared_ptr to the
    // per-call AsyncCall and a weak_ptr to this block; the last one to run
    // drops the last share, whenever that is.
    //
    // This is the same block the completion-subscription lifetime fix
    // introduced (it was CompletionRendezvous: mutex, condvar, completions),
    // widened to carry the in-flight calls the fold moved off their threads.
    // The guarantee it exists for is unchanged and is now load-bearing for
    // three handlers instead of one: NO HANDLER TOUCHES A DESTROYED OBJECT,
    // true by construction rather than by a barrier.
    struct CallState {
        std::mutex              mu;
        // The SYNC path's rendezvous, unchanged in kind: callMethodWithError
        // still parks its own caller's thread here, because a synchronous call
        // has to block someone and that someone is the caller.
        std::condition_variable cv;
        std::map<QString, QVariant> completions;

        // In-flight ASYNC calls, keyed by the call's wire id. This is the whole
        // retention story on the handle now: an entry exists exactly while its
        // call is outstanding and is erased by the one delivery it gets. No
        // waiter registry, no publish list, no reaping, nothing that survives a
        // completed call.
        std::map<std::uint64_t, std::shared_ptr<AsyncCall>> inflight;
        // Second index over the same calls, for the ones a "multi" provider
        // deferred: the completion event is keyed by the provider's callId
        // string, not by our numeric id.
        std::map<QString, std::shared_ptr<AsyncCall>>       deferred;

        // Set once by teardown, never cleared. Written under `mu` so the
        // condition-variable side cannot miss it, and atomic so the lock-free
        // readers do not have to take the mutex.
        std::atomic<bool> stopping{false};
    };

private:
    // Deferred ("multi") completion rendezvous. A multi provider returns a
    // pending sentinel (logos::pendingCallKey) from callMethod and later pushes
    // the real result as a logos::callCompleteEvent event keyed by callId. We
    // subscribe to that event EAGERLY (before any call can defer) so a completion
    // racing ahead of the caller is buffered, then either resolve the waiting
    // AsyncCall directly (async) or wake the parked caller (sync).
    //
    // The completion arrives on the connection's IO thread, and the subscription
    // holds a weak_ptr to CallState — never `this`. That subscription lives in
    // the RpcConnection, which is SHARED by every PlainLogosObject the
    // connection hands out and outlives all of them (see release()), and
    // dispatchIncoming copies the handler out under its own mutex and invokes it
    // with that mutex RELEASED — so the unsubscribe release() sends cannot reach
    // a handler already in flight, and nothing joins the io thread. With `this`
    // captured, a completion arriving across a release() wrote to a freed
    // object; reproduced as a SIGSEGV under Guard Malloc, in
    // test_plain_completion_sub_lifetime.cpp.
    //
    // ONCE, and — the part that is not the same thing — with every other caller
    // WAITING until it is actually up. The flag used to be raised under
    // CallState::mu and the mutex DROPPED before the subscribe, so a second
    // caller could read "subscribed", build its Call and put it on the wire
    // while the Subscribe frame had not been enqueued yet. A "multi" provider
    // that answers such a call quickly emits its completion into a subscription
    // the host has not registered — PlainTransportHost::fanOutEvent finds no
    // sink for that connection and DROPS it — and the caller then waits out its
    // full timeout for a result that was computed and thrown away. Measured on
    // pristine master, four runs: 18 to 28 of 250 two-thread first-call rounds
    // inverted on the wire, every one a dropped completion and a timed-out
    // caller; 6 to 10 of 500 calls through the real host stack.
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

    // Raise the stop flag, then cancel every outstanding async call — each of
    // which delivers its callback, once, with callErrorReleased — and wake the
    // synchronous caller if one is parked.
    //
    // WHAT REPLACED THE JOIN. In-flight calls used to be threads that captured
    // `this`, so teardown had to prove none of them was still running before
    // `delete this`, and the only tool for that was joining threads it first had
    // to ask to stop (a wait slice at best). Nothing captures `this` any more: a
    // handler holds a shared_ptr to its AsyncCall and a weak_ptr to CallState.
    // Teardown therefore waits for NOTHING — not the io thread, not a wait slice
    // — which also means it cannot deadlock when release() is called from inside
    // an event callback running on the single io thread (the shape
    // remote_transport.cpp documents as real). It stays O(in-flight calls).
    void stopAndCancelCalls();

    std::string                          m_objectName;
    std::shared_ptr<RpcConnectionBase>   m_conn;
    std::mutex                           m_mu;
    std::vector<std::pair<QString, EventCallback>> m_subs;

    // Never null and never reseated: the object owns exactly one state block for
    // its whole life, and the only other references are the weak_ptrs its
    // handlers hold and whatever one of them has momentarily locked.
    std::shared_ptr<CallState>           m_state{std::make_shared<CallState>()};
    // "The Subscribe frame is on the strand." Stored with RELEASE after
    // subscribeToCompletions() returns and read with ACQUIRE on the fast path,
    // so a caller that skips the once_flag still inherits the edge that orders
    // its own Call behind that Subscribe.
    std::atomic<bool>                    m_completionSubscribed{false};
    // What makes a concurrent first caller WAIT rather than sail past a flag
    // that has been raised but not yet honoured. The whole fix is this member.
    std::once_flag                       m_completionSubOnce;
};

} // namespace logos::plain

#endif // LOGOS_PLAIN_LOGOS_OBJECT_H
