// PUBLISHING IS A WAITER'S LAST ACCESS TO THE OBJECT.
//
// Its two sibling suites pin the parts of the waiter mechanism that can be
// observed by watching what the object DOES: test_plain_object_teardown.cpp
// pins what happens to a call still in flight when the handle goes away, and
// test_plain_waiter_reaping.cpp pins that finished waiters are retired and that
// retiring them cannot deadlock. This one pins the ORDERING RULE those two rest
// on, which nothing they do can see.
//
// The rule, from plain_logos_object.cpp. A waiter's exit guard runs
// reapFinishedWaiters() and then publishFinishedWaiter(id), and the publish is
// strictly its last access to the object:
//
//     struct FinishOnExit {
//         ~FinishOnExit() {
//             self->reapFinishedWaiters();      // others, never itself
//             self->publishFinishedWaiter(id);  // strictly last
//         }
//     };
//
// Why that is load-bearing. reapFinishedWaiters() takes the ids waiters have
// published, ERASES those entries from m_waiters under m_waiterMu, and joins the
// threads OUTSIDE the lock. stopAndJoinWaiters() (release() / the destructor)
// swaps m_waiters under the same lock and brute-force joins whatever it got. So
// a waiter that a concurrent reaper is mid-join on is NOT in teardown's map, and
// teardown can return — with release() going straight on to `delete this` —
// while that waiter is still unwinding. stopAndJoinWaiters() says so in its own
// comment: the guarantee is not "everything is joined when this returns" but
// "no waiter touches this object after this returns". Publishing being last is
// the entire reason the second sentence is true.
//
// WHAT THIS IS AND IS NOT. It is NOT a lurking use-after-free that this file
// exposes. Be precise, because the window above was overstated in review: on
// every version of this code, a supported caller's waiters are all joined
// transitively, so "teardown returns while a reaped waiter is still unwinding"
// is not reachable. A waiter leaves m_waiters exactly two ways, and both are
// covered:
//
//   * teardown takes it — and teardown joins it;
//   * a reaper takes it — and that reaper is either ANOTHER WAITER, which is
//     itself still registered (it reaps before it publishes), so teardown joins
//     the reaper and therefore waits out the join it is in; or the ASYNC-SPAWN
//     path, whose join completes before callMethodAsyncWithError returns.
//
// The one reaper nobody waits for is the spawn path racing a concurrent
// release() — i.e. a caller still issuing calls on an object another thread is
// destroying, which is caller-side UB on any version of this class and faults
// on correct code too.
//
// So publish-is-last is an invariant the DESIGN RESTS ON and documents, not a
// hole. This suite pins it against future edits — the TODO above the waiter
// (fold the wait into the shared Asio io_context) moves where reaping happens,
// which is exactly the kind of change that could add an access below the
// publish. It does not close an open bug, and nothing here should be read as
// reporting one.
//
// WHY THAT NEEDS ITS OWN MECHANISM. Because "no supported caller can provoke
// it" also means no test can provoke it, so the defect ships green: with a
// touch added after the publish, the whole of PlainObjectTeardownTest and
// PlainWaiterReapingTest passes cleanly, including under Guard Malloc. A test
// built on the unsupported race would be red on correct code, which is not a
// detector.
//
// So this suite does not race anything. It OBSERVES the accesses directly, in
// two halves, because the object splits cleanly into the state a waiter must
// never touch and the registry it exists to update.
//
// HALF ONE — the state (WaiterTouchesNothingOnTheStatePageAfterPublishing).
//
//   * The object is placement-newed into an mmap'd two-page arena, positioned so
//     that a page boundary falls at (or just under) m_waiterMu — i.e. the members
//     teardown coordinates on (m_waiterMu, m_waiters, m_finishedWaiters,
//     m_nextWaiterId, m_stopping) land on the second page and everything else
//     (m_objectName, m_conn, m_mu, m_subs, the completion rendezvous) on the
//     first.
//   * The first page is mprotect(PROT_NONE)'d for exactly as long as a waiter is
//     running, and a SIGSEGV/SIGBUS handler RECORDS each access — faulting
//     address, thread, and how many ids had been published at that instant — then
//     unprotects so the access proceeds. Nothing crashes; the access is
//     evidence, not a punishment. The handler cannot re-arm the page itself (the
//     faulting instruction would fault forever), so the OBSERVING thread does
//     it: it polls the registry anyway, it never touches the guarded page, and
//     without it the first legitimate access would silently disarm the detector
//     for the rest of the round.
//   * WHAT THE ASSERTION IS. The invariant is "no access AFTER the publish", and
//     that is what gets asserted: a round starts with nothing published, so a
//     recorded access stamped `published >= 1` is one this waiter made below
//     publishFinishedWaiter(). It is deliberately NOT "zero accesses", because
//     zero is stricter than the rule and FALSE on a correct waiter: on the
//     deferred/"multi" path the waiter calls awaitCompletion(), which locks
//     m_completionMu and reads m_completions and m_objectName — all on the
//     guarded page, all before it publishes, all legitimate. The fourth round
//     below drives exactly that path and REQUIRES at least one such access, so
//     the narrowing is not a loophole: it is the difference between the two
//     halves of the object's life, and both are checked. (This file asserted
//     zero when it landed. It was green only because none of its rounds
//     deferred; adding one turns the strict form red on correct code, which is
//     how the over-strictness was found.)
//   * WHERE THE DEFERRED ROUND IS WEAKER, stated because "we relaxed the
//     assertion" is exactly the change that can hide a dead detector. On that
//     one round the post-publish half is best-effort rather than deterministic:
//     a legitimate access opens the page, the re-arm is a syscall behind, and a
//     defect firing a microsecond later slips through. Measured with a read of
//     m_objectName added below the publish, and every round forced to run: the
//     other four catch it 5 times in 5, the deferred round 0 times in 5 — and a
//     variant that spins on the re-arm instead of polling records 4-24 accesses
//     per round and still catches it 0 times in 5, so this is not a tuning
//     problem.
//     IT COSTS NOTHING, because FinishOnExit is ONE piece of code shared by all
//     five exit paths. A defect below the publish is the same defect on every
//     round, and the rounds that can see it deterministically do (25/25 for the
//     whole test, unchanged by the narrowing — see the table). The deferred
//     round is here to keep the assertion honest about correct code, not to add
//     a fifth copy of the same detection.
//
// HALF TWO — the registry (PublishedWaiterDoesNotTouchTheRegistryAgain). The
// page trick cannot cover m_waiterMu, m_waiters or m_finishedWaiters, because
// publishing has to reach them. That half is caught with bait instead; the
// method is described on the test itself.
//
// Everything is driven through a fake RpcConnectionBase, so there is no socket,
// no host, no event loop timing, and no race: the test decides exactly when the
// call's future is satisfied, which is what lets it arm the page while the
// waiter is parked and disarm it only once the waiter is gone. The two rounds
// that end on a deadline (timed out, deferred) are the exception, and they wait
// out a real clock — the budget is set so the thing that has to happen first
// takes microseconds against hundreds of milliseconds of slack, and the deferred
// round asserts that it did rather than assuming it.
//
// WHAT THE TWO HALVES DO AND DO NOT COVER, measured by rebuilding the file under
// test with each defect and running each suite 5-30 times:
//
//   defect below publishFinishedWaiter()      here   teardown+reaping suites
//   ------------------------------------      ----   -----------------------
//   read m_objectName                         25/25                    0/10
//   read m_conn                               10/10                    0/10
//   read m_completions                        10/10                    0/10
//   read m_completionSubscribed               10/10                    0/10
//   lock m_mu                                 25/25                    0/10
//   write m_completions under m_completionMu  25/25                       -
//   call reapFinishedWaiters() again          20/20                    9/30
//   read m_stopping                            0/10                    0/10
//   (publish moved ABOVE the reap)              0/5                   12/15
//   no defect                                  0/30                    0/10
//
// TAKE THE RIGHT-HAND COLUMN AS RATES, NOT AS FRACTIONS. Those suites catch
// these by racing, so a sample is a coin count and not a constant: the re-reap
// cell was first reported here as "2/2", which was a two-run sample printed
// beside 10-40 run samples. Over 30 runs it is 9/30, and another 30-run sample
// gave 12/30 — i.e. roughly one run in three, matching what reapFinishedWaiters'
// own comment says ("about one run in four"). The left-hand column is NOT a
// rate: those are deterministic, and anything below 1 there would be a bug in
// this file rather than bad luck.
//
// The one real gap is m_stopping, the single member that shares the registry's
// page and so cannot be guarded without guarding the publish itself. The one
// defect this suite deliberately leaves alone is the inverted order, which the
// reaping suite's hammer already catches — probabilistically, at 12 runs in 15,
// which is the other half of why observing beats racing.
//
// m_waiterMu / m_waiters / m_finishedWaiters / m_stopping stay private: they are
// reached through the same explicit-instantiation access hole the reaping suite
// uses ([temp.spec] does not check access on the template arguments of an
// explicit instantiation), so the code under test is observed exactly as it
// ships — no friend, no test-only accessor, no #define private public.

