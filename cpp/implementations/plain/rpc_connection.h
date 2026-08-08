#ifndef LOGOS_PLAIN_RPC_CONNECTION_H
#define LOGOS_PLAIN_RPC_CONNECTION_H

#include "incoming_call_handler.h"
#include "rpc_framing.h"
#include "rpc_message.h"
#include "wire_codec.h"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace logos::plain {

// -----------------------------------------------------------------------------
// RpcConnectionBase — type-erased public surface of RpcConnection<Stream>.
//
// Callers (plain_logos_object, plain_transport_host) hold a
// shared_ptr<RpcConnectionBase> so they don't have to know whether the
// underlying socket is plain TCP or TLS-wrapped TCP. All the async machinery
// lives in the templated subclass.
// -----------------------------------------------------------------------------
class RpcConnectionBase {
public:
    using ErrorHandler = std::function<void(const std::string& reason)>;
    // A reply, handed over as it arrives instead of parked in a promise.
    //
    // Invoked AT MOST ONCE per call, from one of three places, and a caller has
    // to answer for all three because they are not on the same thread:
    //   * the connection's strand (io thread) when the peer's Result frame is
    //     decoded — the normal path;
    //   * an arbitrary caller thread inside fail(), which sweeps every pending
    //     call when the connection is torn down (stop(), ~PlainTransport-
    //     Connection, RpcServer::stop());
    //   * INLINE on the calling thread, inside sendCallAsync itself, when the
    //     connection is stopped — either before the call was made, or while it
    //     was registering, which is a race sendCallAsync resolves by reclaiming
    //     its own entry rather than by hoping fail() sees it.
    // It must therefore not block and must not run user code directly — see
    // postDelivery in plain_logos_object.cpp.
    //
    // AT MOST ONCE is a property of the REGISTRATION, and it is weaker than it
    // sounds. Four things contend for a registered handler — dispatchIncoming,
    // fail()'s sweep, cancelPending() and sendCallAsync's own reclaim — and the
    // extract-and-erase under m_mu lets exactly one of them have it, so no
    // handler is ever invoked twice.
    //
    // AT LEAST ONCE is the other half, and it is not free either: it holds only
    // because a registration is made BEFORE m_stopped is re-read, so a teardown
    // and a registration cannot pass each other unseen. Get that order wrong and
    // a handler sits in the map of a dead connection forever — see sendCallAsync.
    //
    // What that does NOT buy is a cancel that arrives in time. dispatchIncoming
    // copies the handler out under m_mu and invokes it with the mutex RELEASED,
    // so a cancelPending() landing in that gap erases nothing and the handler
    // runs to completion AFTER cancelPending() has already returned. A caller
    // that gives up must therefore be able to absorb one more call. Both callers
    // here are:
    //   * PlainLogosObject funnels every outcome into AsyncCall::deliver(),
    //     whose CAS makes the later arrival a no-op — that CAS, and nothing at
    //     this layer, is what makes DELIVERY to the user exactly-once;
    //   * sendCall()'s promise handler cannot be reached twice at all (only one
    //     contender ever gets it) and fulfilling a future its caller has already
    //     walked away from is a no-op.
    // test_plain_cancel_pending_race.cpp builds that interleaving by hand rather
    // than racing for it, and pins both.
    using ResultHandler = std::function<void(ResultMessage)>;

    virtual ~RpcConnectionBase() = default;

    virtual void start() = 0;
    virtual void stop(const std::string& reason = "stopped") = 0;
    virtual bool isOpen() const = 0;

    virtual std::future<ResultMessage>        sendCall(CallMessage msg) = 0;
    // The same send, completion-driven. sendCall() is now a thin wrapper over
    // this one (it fulfils a promise from the handler), so there is exactly one
    // registration path and the two cannot drift.
    virtual void sendCallAsync(CallMessage msg, ResultHandler handler) = 0;
    virtual std::future<MethodsResultMessage> sendMethods(MethodsMessage msg) = 0;

