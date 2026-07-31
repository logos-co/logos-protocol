#include "rpc_server.h"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/address.hpp>

#include <QDebug>

#include <algorithm>

namespace logos::plain {

// ── RpcServerTcp ──────────────────────────────────────────────────────────

RpcServerTcp::RpcServerTcp(boost::asio::io_context& ioc,
                           const std::string& host,
                           uint16_t port,
                           std::shared_ptr<IWireCodec> codec,
                           IncomingCallHandler* handler)
    : m_acceptor(ioc)
    , m_strand(boost::asio::make_strand(m_acceptor.get_executor()))
    , m_codec(std::move(codec))
    , m_handler(handler)
    , m_host(host)
    , m_port(port)
{
}

bool RpcServerTcp::start()
{
    boost::system::error_code ec;
    boost::asio::ip::tcp::endpoint ep(
        boost::asio::ip::make_address(m_host, ec),
        m_port);
    if (ec) return false;

    // open/bind/listen run here, on the caller's thread, rather than on the
    // strand: start() has to be synchronous through them because callers read
    // boundPort() the moment it returns (port=0 means "let the kernel pick").
    // That is safe because no async operation on m_acceptor exists yet — the
    // first one is armed below — and callers must not overlap start() with
    // stop() (PlainTransportHost holds its own mutex across both).
    m_acceptor.open(ep.protocol(), ec);       if (ec) return false;
    m_acceptor.set_option(boost::asio::socket_base::reuse_address(true), ec);
    m_acceptor.bind(ep, ec);                  if (ec) return false;
    m_acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) return false;

    m_boundPort = m_acceptor.local_endpoint().port();

    // From here on every touch of m_acceptor goes through the strand. Arming
    // the first accept there rather than inline is invisible to clients:
    // listen() has already run, so anything that connects before the strand
    // gets its turn waits in the backlog.
    auto self = shared_from_this();
    boost::asio::dispatch(m_strand, [self] { self->doAccept(); });
    return true;
}

void RpcServerTcp::stop()
{
    // Move the connection list OUT under the lock, then release it before
    // stopping each connection. conn->stop() fails the connection, which
    // synchronously invokes its error handler (set in doAccept) — and that
    // handler locks m_mu to erase itself from m_conns. Holding m_mu across the
    // stop() call would re-enter this non-recursive mutex on the same thread and
    // self-deadlock. The handler's erase is then a harmless no-op (the list it
    // scans is already empty).
    std::vector<std::shared_ptr<TcpConnection>> conns;
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_stopped = true;
        conns.swap(m_conns);
    }
    closeAcceptorOnStrand();
    for (auto& c : conns) c->stop("server stopped");
}

void RpcServerTcp::closeAcceptor()
{
    boost::system::error_code ignore;
    m_acceptor.close(ignore);
}

void RpcServerTcp::closeAcceptorOnStrand()
{
    // The acceptor is subject to exactly the race RpcConnection's stream was
    // (see closeStreamOnStrand() there): asio acceptors are documented "Shared
    // objects: Unsafe", and close() runs cleanup_descriptor_data(), which nulls
    // the reactor's per-descriptor state. doAccept() re-arms async_accept from
    // inside its own completion handler — on the io thread — while stop() is
    // called from an arbitrary caller thread (in practice the host thread, via
    // ~PlainTransportHost). Closing there let the reactor dereference the
    // descriptor state the close had just nulled, inside
    // reactive_socket_service_base::start_op(): SIGSEGV at +0x98 on the io
    // thread, with RpcServerTcp::doAccept() reached from the accept completion
    // handler at the top of the backtrace.
    //
    // dispatch() (not post()) for the same reason as in RpcConnection: when the
    // caller is already on the strand it closes inline; from any other thread it
    // queues and returns immediately, so teardown never blocks. The lambda holds
    // a shared_ptr, so a close queued from a destructor still finds a live
    // object; if the io_context stops first, the acceptor is closed by
    // ~basic_socket_acceptor when the server is destroyed.
    std::shared_ptr<RpcServerTcp> self;
    try { self = shared_from_this(); } catch (...) {}
    if (!self) {
        // No owning shared_ptr — the object is mid-destruction, so no other
        // thread can still be holding it to run an acceptor operation.
        closeAcceptor();
        return;
    }
    boost::asio::dispatch(m_strand, [self] { self->closeAcceptor(); });
}

void RpcServerTcp::doAccept()
{
    auto self = shared_from_this();
    m_acceptor.async_accept(
        boost::asio::bind_executor(m_strand,
        [self](const boost::system::error_code& ec,
               boost::asio::ip::tcp::socket socket) {
            if (ec) return;  // acceptor probably closed; quietly exit.
            std::shared_ptr<TcpConnection> conn;
            {
                std::lock_guard<std::mutex> g(self->m_mu);
                // Test and publish under one lock. A socket accepted after
                // stop() is dropped here — closed by `socket`'s destructor —
                // rather than wrapped in a connection and stop()ed: stop()
                // would reach IncomingCallHandler::onConnectionClosed, and the
                // handler is the thing that calls RpcServer::stop() from its
                // own destructor (~PlainTransportHost), so by the time this
                // runs it may already be gone. Deferring the acceptor close
                // onto the strand widened that window from "impossible"
                // (close() aborted the pending accept before stop() returned)
                // to "a few microseconds", which is long enough to matter.
                if (self->m_stopped) return;
                conn = std::make_shared<TcpConnection>(
                    std::move(socket), self->m_codec, self->m_handler);
                self->m_conns.push_back(conn);
            }
            std::weak_ptr<RpcServerTcp> weakSelf = self;
            conn->setErrorHandler([weakSelf, conn](const std::string&) {
                auto s = weakSelf.lock();
                if (!s) return;
                std::lock_guard<std::mutex> g(s->m_mu);
                s->m_conns.erase(
                    std::remove(s->m_conns.begin(), s->m_conns.end(), conn),
                    s->m_conns.end());
            });
            conn->start();
            self->doAccept();
        }));
}