#include <gtest/gtest.h>

#include "logos_async_dispatch.h"
#include "logos_call_error.h"

#include "plain_logos_object.h"
#include "rpc_connection.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QVariant>
#include <QVariantList>

#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace logos::plain;

namespace {

// ── reading the waiter registry without touching the production header ──────
template <typename Tag, typename Tag::type Member>
struct Rob {
    friend typename Tag::type get(Tag) { return Member; }
};

struct PubWaitersTag {
    using type = std::map<std::uint64_t, std::thread> PlainLogosObject::*;
    friend type get(PubWaitersTag);
};
template struct Rob<PubWaitersTag, &PlainLogosObject::m_waiters>;

struct PubWaiterMuTag {
    using type = std::mutex PlainLogosObject::*;
    friend type get(PubWaiterMuTag);
};
template struct Rob<PubWaiterMuTag, &PlainLogosObject::m_waiterMu>;

struct PubFinishedTag {
    using type = std::vector<std::uint64_t> PlainLogosObject::*;
    friend type get(PubFinishedTag);
};
template struct Rob<PubFinishedTag, &PlainLogosObject::m_finishedWaiters>;

struct PubStoppingTag {
    using type = std::atomic<bool> PlainLogosObject::*;
    friend type get(PubStoppingTag);
};
template struct Rob<PubStoppingTag, &PlainLogosObject::m_stopping>;

std::size_t memberOffset(const PlainLogosObject* obj, const void* member)
{
    return static_cast<std::size_t>(reinterpret_cast<const char*>(member)
                                    - reinterpret_cast<const char*>(obj));
}

// ── the access recorder ─────────────────────────────────────────────────────
//
// A guarded page plus a fault handler that logs and then lets the access
// through. Deliberately NOT a crash: a test that dies inside a waiter thread
// reports "signal 11" and nothing else, whereas the interesting part is WHICH
// member was touched and whether the touching thread had already published.

constexpr int kMaxFaults = 64;

struct Access {
    std::uintptr_t addr     = 0;
    std::uintptr_t thread   = 0;
    std::size_t    offset   = 0;
    std::size_t    published = 0;   // m_finishedWaiters.size() at fault time
};

std::atomic<int>            gAccessCount{0};
Access                      gAccesses[kMaxFaults];
std::atomic<char*>          gGuardBase{nullptr};
std::atomic<std::size_t>    gGuardLen{0};
std::atomic<char*>          gObjBase{nullptr};
std::atomic<bool>           gArmed{false};   // false once a fault has let one through
const std::vector<std::uint64_t>* gFinished = nullptr;   // on the UNguarded page
struct sigaction            gOldSegv;
struct sigaction            gOldBus;
bool                        gHandlersInstalled = false;

void faultHandler(int /*sig*/, siginfo_t* info, void* /*uctx*/)
{
    char* const base = gGuardBase.load(std::memory_order_acquire);
    const std::size_t len = gGuardLen.load(std::memory_order_acquire);
    char* const addr = static_cast<char*>(info->si_addr);

    if (base == nullptr || addr < base || addr >= base + len) {
        // Not our arena: put the original handlers back and return, so the
        // faulting instruction re-runs and dies with its real diagnosis rather
        // than looping forever in here.
        sigaction(SIGSEGV, &gOldSegv, nullptr);
        sigaction(SIGBUS, &gOldBus, nullptr);
        return;
    }

    const int i = gAccessCount.fetch_add(1, std::memory_order_acq_rel);
    if (i < kMaxFaults) {
        char* const objBase = gObjBase.load(std::memory_order_acquire);
        gAccesses[i].addr   = reinterpret_cast<std::uintptr_t>(addr);
        gAccesses[i].thread = reinterpret_cast<std::uintptr_t>(pthread_self());
        gAccesses[i].offset = objBase ? static_cast<std::size_t>(addr - objBase) : 0;
        // On the registry page, which is never guarded — and with a single
        // waiter in flight the only writer is the very thread stopped here.
        gAccesses[i].published = gFinished ? gFinished->size() : 0;
    }
    // Let it through: re-arming HERE would need a single-step, since the
    // faulting instruction re-runs the moment this returns and would fault
    // forever. Unprotect FIRST and only then drop the armed flag, so an
    // observer racing to re-arm cannot have its mprotect(PROT_NONE) overwritten
    // by this one — the worst interleaving then leaves the page guarded and the
    // flag set, which is the truth.
    mprotect(base, len, PROT_READ | PROT_WRITE);
    gArmed.store(false, std::memory_order_release);
}

void installHandlers()
{
    if (gHandlersInstalled) return;
    struct sigaction sa{};
    sa.sa_sigaction = &faultHandler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    ASSERT_EQ(sigaction(SIGSEGV, &sa, &gOldSegv), 0);
    ASSERT_EQ(sigaction(SIGBUS, &sa, &gOldBus), 0);
    gHandlersInstalled = true;
}

void resetAccesses() { gAccessCount.store(0, std::memory_order_release); }

int accessCount() { return gAccessCount.load(std::memory_order_acquire); }

bool guard(bool on)
{
    char* const base = gGuardBase.load(std::memory_order_acquire);
    const std::size_t len = gGuardLen.load(std::memory_order_acquire);
    if (!base) return false;
    if (mprotect(base, len, on ? PROT_NONE : (PROT_READ | PROT_WRITE)) != 0)
        return false;
    gArmed.store(on, std::memory_order_release);
    return true;
}

// Put the page back after a recorded access let one through. Called ONLY from
// the observing thread, which never touches the guarded page — from anywhere
// else this would be a way to fault on your own re-arm.
//
// Without it the detector is a ONE-SHOT per round: the first access disarms the
// page and everything after it goes unseen. That was harmless while no correct
// waiter ever touched the page, and stops being harmless the moment one
// legitimately does — the deferred round below. What it buys, honestly: the
// page is blind only from a fault until the next poll, instead of for the rest
// of the round. It is NOT enough to catch a defect that fires a microsecond
// after a legitimate access (measured — see the header), and the reason that is
// acceptable is that FinishOnExit is shared by all five rounds, four of which
// see it deterministically. Keep it anyway: any future legitimate access EARLY
// in a round would otherwise silently switch that round's detector off.
void rearmGuard()
{
    if (gArmed.load(std::memory_order_acquire)) return;
    if (accessCount() >= kMaxFaults) return;   // nothing left to record
    guard(true);
}

// THE ACTUAL INVARIANT: accesses made below publishFinishedWaiter(). Each round
// starts with m_finishedWaiters empty (the spawn path reaps the previous round's
// id, and the round asserts it), and one waiter runs at a time, so an access
// stamped `published >= 1` is one this waiter made after publishing its own id.
// Everything stamped 0 happened before the publish, where the object is still
// alive by construction and touching it is legal.
int postPublishAccesses(int* firstIndex = nullptr)
{
    const int n = std::min(accessCount(), kMaxFaults);
    int count = 0;
    for (int i = 0; i < n; ++i) {
        if (gAccesses[i].published == 0) continue;
        if (count == 0 && firstIndex) *firstIndex = i;
        ++count;
    }
    return count;
}

// ── a connection that answers exactly when the test says so ─────────────────
class ScriptedConn : public RpcConnectionBase {
public:
    void start() override {}
    void stop(const std::string& = "stopped") override { m_open = false; }
    bool isOpen() const override { return m_open; }