    // Forget a pending Call or Methods registration whose caller has given up.
    //
    // THIS IS A RETENTION FIX, and it closes a hole that predates the async
    // rework. m_pendingCalls / m_pendingMethods are emptied by exactly two
    // events: a decoded reply carrying that id, and fail()'s teardown sweep. A
    // call that is resolved by its DEADLINE and never answered is in neither,
    // so its registration — a promise, or now a handler holding the caller's
    // std::function — stayed in the map for the whole life of the connection.
    // Measured against pristine master with a server that never answers: 8.5MB
    // of resident memory over 24,000 orphaned calls, 353 bytes each, growing
    // strictly linearly with the call count. And the connection outlives every
    // handle it hands out, so nothing else was ever going to collect it.
    //
    // Erasing is the right semantic and not merely a cleanup: the caller has
    // already been told the call timed out, so a reply arriving afterwards must
    // be dropped, which is exactly what an absent registration does.
    //
    // Safe to call at any time and from any thread, including for an id that
    // has already been answered (the erase simply finds nothing). Ids come from
    // nextId() and are unique across BOTH maps, so one entry point covers them.
    //
    // BEST EFFORT AGAINST A REPLY ALREADY IN FLIGHT, and deliberately not more.
    // It withdraws a REGISTRATION; it does not stop a handler dispatchIncoming
    // has already taken out of the map. Returning from this is therefore not a
    // guarantee of silence — see ResultHandler for who has to absorb the
    // difference and how.
    virtual void cancelPending(uint64_t id) = 0;

    virtual void sendSubscribe(SubscribeMessage msg,
                               std::function<void(EventMessage)> callback) = 0;
    virtual void sendUnsubscribe(UnsubscribeMessage msg) = 0;
    virtual void sendEvent(EventMessage msg) = 0;
    virtual void sendToken(TokenMessage msg) = 0;

    virtual void setErrorHandler(ErrorHandler handler) = 0;
    virtual uint64_t nextId() = 0;
};

// -----------------------------------------------------------------------------
// RpcConnection<Stream> — one full-duplex RPC conversation over a Boost.Asio
// stream-like socket (plain TCP or SSL-wrapped TCP, sharing this template).
//
// Roles: the same connection supports both directions. Either peer can
// initiate Call / Methods / Subscribe / Token / Event messages. Provider-side
// dispatch of inbound Call/Methods/Subscribe/Token goes through an
// IncomingCallHandler supplied at construction (may be null for pure-consumer
// connections).
//
// Lifecycle: heap-allocated via std::make_shared; call start() once the
// socket is ready; call stop() (or destroy) to tear down.
// -----------------------------------------------------------------------------
template <typename Stream>
class RpcConnection
    : public RpcConnectionBase
    , public std::enable_shared_from_this<RpcConnection<Stream>>
{
public:
    RpcConnection(Stream stream,
                  std::shared_ptr<IWireCodec> codec,
                  IncomingCallHandler* handler = nullptr);

    void start() override;
    void stop(const std::string& reason = "stopped") override;
    bool isOpen() const override { return !m_stopped.load(); }

    std::future<ResultMessage>        sendCall(CallMessage msg) override;
    void sendCallAsync(CallMessage msg, ResultHandler handler) override;
    std::future<MethodsResultMessage> sendMethods(MethodsMessage msg) override;

    void cancelPending(uint64_t id) override {
        std::lock_guard<std::mutex> g(m_mu);
        m_pendingCalls.erase(id);
        m_pendingMethods.erase(id);
    }

    void sendSubscribe(SubscribeMessage msg,
                       std::function<void(EventMessage)> callback) override;
    void sendUnsubscribe(UnsubscribeMessage msg) override;
    void sendEvent(EventMessage msg) override;
    void sendToken(TokenMessage msg) override;

    void setErrorHandler(ErrorHandler handler) override {
        std::lock_guard<std::mutex> g(m_mu);
        m_error = std::move(handler);
    }

    uint64_t nextId() override {
        return m_nextId.fetch_add(1, std::memory_order_relaxed);
    }

private:
    void doRead();
    void handleFrame(MessageType tag, std::vector<uint8_t> payload);
    void dispatchIncoming(AnyMessage msg);
    void writeFrame(std::vector<uint8_t> frame);
    void doWrite();
    void fail(const std::string& reason);

    // Close the socket. MUST run on m_strand — see closeStreamOnStrand().
    void closeStream();
    void closeStreamOnStrand();

    Stream                                       m_stream;
    std::shared_ptr<IWireCodec>                  m_codec;
    IncomingCallHandler*                         m_handler;
    boost::asio::strand<boost::asio::any_io_executor> m_strand;

    // Read side
    FrameReader                                  m_reader;
    std::vector<uint8_t>                         m_readBuf;

    // Write side
    std::deque<std::vector<uint8_t>>             m_writeQueue;
    bool                                         m_writing = false;

    // Outgoing-pending maps. Calls hold a HANDLER rather than a promise: the
    // promise is one possible handler (see sendCall), not the mechanism.
    std::mutex                                   m_mu;
    std::map<uint64_t, ResultHandler>                                       m_pendingCalls;
    std::map<uint64_t, std::shared_ptr<std::promise<MethodsResultMessage>>> m_pendingMethods;

    using EventKey = std::pair<std::string, std::string>; // object, event
    std::map<EventKey, std::function<void(EventMessage)>> m_eventCallbacks;

    ErrorHandler                                 m_error;
    std::atomic<uint64_t>                        m_nextId{1};
    std::atomic<bool>                            m_stopped{false};
    std::atomic<bool>                            m_started{false};
};