// ── RpcServerSsl ──────────────────────────────────────────────────────────

RpcServerSsl::RpcServerSsl(boost::asio::io_context& ioc,
                           const std::string& host,
                           uint16_t port,
                           boost::asio::ssl::context sslCtx,
                           std::shared_ptr<IWireCodec> codec,
                           IncomingCallHandler* handler)
    : m_acceptor(ioc)
    , m_strand(boost::asio::make_strand(m_acceptor.get_executor()))
    , m_sslCtx(std::move(sslCtx))
    , m_codec(std::move(codec))
    , m_handler(handler)
    , m_host(host)
    , m_port(port)
{
}

bool RpcServerSsl::start()
{
    // See RpcServerTcp::start() for why bind/listen stay on the caller's thread
    // and only the accept loop moves onto the strand.
    boost::system::error_code ec;
    boost::asio::ip::tcp::endpoint ep(
        boost::asio::ip::make_address(m_host, ec),
        m_port);
    if (ec) return false;

    m_acceptor.open(ep.protocol(), ec);       if (ec) return false;
    m_acceptor.set_option(boost::asio::socket_base::reuse_address(true), ec);
    m_acceptor.bind(ep, ec);                  if (ec) return false;
    m_acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) return false;

    m_boundPort = m_acceptor.local_endpoint().port();

    auto self = shared_from_this();
    boost::asio::dispatch(m_strand, [self] { self->doAccept(); });
    return true;
}

void RpcServerSsl::stop()
{
    // See RpcServerTcp::stop — release m_mu before stopping connections so the
    // per-connection error handler (which re-locks m_mu) can't self-deadlock.
    std::vector<std::shared_ptr<SslConnection>> conns;
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_stopped = true;
        conns.swap(m_conns);
    }
    closeAcceptorOnStrand();
    for (auto& c : conns) c->stop("server stopped");
}

void RpcServerSsl::closeAcceptor()
{
    boost::system::error_code ignore;
    m_acceptor.close(ignore);
}

void RpcServerSsl::closeAcceptorOnStrand()
{
    // See RpcServerTcp::closeAcceptorOnStrand().
    std::shared_ptr<RpcServerSsl> self;
    try { self = shared_from_this(); } catch (...) {}
    if (!self) {
        closeAcceptor();
        return;
    }
    boost::asio::dispatch(m_strand, [self] { self->closeAcceptor(); });
}

void RpcServerSsl::doAccept()
{
    auto self = shared_from_this();
    m_acceptor.async_accept(
        boost::asio::bind_executor(m_strand,
        [self](const boost::system::error_code& ec,
               boost::asio::ip::tcp::socket socket) {
            if (ec) return;
            auto stream = std::make_shared<SslStream>(std::move(socket), self->m_sslCtx);
            // The handshake completes off the strand: it touches the new
            // stream, never m_acceptor, and every server member it reads
            // afterwards is guarded by m_mu.
            stream->async_handshake(
                boost::asio::ssl::stream_base::server,
                [self, stream](const boost::system::error_code& hs) {
                    if (hs) {
                        // Handshake failed. Log the error — silently
                        // discarding the socket used to be a debugging
                        // dead-end (clients see a generic "alert 40"
                        // and can't tell if the cert is bad, a curve
                        // is unavailable, or the listener picked a
                        // version the client refuses). Category + code
                        // + message give enough to grep for.
                        qWarning().nospace()
                            << "RpcServerSsl: TLS handshake failed: "
                            << hs.category().name() << ':' << hs.value()
                            << " (" << QString::fromStdString(hs.message()) << ")";
                        return;
                    }
                    // Hand the SslStream off to a connection that owns
                    // it. We held it in a shared_ptr only for the
                    // duration of async_handshake (so the buffer
                    // outlives the dispatch); now move the underlying
                    // stream into the connection by value.
                    std::shared_ptr<SslConnection> conn;
                    {
                        std::lock_guard<std::mutex> g(self->m_mu);
                        // Dropped rather than stop()ed once the server is
                        // stopped — see the matching note in
                        // RpcServerTcp::doAccept(). Closing the acceptor never
                        // aborted a handshake already in flight, so this path
                        // has always had to survive a handler that is gone.
                        if (self->m_stopped) return;
                        conn = std::make_shared<SslConnection>(
                            std::move(*stream), self->m_codec, self->m_handler);
                        self->m_conns.push_back(conn);
                    }
                    std::weak_ptr<RpcServerSsl> weakSelf = self;
                    conn->setErrorHandler([weakSelf, conn](const std::string&) {
                        auto s = weakSelf.lock();
                        if (!s) return;
                        std::lock_guard<std::mutex> g(s->m_mu);
                        s->m_conns.erase(
                            std::remove(s->m_conns.begin(), s->m_conns.end(), conn),
                            s->m_conns.end());
                    });
                    conn->start();
                });
            self->doAccept();
        }));
}

} // namespace logos::plain