    std::future<ResultMessage> sendCall(CallMessage msg) override
    {
        auto p = std::make_shared<std::promise<ResultMessage>>();
        auto f = p->get_future();
        std::lock_guard<std::mutex> g(m_mu);
        m_pending[msg.id] = std::move(p);
        m_lastId = msg.id;
        return f;
    }

    std::future<MethodsResultMessage> sendMethods(MethodsMessage msg) override
    {
        std::promise<MethodsResultMessage> p;
        MethodsResultMessage r;
        r.id = msg.id;
        r.ok = true;
        p.set_value(std::move(r));
        return p.get_future();
    }

    void sendSubscribe(SubscribeMessage, std::function<void(EventMessage)>) override {}
    void sendUnsubscribe(UnsubscribeMessage) override {}
    void sendEvent(EventMessage) override {}
    void sendToken(TokenMessage) override {}
    void setErrorHandler(ErrorHandler) override {}
    std::uint64_t nextId() override { return m_nextId++; }

    std::uint64_t lastId()
    {
        std::lock_guard<std::mutex> g(m_mu);
        return m_lastId;
    }

    // Satisfy the call's future. `ok == false` produces the wire-error shape.
    void answer(std::uint64_t id, bool ok)
    {
        ResultMessage r;
        r.id = id;
        r.ok = ok;
        if (ok) {
            r.value = RpcValue(std::string("pong"));
        } else {
            r.err = "provider said no";
            r.errCode = "CALL_FAILED";
        }
        deliver(id, std::move(r));
    }