// ── Template implementation (must be visible at instantiation sites) ─────

template <typename Stream>
RpcConnection<Stream>::RpcConnection(Stream stream,
                                     std::shared_ptr<IWireCodec> codec,
                                     IncomingCallHandler* handler)
    : m_stream(std::move(stream))
    , m_codec(std::move(codec))
    , m_handler(handler)
    , m_strand(boost::asio::make_strand(m_stream.get_executor()))
{
    m_readBuf.resize(4096);
}

template <typename Stream>
void RpcConnection<Stream>::start()
{
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true)) return;
    auto self = this->shared_from_this();
    boost::asio::post(m_strand, [self] { self->doRead(); });
}

template <typename Stream>
void RpcConnection<Stream>::stop(const std::string& reason)
{
    fail(reason);
}

template <typename Stream>
void RpcConnection<Stream>::doRead()
{
    auto self = this->shared_from_this();
    m_stream.async_read_some(boost::asio::buffer(m_readBuf),
        boost::asio::bind_executor(m_strand,
            [self](const boost::system::error_code& ec, std::size_t n) {
                if (ec) { self->fail(ec.message()); return; }
                // fail() may have run on another thread while this read was in
                // flight. Before the close moved onto the strand it aborted the
                // read immediately, so a stopped connection could not deliver
                // one more frame; now the socket stays open until the strand
                // gets to it, and a frame arriving in that gap would be
                // dispatched into an IncomingCallHandler its owner may already
                // have torn down. The connection is dead either way — drop it.
                if (self->m_stopped.load()) return;
                try {
                    self->m_reader.append(self->m_readBuf.data(), n);
                    MessageType tag;
                    std::vector<uint8_t> payload;
                    while (self->m_reader.next(tag, payload)) {
                        self->handleFrame(tag, std::move(payload));
                    }
                } catch (const std::exception& e) {
                    self->fail(std::string("frame error: ") + e.what());
                    return;
                }
                self->doRead();
            }));
}

