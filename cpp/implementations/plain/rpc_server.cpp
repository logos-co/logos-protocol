#include "rpc_server.h"

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

    m_acceptor.open(ep.protocol(), ec);       if (ec) return false;
    m_acceptor.set_option(boost::asio::socket_base::reuse_address(true), ec);
    m_acceptor.bind(ep, ec);                  if (ec) return false;
    m_acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) return false;

    m_boundPort = m_acceptor.local_endpoint().port();
    doAccept();
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
        boost::system::error_code ignore;
        m_acceptor.close(ignore);
        conns.swap(m_conns);
    }
    for (auto& c : conns) c->stop("server stopped");
}

void RpcServerTcp::doAccept()
{
    auto self = shared_from_this();
    m_acceptor.async_accept(
        [self](const boost::system::error_code& ec,
               boost::asio::ip::tcp::socket socket) {
            if (ec) return;  // acceptor probably closed; quietly exit.
            auto conn = std::make_shared<TcpConnection>(
                std::move(socket), self->m_codec, self->m_handler);
            {
                std::lock_guard<std::mutex> g(self->m_mu);
                if (self->m_stopped) { conn->stop("server stopped"); return; }
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
        });
}

// ── RpcServerSsl ──────────────────────────────────────────────────────────

RpcServerSsl::RpcServerSsl(boost::asio::io_context& ioc,
                           const std::string& host,
                           uint16_t port,
                           boost::asio::ssl::context sslCtx,
                           std::shared_ptr<IWireCodec> codec,
                           IncomingCallHandler* handler)
    : m_acceptor(ioc)
    , m_sslCtx(std::move(sslCtx))
    , m_codec(std::move(codec))
    , m_handler(handler)
    , m_host(host)
    , m_port(port)
{
}

bool RpcServerSsl::start()
{
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
    doAccept();
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
        boost::system::error_code ignore;
        m_acceptor.close(ignore);
        conns.swap(m_conns);
    }
    for (auto& c : conns) c->stop("server stopped");
}

void RpcServerSsl::doAccept()
{
    auto self = shared_from_this();
    m_acceptor.async_accept(
        [self](const boost::system::error_code& ec,
               boost::asio::ip::tcp::socket socket) {
            if (ec) return;
            auto stream = std::make_shared<SslStream>(std::move(socket), self->m_sslCtx);
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
                    auto conn = std::make_shared<SslConnection>(
                        std::move(*stream), self->m_codec, self->m_handler);
                    {
                        std::lock_guard<std::mutex> g(self->m_mu);
                        if (self->m_stopped) { conn->stop("server stopped"); return; }
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
        });
}

} // namespace logos::plain