    // Satisfy it with a "multi" provider's PENDING SENTINEL — ok, but the value
    // says "the real result comes later, as a completion event keyed by this
    // id". Nothing ever pushes that completion here (this connection has no
    // event plumbing at all), so the waiter parks in awaitCompletion() until the
    // call's own deadline, which is what the deferred round wants: a CORRECT
    // waiter legitimately holding m_completionMu and reading m_completions and
    // m_objectName — all on the guarded page — before it publishes.
    void answerDeferred(std::uint64_t id, const std::string& callId)
    {
        RpcMap m;
        m.emplace(logos::pendingCallKey().toStdString(), RpcValue(callId));
        ResultMessage r;
        r.id = id;
        r.ok = true;
        r.value = RpcValue(std::move(m));
        deliver(id, std::move(r));
    }

private:
    void deliver(std::uint64_t id, ResultMessage r)
    {
        std::shared_ptr<std::promise<ResultMessage>> p;
        {
            std::lock_guard<std::mutex> g(m_mu);
            const auto it = m_pending.find(id);
            if (it == m_pending.end()) return;
            p = std::move(it->second);
            m_pending.erase(it);
        }
        p->set_value(std::move(r));
    }

    std::mutex m_mu;
    std::map<std::uint64_t, std::shared_ptr<std::promise<ResultMessage>>> m_pending;
    std::uint64_t m_lastId = 0;
    std::atomic<std::uint64_t> m_nextId{1};
    std::atomic<bool> m_open{true};
};

// ── the arena ───────────────────────────────────────────────────────────────
//
// Two pages. The object straddles the boundary between them, placed so that
// m_waiterMu begins exactly on the second page: page 1 is then everything the
// waiter must never touch, and page 2 is the registry it legitimately does.
class GuardedObject {
public:
    GuardedObject()
    {
        m_pageSize = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        m_conn = std::make_shared<ScriptedConn>();

        // The offset of m_waiterMu, measured on a throwaway instance so the
        // real one can be placed against it.
        std::size_t split = 0;
        {
            PlainLogosObject probe("probe", m_conn);
            split = memberOffset(&probe, &(probe.*get(PubWaiterMuTag{})));
        }
        m_split = split;
        if (m_split % alignof(PlainLogosObject) != 0)
            m_split -= m_split % alignof(PlainLogosObject);

        void* mem = mmap(nullptr, m_pageSize * 2, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) return;
        m_arena = static_cast<char*>(mem);

        char* const at = m_arena + m_pageSize - m_split;
        m_obj = new (at) PlainLogosObject("guarded_module", m_conn);

        m_waiterMuOffset = memberOffset(m_obj, &(m_obj->*get(PubWaiterMuTag{})));
        gFinished = &(m_obj->*get(PubFinishedTag{}));
        gObjBase.store(reinterpret_cast<char*>(m_obj), std::memory_order_release);
        gGuardBase.store(m_arena, std::memory_order_release);
        gGuardLen.store(m_pageSize, std::memory_order_release);
    }