template <typename Stream>
void RpcConnection<Stream>::handleFrame(MessageType tag, std::vector<uint8_t> payload)
{
    AnyMessage msg;
    try {
        msg = m_codec->decode(tag, payload.data(), payload.size());
    } catch (const std::exception& e) {
        fail(std::string("decode error: ") + e.what());
        return;
    }
    dispatchIncoming(std::move(msg));
}

template <typename Stream>
void RpcConnection<Stream>::dispatchIncoming(AnyMessage msg)
{
    std::visit([this](auto&& m) {
        using T = std::decay_t<decltype(m)>;

        if constexpr (std::is_same_v<T, ResultMessage>) {
            ResultHandler h;
            {
                std::lock_guard<std::mutex> g(m_mu);
                auto it = m_pendingCalls.find(m.id);
                if (it != m_pendingCalls.end()) {
                    h = std::move(it->second);
                    m_pendingCalls.erase(it);
                }
            }
            // Erased under the lock BEFORE the call, so this, fail()'s sweep and
            // cancelPending() cannot all get the same handler — that is what
            // makes INVOCATION at-most-once at this layer, and it is the whole
            // of what this layer promises. It is NOT a cancellation barrier: the
            // call below runs with m_mu released, so a cancelPending() racing it
            // finds the entry already gone, erases nothing, and returns while
            // this handler is still running. Exactly-once DELIVERY belongs to the
            // handler — see ResultHandler.
            if (h) h(std::forward<decltype(m)>(m));

        } else if constexpr (std::is_same_v<T, MethodsResultMessage>) {
            std::shared_ptr<std::promise<MethodsResultMessage>> p;
            {
                std::lock_guard<std::mutex> g(m_mu);
                auto it = m_pendingMethods.find(m.id);
                if (it != m_pendingMethods.end()) {
                    p = std::move(it->second);
                    m_pendingMethods.erase(it);
                }
            }
            if (p) p->set_value(std::forward<decltype(m)>(m));

        } else if constexpr (std::is_same_v<T, EventMessage>) {
            std::function<void(EventMessage)> cb;
            std::function<void(EventMessage)> wildcardCb;
            {
                std::lock_guard<std::mutex> g(m_mu);
                auto it = m_eventCallbacks.find({m.object, m.eventName});
                if (it != m_eventCallbacks.end()) cb = it->second;
                auto wit = m_eventCallbacks.find({m.object, std::string{}});
                if (wit != m_eventCallbacks.end()) wildcardCb = wit->second;
            }
            if (cb)         cb(m);
            if (wildcardCb) wildcardCb(m);

        } else if constexpr (std::is_same_v<T, CallMessage>) {
            if (!m_handler) return;
            auto self = this->shared_from_this();
            m_handler->onCall(m, [self](ResultMessage res) {
                self->writeFrame(encodeFrame(*self->m_codec, AnyMessage{std::move(res)}));
            });

        } else if constexpr (std::is_same_v<T, MethodsMessage>) {
            if (!m_handler) return;
            auto self = this->shared_from_this();
            m_handler->onMethods(m, [self](MethodsResultMessage res) {
                self->writeFrame(encodeFrame(*self->m_codec, AnyMessage{std::move(res)}));
            });

        } else if constexpr (std::is_same_v<T, SubscribeMessage>) {
            if (!m_handler) return;
            // weak_ptr capture so the host's stored sink doesn't keep the
            // connection alive past its natural lifetime — without this,
            // `[self]` would leak every subscribed connection until
            // unsubscribe (which a crashing client never sends).
            std::weak_ptr<RpcConnection<Stream>> weak = this->shared_from_this();
            const void* connId = static_cast<const void*>(this);
            m_handler->onSubscribe(m, [weak](EventMessage evt) {
                if (auto self = weak.lock()) self->sendEvent(std::move(evt));
            }, connId);

        } else if constexpr (std::is_same_v<T, UnsubscribeMessage>) {
            if (m_handler)
                m_handler->onUnsubscribe(m, static_cast<const void*>(this));

        } else if constexpr (std::is_same_v<T, TokenMessage>) {
            if (m_handler) m_handler->onToken(m);
        }
    }, std::move(msg));
}

