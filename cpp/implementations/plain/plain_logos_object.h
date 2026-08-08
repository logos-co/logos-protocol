#ifndef LOGOS_PLAIN_LOGOS_OBJECT_H
#define LOGOS_PLAIN_LOGOS_OBJECT_H

#include "logos_object.h"

#include "rpc_connection.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
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

    // RELEASE() IS SAFE AGAINST CALLS THAT ARE ALREADY RUNNING, and the line
    // between that and what is still a caller error is worth stating exactly.
    //
    // release() used to end in an unconditional `delete this`, so a call another
    // thread was executing at that moment went on to touch freed members — a
    // synchronous callMethod parked in its wait, released from a second thread,
    // faults on the very next line it runs. That is fixed, not merely
    // documented: this object carries a LIVE-REFERENCE COUNT. Every entry into a
    // public method takes a reference before it touches anything else and drops
    // it after everything else, release() drops the owner's reference instead of
    // deleting, and whoever drops the LAST one destroys the object. So the
    // destruction is DEFERRED to the moment the last in-flight call leaves — on
    // that call's thread, not the releasing one — and release() itself still
    // waits for nothing.
    //
    //   SAFE:      release() concurrent with any call that had ENTERED before
    //              release() was called, sync or async, from any number of
    //              threads. Also release() re-entered from inside a call or an
    //              event callback on the same thread, which is shipped
    //              behaviour.
    //   STILL A    a call that ENTERS at or after release() (the object may
    //   CALLER     already be gone, and a reference cannot be taken out of freed
    //   ERROR:     storage), and `delete obj` instead of release() while a call
    //              is in flight (the caller has demanded destruction NOW).
    //
    // Both remaining shapes are reported, and abort in debug builds, whenever
    // the object still exists to notice — which for an entry after release() is
    // whenever some other call is holding it alive. When the storage is already
    // freed, nothing inside the object can see it; that residue is the contract
    // documented on LogosObject::release() and cannot be closed from here.
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
        // Completions that arrived before anyone was waiting on them. It exists
        // for ONE ordering: a "multi" provider whose worker finishes before the
        // sentinel it is answering has been written, which puts the completion
        // event on the wire AHEAD of its own Result. The synchronous caller is
        // still blocked on the Result at that instant and cannot possibly have
        // registered, so the completion has to be parked somewhere.
        //
        // WHOSE completions, though, is a question this map could not answer
        // until every handle got its own event channel. The callId is minted by
        // the provider, so a completion arriving early is not attributable to a
        // handle at all — every handle subscribed to the object now sees it, and
        // a handle that parked every one it could not place would grow this map
        // without bound for the whole life of the connection. (Pre-fix the same
        // leak existed with one victim instead of N: the single handle that had
        // stolen the channel parked every OTHER handle's completions here and
        // never claimed one of them.)
        //
        // So parking is gated on `busy` below and the map is EMPTIED the moment
        // this handle has nothing outstanding — at which point, by construction,
        // nothing it holds can ever be claimed.
        std::map<QString, QVariant> completions;
        // Order of arrival, so the gate below can evict the oldest rather than
        // refuse the newest when the cap is hit. Entries here may name a callId
        // that has already been claimed; the eviction skips those.
        std::deque<QString>         completionOrder;

        // Synchronous calls this handle has issued that have not finished — from
        // just before the Call goes out until callMethodWithError returns,
        // INCLUDING the deferred wait in awaitCompletion. Async calls are counted
        // by `inflight` and deferred ones by `deferred`, so the three together are
        // "this handle could still claim a completion".
        int syncOutstanding = 0;

        // Guarded by `mu`. True while some call of this handle's could still turn
        // out to own an unattributed completion.
        bool busy() const
        {
            return syncOutstanding > 0 || !inflight.empty() || !deferred.empty();
        }
        // All three below run under `mu`, which the caller already holds.
        //
        // Park an unclaimed completion, or drop it when it cannot be ours.
        void parkCompletion(const QString& callId, const QVariant& value);
        // Claim a parked completion. Keeps `completions` and `completionOrder`
        // exactly in step, which is the whole reason it is one function.
        bool takeCompletion(const QString& callId, QVariant* out);
        // Called whenever something stops being outstanding: at that point
        // anything still parked is provably another handle's.
        void dropUnclaimedIfIdle();

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

    // ── the live-reference count, and the detector beside it ────────────────
    //
    // m_liveRefs is the MECHANISM: 1 for the owner's reference — the one
    // release() drops — plus one per caller currently inside a public entry
    // point. Whoever drops it to zero destroys the object. That is what makes
    // release() safe against a call already running (see release()), and it is
    // reached only through EntryGuard and release(), never read to make a
    // decision anywhere else.
    //
    // m_callsInFlight and m_lastEntryPoint are the DETECTOR, and they are NOT
    // the same thing as the count above even though they move together. They
    // answer "is somebody else inside this object right now, and where did they
    // come in", for the two moments where that means the calling program is
    // wrong in a way the reference count cannot repair: `delete obj` while a
    // call is in flight, and a call entering after release(). A separate counter
    // rather than a reading of m_liveRefs because the two differ in exactly the
    // cases that matter — during destruction m_liveRefs is zero by construction,
    // and the diagnostic still has to say how many callers were inside.
    //
    // The detector is deliberately biased to UNDER-report and never to
    // false-alarm; see reportConcurrentCallers() for the shape it deliberately
    // ignores. A detector that can fire on a correct program is worse than no
    // detector.
    //
    // All three members are present in every build, NDEBUG or not, so this class
    // has one layout everywhere: this header ships in the source export and is
    // compiled into consumers whose optimisation settings are not ours to
    // choose. Only the ABORT is conditional.
    std::atomic<int>         m_liveRefs{1};
    std::atomic<int>         m_callsInFlight{0};
    std::atomic<const char*> m_lastEntryPoint{nullptr};
    // Raised by release() before it tears anything down, so a call ENTERING
    // afterwards can be named instead of quietly running against a dead object.
    // Never cleared.
    std::atomic<bool>        m_released{false};

    // One per public entry point. Takes a live reference on the way in and drops
    // it — last of all — on the way out, so a call that has entered cannot be
    // destroyed underneath itself. RAII so an early return, of which
    // callMethodWithError has four, can neither leak a reference (the object
    // would never be destroyed) nor drop one twice, and so a nested entry
    // (callMethodWithError calls ensureCompletionSub, which calls onEvent) hands
    // the diagnostic name back on the way out instead of leaving it pointing at
    // the innermost frame that happened to run last.
    class EntryGuard {
    public:
        EntryGuard(PlainLogosObject* obj, const char* entryPoint);
        ~EntryGuard();
        EntryGuard(const EntryGuard&) = delete;
        EntryGuard& operator=(const EntryGuard&) = delete;
    private:
        PlainLogosObject* m_obj;
        const char*       m_prev;
    };

    // Prints the diagnostic and, in a debug build, aborts. `where` names the
    // teardown entry point that found the object busy.
    void reportConcurrentCallers(const char* where) const;
    // The other half: a call that entered after release(). Named for the entry
    // point it came in through.
    void reportEntryAfterRelease(const char* entryPoint) const;
    // disconnectEvents() without the EntryGuard, for the two callers that must
    // NOT take a reference: release() and the destructor. A guard taken during
    // destruction would raise the count from zero and drop it again, and the
    // drop-to-zero is what destroys the object — i.e. it would recurse into
    // `delete this` from inside `delete this`.
    void disconnectEventsImpl();

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
    // The connection-side tokens for THIS handle's subscriptions, not the
    // (name, callback) pairs they used to be. The connection now holds several
    // registrations per (object, event) — one per handle that asked — so the only
    // way to withdraw ours and no one else's is to name the registration.
    std::vector<RpcConnectionBase::SubscriptionId> m_subs;

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