    ~GuardedObject()
    {
        guard(false);
        gGuardBase.store(nullptr, std::memory_order_release);
        gObjBase.store(nullptr, std::memory_order_release);
        gFinished = nullptr;
        // NOT release(): that ends in `delete this`, and this object was never
        // new'd. The destructor is the same teardown minus the free.
        if (m_obj) m_obj->~PlainLogosObject();
        if (m_arena) munmap(m_arena, m_pageSize * 2);
    }

    // The guarded region is [obj, obj + split). What has to hold is that the
    // split lands ON the page boundary and NOT past m_waiterMu — the registry
    // and its mutex must stay writable, or publishing itself would fault. The
    // split is the member offset rounded down to the object's alignment, so on
    // a platform where m_waiterMu is not aligned to it the guard simply stops a
    // few bytes of padding short, which is still correct.
    bool ok() const
    {
        return m_obj != nullptr && m_arena != nullptr
            && m_split > 0 && m_split <= m_waiterMuOffset
            && reinterpret_cast<char*>(m_obj) + m_split == m_arena + m_pageSize
            && sizeof(PlainLogosObject) < m_pageSize;
    }

    PlainLogosObject* obj() const { return m_obj; }
    ScriptedConn* conn() const { return m_conn.get(); }
    std::size_t split() const { return m_split; }
    std::size_t waiterMuOffset() const { return m_waiterMuOffset; }

    std::size_t waiterCount() const
    {
        std::lock_guard<std::mutex> g(m_obj->*get(PubWaiterMuTag{}));
        return (m_obj->*get(PubWaitersTag{})).size();
    }

    std::size_t publishedCount() const
    {
        std::lock_guard<std::mutex> g(m_obj->*get(PubWaiterMuTag{}));
        return (m_obj->*get(PubFinishedTag{})).size();
    }

    void forceStopping()
    {
        (m_obj->*get(PubStoppingTag{})).store(true, std::memory_order_release);
    }

private:
    std::size_t m_pageSize = 0;
    std::size_t m_split = 0;
    std::size_t m_waiterMuOffset = 0;
    char* m_arena = nullptr;
    PlainLogosObject* m_obj = nullptr;
    std::shared_ptr<ScriptedConn> m_conn;
};

QCoreApplication* ensureAppForGuard()
{
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance())
        new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

void pumpMs(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
}

// Wait — WITHOUT pumping, so nothing of ours runs on the object — until the
// waiter has published and had time to unwind past it.
//
// Doubles as the re-armer. Both things it does touch only the registry page
// (publishedCount takes m_waiterMu) or the mapping itself, never the guarded
// page, so this is the one place from which putting the guard back is safe.
bool waitForPublish(const GuardedObject& g, int budgetMs)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < budgetMs) {
        rearmGuard();
        if (g.publishedCount() > 0) return true;
        QThread::usleep(200);
    }
    return false;
}

// The publish is the waiter's last act, so anything it does below it lands in
// the moments right after — which is where the guard has to be up. Poll instead
// of sleeping so a page disarmed by a legitimate pre-publish access is back
// before then.
void settleGuarded(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        rearmGuard();
        QThread::usleep(200);
    }
}

std::string describe(const Access& a, std::size_t split, std::uintptr_t testThread)
{
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "offset %zu (the guarded state page runs to %zu, where the "
                  "waiter registry begins), from %s thread, %zu waiter id(s) "
                  "already published",
                  a.offset, split,
                  a.thread == testThread ? "the TEST" : "a non-test",
                  a.published);
    return std::string(buf);
}

std::uintptr_t selfThread()
{
    return reinterpret_cast<std::uintptr_t>(pthread_self());
}

// A thread parked until the test lets it go — the bait a reaper picks up and
// then blocks on, which is what holds the window open in the second test.
class Gate {
public:
    void wait()
    {
        std::unique_lock<std::mutex> lk(m_mu);
        m_cv.wait(lk, [this] { return m_open; });
    }
    void open()
    {
        {
            std::lock_guard<std::mutex> g(m_mu);
            m_open = true;
        }
        m_cv.notify_all();
    }

private:
    std::mutex              m_mu;
    std::condition_variable m_cv;
    bool                    m_open = false;
};

const char* kGuardToken = "guard-token";

} // namespace

class PlainWaiterPublishIsLastTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ensureAppForGuard();
        installHandlers();
        resetAccesses();
    }
};

// ── 0. the detector has to be able to fire ──────────────────────────────────
//
// The whole suite is an assertion that a counter stays at zero, which is the
// shape of test that passes just as happily when the mechanism under it is
// dead. So prove the mechanism first: a deliberate read of the object's state
// page, from a thread that is not the test's, must be recorded — with the
// address, the thread, and the published-id count that the real check reads.
TEST_F(PlainWaiterPublishIsLastTest, DetectorRecordsADeliberateTouch)
{
    GuardedObject g;
    ASSERT_TRUE(g.ok()) << "arena layout: split=" << g.split()
                        << " m_waiterMu at " << g.waiterMuOffset()
                        << " sizeof=" << sizeof(PlainLogosObject);

    resetAccesses();
    ASSERT_TRUE(guard(true)) << "mprotect failed: " << strerror(errno);

    std::uintptr_t toucherThread = 0;
    std::thread toucher([&] {
        toucherThread = reinterpret_cast<std::uintptr_t>(pthread_self());
        // Any byte of the state page will do; this is m_objectName's first.
        volatile const char* p = reinterpret_cast<const char*>(g.obj());
        (void)*p;
    });
    toucher.join();

    guard(false);

    ASSERT_GE(accessCount(), 1)
        << "the guard page recorded nothing for a read that certainly happened "
           "— the detector is dead and every other test in this file is vacuous";
    EXPECT_EQ(gAccesses[0].thread, toucherThread);
    EXPECT_NE(gAccesses[0].thread, selfThread());
    EXPECT_EQ(gAccesses[0].offset, 0u);
    std::cout << "  detector live: "
              << describe(gAccesses[0], g.split(), selfThread()) << std::endl;
}