template <typename Stream>
std::future<ResultMessage>
RpcConnection<Stream>::sendCall(CallMessage msg)
{
    auto p = std::make_shared<std::promise<ResultMessage>>();
    auto f = p->get_future();
    // The promise is now just one shape of handler. Everything the future path
    // relied on — registration under m_mu before the write, the stopped answer,
    // fail()'s sweep — lives in sendCallAsync and is shared verbatim, which is
    // also why this path inherits the register-then-re-check ordering there
    // instead of needing its own.
    sendCallAsync(std::move(msg), [p](ResultMessage r) {
        try { p->set_value(std::move(r)); } catch (...) {}
    });
    return f;
}

// REGISTER FIRST, THEN RE-CHECK — the order is the whole of this function, and
// it is the opposite of what reads naturally.
//
// The obvious spelling is "if stopped, answer inline; otherwise register". It
// has a hole, because the two things it does are ordered the opposite way round
// from fail(): this reads m_stopped and then takes m_mu, while fail() writes
// m_stopped and then takes m_mu. A fail() that completes in between sweeps a
// map this caller has not written to yet —
//
//     caller                              fail()
//     ------------------------------      -------------------------------
//     m_stopped.load()  -> false
//                                         CAS m_stopped -> true
//                                         lock(m_mu); swap(m_pendingCalls)
//                                         unlock(m_mu)   ... the map was EMPTY
//     lock(m_mu); m_pendingCalls[id] = h
//     writeFrame()                        ... drops: stopped
//
// — and the handler is left in the pending map of a connection nobody will
// sweep again. fail() runs once and has been; no reply can arrive on a closed
// socket; the frame was never written. THE CALL IS ANSWERED BY NOTHING, and
// what answers instead is the caller's deadline: callMethodAsyncWithError
// reports "timeout" after its full timeoutMs, and getMethods() after a
// hard-coded five seconds. A connection already known to be gone is reported as
// a peer that was merely slow, which is also the code callers retry on.
//
// Registering unconditionally and then re-reading m_stopped closes it, and the
// reason it closes it is an ordering argument rather than a smaller window:
//
//   * if fail()'s sweep ran BEFORE the registration, then its CAS ran before
//     that, so this re-read cannot see false. The reclaim below finds the
//     entry and answers the call.
//   * if this re-read DOES see false, then in the total order over m_stopped
//     this load precedes fail()'s store, and the registration is
//     sequenced-before this load, so it precedes fail()'s lock of m_mu — the
//     sweep is guaranteed to find the entry and answer the call.
//
// There is no third case, and neither branch can lose the handler: exactly one
// of the reclaim and the sweep extracts it, because both extract-and-erase
// under m_mu. That is the same single-winner rule dispatchIncoming and
// cancelPending already play by, with one more contender.
//
// WHAT WAS REJECTED, since the tempting fixes are the ones that deadlock:
//
//   * "Hold m_mu across the check AND the delivery" — i.e. answer the doomed
//     call from inside the lock. It self-deadlocks on the first inline
//     delivery: a handler here is AsyncCall's, which calls deliver(), which
//     calls RpcConnectionBase::cancelPending() to withdraw its own
//     registration, which takes m_mu — and m_mu is not recursive. The same
//     objection kills the symmetric version (fail() invoking its swept handlers
//     under the lock). Nothing in this class may invoke a handler with m_mu
//     held, which is why the reclaim below drops the lock first.
//   * "Move fail()'s once-only CAS under m_mu and check m_stopped under the
//     same lock here." Correct, and it does not deadlock — but it makes
//     teardown's flag wait on a mutex every in-flight send and every decoded
//     reply also takes, so m_stopped stops being the instantly-visible "stop
//     writing" signal that writeFrame(), doWrite() and doRead() all read
//     lock-free. That trades a race for a teardown-latency regression, and
//     teardown latency is a guarantee here.
//   * "Let writeFrame() notice." It already re-checks m_stopped on the strand
//     — and DROPS, silently, which is exactly the state being fixed. It also
//     cannot answer anything if the io_context is stopped and the posted
//     handler never runs.
//
// The cost is one map insert plus one erase for a call made on a connection
// that is already dead — a case that used to return without touching the map.
// That buys a single registration path (the already-stopped call and the raced
// one are now the same code) instead of two that have to be kept in agreement.
//
// ONE BEHAVIOURAL CONSEQUENCE OF REGISTERING UNCONDITIONALLY, which is benign
// but is not obvious: a call on an already-dead connection is now briefly
// visible in m_pendingCalls, so a cancelPending() for the same id landing in
// that window takes the entry and this function delivers nothing. That is
// correct rather than tolerated. The only thing that can cancel an id whose
// sendCallAsync has not returned yet is AsyncCall's deadline (armTimer runs
// before the send), and reaching deliver() means the caller has already had its
// one callback — a second one would be the bug. It is also exactly what
// cancelPending() is documented to mean: the caller has been answered, so a
// later answer must be dropped.
template <typename Stream>
void RpcConnection<Stream>::sendCallAsync(CallMessage msg, ResultHandler handler)
{
    if (!handler) return;
    const uint64_t id = msg.id;
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_pendingCalls[id] = std::move(handler);
    }

    if (m_stopped.load()) {
        // Take back OUR OWN registration — by id, so this can never remove
        // somebody else's (ids come from nextId() and are unique across both
        // pending maps). Finding nothing is the ordinary outcome when fail()'s
        // sweep got here first, and it means the call has already been
        // answered.
        ResultHandler mine;
        {
            std::lock_guard<std::mutex> g(m_mu);
            auto it = m_pendingCalls.find(id);
            if (it != m_pendingCalls.end()) {
                mine = std::move(it->second);
                m_pendingCalls.erase(it);
            }
        }
        // Answered INLINE, on the caller's thread, with m_mu RELEASED. That is
        // the same shape the future path had (it set the promise before
        // returning it), and it is why every handler in this codebase has to be
        // non-blocking and has to hand user code off to the Qt loop rather than
        // run it here — this thread can be the io thread, since a user event
        // callback runs inline on it and is allowed to make calls.
        if (mine) {
            ResultMessage r;
            r.id = id; r.ok = false;
            r.err = "connection stopped"; r.errCode = "TRANSPORT_CLOSED";
            mine(std::move(r));
        }
        return;
    }

    writeFrame(encodeFrame(*m_codec, AnyMessage{std::move(msg)}));
}

