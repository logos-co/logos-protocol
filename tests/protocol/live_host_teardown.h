#ifndef LOGOS_TESTS_LIVE_HOST_TEARDOWN_H
#define LOGOS_TESTS_LIVE_HOST_TEARDOWN_H

// Destroying a live PlainTransportHost that publishes a ModuleProxy living on
// its own QThread.
//
// Every "LiveHost" fixture in this directory is the same shape: a
// PlainTransportHost built on the test thread, a ModuleProxy moved onto a
// worker QThread, and publishObject() wiring the two together. They all tore
// down the same way too, and it was wrong the same way:
//
//     m_host.reset();          // test thread
//     m_thread->quit();
//     m_thread->wait();
//
// WHY THAT FAULTS. publishObject() connects a lambda to the proxy's
// eventResponse signal with NO context object, so the connection is direct and
// the lambda runs on whichever thread emits. ModuleProxy always QUEUES that
// emission to its own thread (module_proxy.cpp — it must, or QtRO source
// serialization races the reply socket), so the emitting thread is always the
// proxy's worker thread. The lambda converts the payload and then calls
// PlainTransportHost::fanOutEvent, which locks the host's m_mu.
//
// A reset() on the test thread therefore frees the host underneath a lambda the
// worker thread is already inside. ~PlainTransportHost does disconnect the
// connection, and that closes the window for an emission that has not STARTED —
// but a worker already past the connection lookup is not called back, it is
// simply running, and the payload conversion between lambda entry and the
// fanOutEvent lock is milliseconds wide on a real event. Observed, on this
// tree, as a hard SIGSEGV on the worker thread:
//
//     pthread_mutex_lock
//     std::mutex::lock
//     logos::plain::PlainTransportHost::fanOutEvent      plain_transport_host.cpp:354
//     PlainTransportHost::publishObject(...)::$_0        plain_transport_host.cpp:334
//     doActivate<false>
//     ModuleProxy::eventResponse
//     ModuleProxy::ModuleProxy(...)::$_0                 module_proxy.cpp:38
//
// (`EXC_BAD_ACCESS ... at 0x71de13fa0`, thread named "QThread"; under Guard
// Malloc the freed page is unmapped so it faults every time instead of
// sometimes corrupting. Without a detector the same run reports
// `mutex lock failed: Invalid argument` out of a Qt event handler.)
//
// WHY MOVING reset() AFTER quit()/wait() IS NOT THE FIX — measured, because the
// obvious argument for rejecting it turns out to be wrong. That order DOES stop
// the fault: wait() joins the worker, so a direct-connection slot already
// running there has finished before reset(), and an emission posted after the
// loop exits is never dispatched. It is clean under Guard Malloc, twice over.
//
// It is clean because it throws the queue away. QThread::quit() reaches
// QEventLoop::exit(), which stores an exit flag SYNCHRONOUSLY from the calling
// thread instead of posting an event, so the worker's loop stops at its next
// iteration and every QMetaCallEvent still behind it — every pending
// eventResponse emission — is discarded. Measured on the specimen in
// test_plain_host_event_teardown.cpp (24 events queued per round, 40 rounds):
// 273-383 of 960 delivered, the rest dropped, and dropped silently. In a suite
// whose tests COUNT deliveries that is a worse failure than the crash, because
// nothing reports it.
//
// It is a cross-thread deletion either way, safe only because wait() happened
// to join first; and it shuts the host down AFTER the proxy's thread, which
// test_call_error_after_acquire needs the other way round — the host must stop
// serving before the proxy it dispatches to is dismantled.
//
// THE FIX: do the deletion ON the thread that emits. A QMetaCallEvent is
// dispatched by the worker's event loop, so while it runs the worker is by
// definition not inside any other slot — no emission can be in flight. And Qt
// posts equal-priority events FIFO, so every emission queued before this call
// runs first, against a host that is still alive — 960 of 960 on the same
// specimen, where the quit()-first order delivered a third of them. Everything
// queued after it finds the connection already severed by ~PlainTransportHost.
// The io thread, the third thread that reaches the host, is covered by the drain
// barrier ~PlainTransportHost already carries.
//
// The blocking wait adds no hang that was not already there: the caller's very
// next statements are quit()/wait(), which block on the same worker thread with
// no timeout. As before, park nothing in the provider across teardown — release
// the provider's gate first, as the fixtures do.
//
// The marshal itself is the SDK's own logos::runOnOwnerThread — the same
// primitive every inbound Logos call already uses to reach a module's thread,
// including its same-thread short circuit. Only the "that thread is gone"
// guard is added, because a fixture may tear down after its worker has
// finished and a blocking marshal onto a dead event loop never returns.

#include "logos_thread_marshal.h"
#include "plain_transport_host.h"

#include <QObject>
#include <QThread>

#include <memory>

namespace logos::testing {

inline void destroyHostOnProxyThread(
    std::unique_ptr<logos::plain::PlainTransportHost>& host,
    QObject* proxy)
{
    if (!host) return;

    // No worker to serialize against: no proxy, or its thread has stopped, so
    // nothing can be inside the fan-out lambda and nothing ever will be.
    QThread* worker = proxy ? proxy->thread() : nullptr;
    if (!worker || !worker->isRunning()) {
        host.reset();
        return;
    }

    // Capturing `host` by reference is safe precisely because this blocks until
    // the lambda has returned (and runs it inline when we are already there).
    logos::runOnOwnerThread(proxy, [&host] { host.reset(); });
}

} // namespace logos::testing

#endif // LOGOS_TESTS_LIVE_HOST_TEARDOWN_H