// ── 1. the invariant ────────────────────────────────────────────────────────
//
// Five waiters: one per return out of the lambda (answered, rejected, timed out,
// cancelled) plus the deferred detour that awaitCompletion() adds to the
// answered one. Every one of them ends in the same FinishOnExit guard, and it is
// the guard that has to keep its hands off the object once it has published.
//
// Each round arms the state page only after the call has returned (the caller's
// own accesses — m_conn, m_objectName, ensureCompletionSub — are legitimate and
// happen there), and disarms it only once the waiter has published and unwound.
// For that whole window the waiter is the only thing running against the object.
//
// WHAT IS ASSERTED IS "NOTHING AFTER THE PUBLISH", not "nothing at all". Four of
// the five rounds do touch the page zero times, because objectName and method
// are copied into the closure precisely so the waiter needs nothing from the
// object. The DEFERRED round does not, and correctly so: a "multi" provider's
// pending sentinel sends the waiter into awaitCompletion(), which locks
// m_completionMu and reads m_completions and m_objectName — guarded page, before
// the publish, entirely legal. Asserting zero would make this file red on
// correct code, so it asserts what the rule actually says: no access stamped
// `published >= 1`. The deferred round then requires at least one stamped 0, so
// the looser predicate cannot be satisfied by the path simply not running.
//
// Pre-fix — i.e. with any member access added below publishFinishedWaiter — this
// records an access with published >= 1 and fails naming the offset.
TEST_F(PlainWaiterPublishIsLastTest, WaiterTouchesNothingOnTheStatePageAfterPublishing)
{
    GuardedObject g;
    ASSERT_TRUE(g.ok()) << "arena layout: split=" << g.split()
                        << " m_waiterMu at " << g.waiterMuOffset()
                        << " sizeof=" << sizeof(PlainLogosObject);

    struct Round {
        const char* what;
        int         timeoutMs;
        enum { Answer, Fail, Timeout, Deferred, Cancel } how;
        // Rounds where a CORRECT waiter must reach the guarded page before it
        // publishes. Only the deferred path does; requiring it is what keeps
        // that round from passing vacuously if it stops deferring.
        bool        touchesBeforePublishing;
    };
    const Round rounds[] = {
        { "a call that ANSWERS",           5000, Round::Answer,   false },
        { "a call the provider REJECTS",   5000, Round::Fail,     false },
        { "a call that TIMES OUT",          150, Round::Timeout,  false },
        // The pending-sentinel path. The budget is the awaitCompletion deadline
        // (no completion is ever pushed, so it runs out) and also the slack the
        // future has to be satisfied in — microseconds are needed, 400ms given.
        { "a DEFERRED call (pending sentinel)", 400, Round::Deferred, true },
        { "a call CANCELLED by teardown",  5000, Round::Cancel,   false },
    };
    constexpr int kRounds = static_cast<int>(sizeof(rounds) / sizeof(rounds[0]));

    int delivered = 0;
    for (const Round& r : rounds) {
        resetAccesses();

        // Owned by the callback, not by this frame: a bail-out below leaves the
        // waiter to be cancelled in teardown, and its callback is delivered on a
        // LATER event-loop iteration — after this frame is gone. A captured
        // `&got` would be a genuine use-after-free on the way out of a failing
        // test, which is a poor advertisement for a file about use-after-free.
        auto got = std::make_shared<std::atomic<int>>(0);
        g.obj()->callMethodAsyncWithError(
            QString::fromLatin1(kGuardToken), QStringLiteral("ping"),
            QVariantList{ QVariant(1) }, r.timeoutMs,
            [got](QVariant, const logos::CallError&) { got->fetch_add(1); });

        // The waiter is registered (that happens under m_waiterMu inside the
        // call) and parked in waitForResult: nothing can satisfy its future
        // until the line below. Safe to close the state page over it.
        ASSERT_GT(g.waiterCount(), 0u) << r.what << ": no waiter registered";
        // The spawn path above reaps the previous round's id, so nothing is
        // published as this round starts — which is what makes `published >= 1`
        // mean "after THIS waiter's publish" rather than "after some earlier
        // waiter's".
        ASSERT_EQ(g.publishedCount(), 0u)
            << r.what << ": a previous round's waiter id is still published, so "
               "the after-the-publish stamp no longer means what it says";
        ASSERT_TRUE(guard(true)) << "mprotect failed: " << strerror(errno);

        switch (r.how) {
        case Round::Answer:  g.conn()->answer(g.conn()->lastId(), true);  break;
        case Round::Fail:    g.conn()->answer(g.conn()->lastId(), false); break;
        case Round::Timeout: break;                 // let the deadline elapse
        case Round::Deferred:
            g.conn()->answerDeferred(g.conn()->lastId(), "deferred-call-1");
            break;
        case Round::Cancel:  g.forceStopping();     // registry page, not guarded
            break;
        }

        const bool published = waitForPublish(g, 10000);
        // The publish is the waiter's last act; give the thread room to unwind
        // past it, which is where a stray access would land — with the guard
        // kept up for all of it.
        settleGuarded(30);

        const int recorded = accessCount();
        int firstAfter = 0;
        const int afterPublish = postPublishAccesses(&firstAfter);
        guard(false);

        EXPECT_TRUE(published) << r.what << ": the waiter never published";
        ASSERT_EQ(afterPublish, 0)
            << r.what << ": the object's state page was touched AFTER this "
               "waiter published its id, while only the waiter was running — "
            << describe(gAccesses[firstAfter], g.split(), selfThread()) << ".\n"
            << "That access is below publishFinishedWaiter() in the FinishOnExit "
               "guard, and it breaks the guarantee stopAndJoinWaiters() "
               "documents: a reaper joining this waiter outside m_waiterMu has "
               "already taken it out of m_waiters, so teardown does not wait for "
               "it and release() goes on to `delete this` while it runs. No "
               "SUPPORTED caller can reach that reaper today (see the header), "
               "so this is the regression test for an invariant the design "
               "rests on — not the report of a live use-after-free. Publishing "
               "must stay the last thing a waiter does.";

        // A round that must have gone through the guarded page has to prove it,
        // or it is not testing the path it names.
        if (r.touchesBeforePublishing) {
            ASSERT_GT(recorded, 0)
                << r.what << ": nothing on the guarded page was touched, so "
                   "awaitCompletion() never ran — the call did not actually "
                   "defer (a slow machine can turn this into a plain timeout) "
                   "and this round proves nothing about the deferred path";
        } else {
            EXPECT_EQ(recorded, 0)
                << r.what << ": the guarded page was touched at all, which on "
                   "this path a correct waiter never does — "
                << describe(gAccesses[0], g.split(), selfThread());
        }

        // Drain the queued callback so the round is provably complete.
        pumpMs(60);
        delivered += got->load();
        std::cout << "  " << r.what << ": published, " << recorded
                  << " access(es) to the state page, " << afterPublish
                  << " of them after the publish" << std::endl;

        if (r.how == Round::Cancel) break;   // m_stopping never clears
    }

    EXPECT_EQ(delivered, kRounds)
        << "a callback went missing; the rounds did not all run";
}