// The same register-then-re-check as sendCallAsync, for the same reason and
// with the same ordering argument — see the comment there. Kept as its own
// registration rather than folded into that one because the maps hold different
// things (a promise, not a handler); what must not diverge is the ORDER, and it
// does not.
//
// It matters more here, if anything: the only caller, getMethods(), waits on
// this future for a hard-coded five seconds that no caller can shorten, so a
// lost registration is a fixed five-second stall in module introspection every
// time a connection drops underneath it.
template <typename Stream>
std::future<MethodsResultMessage>
RpcConnection<Stream>::sendMethods(MethodsMessage msg)
{
    auto p = std::make_shared<std::promise<MethodsResultMessage>>();
    auto f = p->get_future();
    const uint64_t id = msg.id;
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_pendingMethods[id] = p;
    }

    if (m_stopped.load()) {
        std::shared_ptr<std::promise<MethodsResultMessage>> mine;
        {
            std::lock_guard<std::mutex> g(m_mu);
            auto it = m_pendingMethods.find(id);
            if (it != m_pendingMethods.end()) {
                mine = std::move(it->second);
                m_pendingMethods.erase(it);
            }
        }
        if (mine) {
            MethodsResultMessage r;
            r.id = id; r.ok = false; r.err = "connection stopped";
            // Same containment as fail()'s sweep: a promise whose future has
            // already been consumed throws, and that is not this caller's
            // problem.
            try { mine->set_value(std::move(r)); } catch (...) {}
        }
        return f;
    }

    writeFrame(encodeFrame(*m_codec, AnyMessage{std::move(msg)}));
    return f;
}

// THE THIRD REGISTRATION WITH THE SAME SHAPE, and deliberately NOT given the
// same treatment — recorded here so the next reader does not have to wonder
// whether it was missed. A fail() landing between the map write below and the
// writeFrame() leaves a callback in m_eventCallbacks that fail() has already
// cleared, exactly as it would have left a pending call.
//
// What that costs is different in kind, and that is the whole reason: nobody is
// WAITING on a subscription. There is no deadline to blow, no error code to get
// wrong and no caller to strand — the connection is dead, so no event can ever
// arrive to invoke it, and the entry dies with the connection (which the handle
// does not outlive). Adding a reclaim here would buy one map erase and a second
// mutex round trip on the subscribe path in exchange for nothing observable, so
// the pending maps get the fix and this does not.
template <typename Stream>
void RpcConnection<Stream>::sendSubscribe(SubscribeMessage msg,
                                          std::function<void(EventMessage)> cb)
{
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_eventCallbacks[{msg.object, msg.eventName}] = std::move(cb);
    }
    writeFrame(encodeFrame(*m_codec, AnyMessage{std::move(msg)}));
}

template <typename Stream>
void RpcConnection<Stream>::sendUnsubscribe(UnsubscribeMessage msg)
{
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_eventCallbacks.erase({msg.object, msg.eventName});
    }
    writeFrame(encodeFrame(*m_codec, AnyMessage{std::move(msg)}));
}

template <typename Stream>
void RpcConnection<Stream>::sendEvent(EventMessage msg)
{
    writeFrame(encodeFrame(*m_codec, AnyMessage{std::move(msg)}));
}

template <typename Stream>
void RpcConnection<Stream>::sendToken(TokenMessage msg)
{
    writeFrame(encodeFrame(*m_codec, AnyMessage{std::move(msg)}));
}

template <typename Stream>
void RpcConnection<Stream>::writeFrame(std::vector<uint8_t> frame)
{
    if (m_stopped.load()) return;
    auto self = this->shared_from_this();
    boost::asio::post(m_strand, [self, frame = std::move(frame)]() mutable {
        // Re-check inside the strand: the load above is a hint, and fail()
        // can land between it and this handler. Without this the queued
        // frame would start an async_write on a socket fail() is closing.
        if (self->m_stopped.load()) return;
        self->m_writeQueue.push_back(std::move(frame));
        if (!self->m_writing) {
            self->m_writing = true;
            self->doWrite();
        }
    });
}

template <typename Stream>
void RpcConnection<Stream>::doWrite()
{
    // Runs on m_strand. fail() may have closed the socket already (via a
    // close it dispatched onto this same strand); starting another write
    // would only produce a bad_descriptor completion.
    if (m_stopped.load()) { m_writing = false; return; }
    auto self = this->shared_from_this();
    boost::asio::async_write(m_stream,
        boost::asio::buffer(m_writeQueue.front()),
        boost::asio::bind_executor(m_strand,
            [self](const boost::system::error_code& ec, std::size_t /*n*/) {
                if (ec) { self->fail(ec.message()); return; }
                self->m_writeQueue.pop_front();
                if (self->m_writeQueue.empty()) {
                    self->m_writing = false;
                } else {
                    self->doWrite();
                }
            }));
}

template <typename Stream>
void RpcConnection<Stream>::closeStream()
{
    boost::system::error_code ignore;
    try {
        // lowest_layer() works for plain asio::ip::tcp::socket (returns
        // itself) and for asio::ssl::stream (returns the underlying TCP
        // socket). Closing the lowest layer tears the stack down cleanly
        // without needing protocol-specific shutdown sequences.
        m_stream.lowest_layer().close(ignore);
    } catch (...) {}
}