// ── 2. the other half of the object: the registry itself ────────────────────
//
// The guard page above cannot cover m_waiterMu, m_waiters or m_finishedWaiters —
// publishing has to be able to reach them. So the one access it cannot see is a
// waiter REAPING AGAIN after publishing, which is precisely the mistake
// publishFinishedWaiter's comment calls out: "Nothing the waiter does may follow
// it, its own reap least of all."
//
// Nothing else catches that either. It is invisible to the reaping suite (the
// self-join guard absorbs it into a leaked registry entry rather than a crash)
// and to the teardown suite, and racing a reaper against it catches it roughly
// one run in three — 9 in 30 measured here, 12 in 30 on another 30-run sample,
// against the "2/2" this file first printed from a two-run sample. That spread
// IS the argument for not racing: at that rate a green run means nothing, and
// the bait below is deterministic.
//
// Instead it uses the reaper's own shape against it. reapFinishedWaiters()
// joins OUTSIDE m_waiterMu, so a waiter that has picked up somebody else's
// finished thread sits in that join with the lock free — a window the test can
// hold open for as long as it likes, because the thread being joined is one the
// test planted and can keep parked:
//
//   1. plant BAIT 1 — a parked thread registered under a synthetic id, and that
//      id published — while the call is still in flight;
//   2. answer the call. The waiter's exit guard reaps, takes bait 1, and parks
//      in join(bait 1), holding no lock;
//   3. plant BAIT 2 the same way. There is no hurry: the waiter is parked;
//   4. release bait 1. The waiter finishes its reap and publishes;
//   5. bait 2 is now the tell. A waiter that is done never looks at the registry
//      again, so bait 2 must still be registered. A waiter that reaps once more
//      after publishing takes it.
//
// Bait 1 doubles as a check that the reap really does join with the lock free:
// step 3 needs m_waiterMu while the join is in progress, and says so if it
// cannot get it.
//
// A NOTE ON BAILING OUT, because the bait is a trap for the test as much as for
// the waiter. Every planted thread is parked until this test opens its gate, and
// ~PlainLogosObject joins whatever is still registered — so an early return that
// skips a gate.open() does not fail, it WEDGES. Measured on the first version of
// this test: remove the reap from the exit guard, ASSERT_TRUE(tookBait1) fires
// and prints — and then the destructor blocks forever joining a thread nobody
// will release, so the run ends as a timeout kill (exit 124) with no test result
// at all. With the gates on a scope guard the same break reports the same
// assertion and exits 1 in 10s, which is the tryWithRegistry budget and not a
// hang.
//
// Not hypothetical, either: the TODO above the waiter (fold the wait into the
// shared Asio io_context) moves where reaping happens, which is exactly the edit
// that makes tookBait1 false. So the gates are opened by a scope guard on EVERY
// exit path, and they are declared BEFORE the object so they outlive the
// teardown that joins the threads waiting on them.
TEST_F(PlainWaiterPublishIsLastTest, PublishedWaiterDoesNotTouchTheRegistryAgain)
{
    // Declared first, destroyed last: ~GuardedObject joins the planted threads,
    // and a thread parked on an already-destroyed condition_variable would be
    // its own use-after-free.
    Gate gate1;
    Gate gate2;

    GuardedObject g;

    // Destroyed BEFORE g (declared after it), so by the time the object's
    // teardown joins the bait, both gates are open. A failed precondition below
    // now FAILS — fast, named, with a normal exit code — instead of wedging.
    struct OpenGatesOnExit {
        Gate& first;
        Gate& second;
        ~OpenGatesOnExit() { first.open(); second.open(); }
    } openGatesOnExit{gate1, gate2};

    ASSERT_TRUE(g.ok());

    std::mutex& mu = g.obj()->*get(PubWaiterMuTag{});
    auto& registry = g.obj()->*get(PubWaitersTag{});
    auto& finished = g.obj()->*get(PubFinishedTag{});

    // Far above anything m_nextWaiterId will reach in this test.
    constexpr std::uint64_t kBait1 = 1ull << 40;
    constexpr std::uint64_t kBait2 = (1ull << 40) + 1;

    // Every registry read below is a TRY-lock: if the reap ever started joining
    // with m_waiterMu held, a blocking lock here would hang the suite instead of
    // reporting it.
    auto tryWithRegistry = [&](const std::function<bool()>& fn, int budgetMs,
                               bool* lockedAtLeastOnce) -> bool {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < budgetMs) {
            std::unique_lock<std::mutex> lk(mu, std::try_to_lock);
            if (lk.owns_lock()) {
                if (lockedAtLeastOnce) *lockedAtLeastOnce = true;
                if (fn()) return true;
            }
            QThread::usleep(200);
        }
        return false;
    };

    auto plant = [&](std::uint64_t id, Gate& gate) {
        std::lock_guard<std::mutex> lk(mu);
        registry.emplace(id, std::thread([&gate] { gate.wait(); }));
        finished.push_back(id);
    };

    // 1. a call that cannot finish until the test says so. The counter is owned
    // by the callback for the same reason as in the test above: on a bail-out
    // this callback is delivered after the frame is gone.
    auto got = std::make_shared<std::atomic<int>>(0);
    g.obj()->callMethodAsyncWithError(
        QString::fromLatin1(kGuardToken), QStringLiteral("ping"),
        QVariantList{ QVariant(1) }, 20000,
        [got](QVariant, const logos::CallError&) { got->fetch_add(1); });

    plant(kBait1, gate1);

    // 2. let it finish. Its exit guard reaps first, so it takes bait 1.
    g.conn()->answer(g.conn()->lastId(), true);

    bool locked = false;
    const bool tookBait1 = tryWithRegistry(
        [&] { return registry.count(kBait1) == 0; }, 10000, &locked);
    ASSERT_TRUE(locked)
        << "m_waiterMu was never free while the waiter's reap ran — a reap that "
           "joins while holding it is the deadlock reapFinishedWaiters() "
           "documents";
    ASSERT_TRUE(tookBait1)
        << "the waiter's exit guard never reaped the planted entry, so this "
           "probe never got its window; the test needs updating, not the code";

    // 3. the waiter is now inside join(bait 1), holding nothing.
    plant(kBait2, gate2);

    // 4. release it. Reap finishes, then it publishes — and that must be that.
    gate1.open();

    const bool published = tryWithRegistry(
        [&] {
            for (const std::uint64_t id : finished)
                if (id != kBait2) return true;
            return false;
        },
        10000, nullptr);
    EXPECT_TRUE(published) << "the waiter never published";

    // 5. anything it did after publishing has had ample room to happen.
    QThread::msleep(200);

    bool bait2Gone = false;
    {
        std::lock_guard<std::mutex> lk(mu);
        bait2Gone = registry.count(kBait2) == 0;
    }

    EXPECT_FALSE(bait2Gone)
        << "a waiter that had already published came back and reaped again: the "
           "entry planted while it was parked mid-join is gone from m_waiters, "
           "so something below publishFinishedWaiter() in the FinishOnExit guard "
           "still touches the object.\n"
           "That breaks the guarantee stopAndJoinWaiters() documents. A reaper "
           "that has taken this waiter out of m_waiters and is joining it "
           "outside the lock is not in the map stopAndJoinWaiters() swapped, so "
           "teardown does not wait for it and release() goes on to `delete this` "
           "while the waiter is still running. No supported caller can reach "
           "that reaper today — see the header — so this is a regression test "
           "for an invariant the design rests on, not the report of a live "
           "use-after-free. Publishing has to stay the last thing a waiter does.";

    // Cleanup: free bait 2 whoever ended up holding it, and retire it if the
    // registry still has it.
    gate2.open();
    {
        std::lock_guard<std::mutex> lk(mu);
        const auto it = registry.find(kBait2);
        if (it != registry.end()) {
            std::thread t = std::move(it->second);
            registry.erase(it);
            if (t.joinable()) t.join();
        }
        finished.clear();
    }

    pumpMs(50);
    EXPECT_EQ(got->load(), 1) << "the call did not deliver exactly once";
    std::cout << "  bait planted mid-join survived the publish: the waiter never "
                 "came back to the registry" << std::endl;
}