template <typename Stream>
void RpcConnection<Stream>::closeStreamOnStrand()
{
    // Asio sockets are NOT safe for concurrent use ("Shared objects:
    // Unsafe"), and close() is no exception: it runs
    // cleanup_descriptor_data(), which nulls the reactor's per-descriptor
    // state. Every other touch of m_stream in this class is serialized on
    // m_strand — start()/writeFrame() post onto it, doRead()/doWrite()
    // complete through bind_executor(m_strand, …). A strand serializes
    // *handlers*; a raw call made from outside it is not covered.
    //
    // fail() is reached from both sides: from the io thread (a read/write
    // handler that saw an error, already inside the strand) and from an
    // arbitrary caller thread (stop(), ~PlainTransportConnection,
    // RpcServer::stop()). Closing on the caller's thread let close() run
    // concurrently with an in-flight doWrite() initiating async_write on
    // the io thread, and the reactor dereferenced the descriptor state the
    // close had just nulled → SIGSEGV inside
    // reactive_socket_service_base::start_op().
    //
    // dispatch() (not post()) is deliberate: when fail() is already running
    // inside the strand it invokes closeStream() inline, so the io-thread
    // error path keeps its current synchronous behaviour and cannot
    // deadlock on itself. From any other thread it queues onto the strand
    // and returns immediately — never blocking, so teardown cannot hang.
    //
    // The lambda keeps a shared_ptr to this connection, so a close queued
    // from a destructor still finds a live object. Should the io_context be
    // stopped before the queued close runs, the socket is still closed when
    // the connection (and with it m_stream) is destroyed.
    std::shared_ptr<RpcConnection<Stream>> self;
    try { self = this->shared_from_this(); } catch (...) {}
    if (!self) {
        // No owning shared_ptr — the object is mid-destruction, so no other
        // thread can still be holding it to run a stream operation.
        closeStream();
        return;
    }
    boost::asio::dispatch(m_strand, [self] { self->closeStream(); });
}

template <typename Stream>
void RpcConnection<Stream>::fail(const std::string& reason)
{
    bool expected = false;
    if (!m_stopped.compare_exchange_strong(expected, true)) return;

    // Fail every pending call with a transport-level error.
    std::map<uint64_t, ResultHandler>                                       calls;
    std::map<uint64_t, std::shared_ptr<std::promise<MethodsResultMessage>>> methods;
    ErrorHandler errCb;
    {
        std::lock_guard<std::mutex> g(m_mu);
        calls.swap(m_pendingCalls);
        methods.swap(m_pendingMethods);
        errCb.swap(m_error);
        m_eventCallbacks.clear();
    }
    for (auto& [id, h] : calls) {
        ResultMessage r; r.id = id; r.ok = false;
        r.err = reason; r.errCode = "TRANSPORT_ERROR";
        // Runs on WHATEVER THREAD called stop() — usually not the io thread.
        // Handlers are written for that (see ResultHandler); the try/catch is
        // the same containment the promise sweep already had.
        try { h(std::move(r)); } catch (...) {}
    }
    for (auto& [id, p] : methods) {
        MethodsResultMessage r; r.id = id; r.ok = false; r.err = reason;
        try { p->set_value(std::move(r)); } catch (...) {}
    }

    closeStreamOnStrand();

    // Notify the dispatch handler so it can drop any subscriptions still
    // keyed to this connection. Without this, a connection that drops
    // without sending Unsubscribe leaks sinks in the host's per-event map.
    if (m_handler) {
        try { m_handler->onConnectionClosed(static_cast<const void*>(this)); }
        catch (...) {}
    }

    if (errCb) errCb(reason);
}

} // namespace logos::plain

#endif // LOGOS_PLAIN_RPC_CONNECTION_H
