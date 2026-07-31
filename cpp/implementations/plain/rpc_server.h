#ifndef LOGOS_PLAIN_RPC_SERVER_H
#define LOGOS_PLAIN_RPC_SERVER_H

#include "incoming_call_handler.h"
#include "rpc_connection.h"
#include "wire_codec.h"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/strand.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace logos::plain {

// -----------------------------------------------------------------------------
// RpcServer (TCP) — accepts TCP connections, wraps each in an
// RpcConnection, keeps them alive until they drop.
//
// Every accepted connection uses the same shared IncomingCallHandler so
// the provider layer can dispatch regardless of which client is talking.
// The server doesn't multiplex objects by itself; it's the handler's job
// to look up the target object for each incoming CallMessage.
// -----------------------------------------------------------------------------

using TcpStream = boost::asio::ip::tcp::socket;
using TcpConnection = RpcConnection<TcpStream>;

class RpcServerTcp : public std::enable_shared_from_this<RpcServerTcp> {
public:
    RpcServerTcp(boost::asio::io_context& ioc,
                 const std::string& host,
                 uint16_t port,
                 std::shared_ptr<IWireCodec> codec,
                 IncomingCallHandler* handler);

    // Start accepting. Returns false if bind fails. Synchronous through
    // listen(), so boundPort() is valid the moment it returns true; the accept
    // loop itself is armed on m_strand (see closeAcceptorOnStrand). Callers
    // must not overlap start() with stop() — PlainTransportHost serializes
    // them under its own mutex.
    bool start();

    // Actual bound port (useful when the caller requested port=0).
    uint16_t boundPort() const { return m_boundPort; }

    void stop();

private:
    // MUST run on m_strand — see closeAcceptorOnStrand().
    void doAccept();
    void closeAcceptor();
    void closeAcceptorOnStrand();

    boost::asio::ip::tcp::acceptor    m_acceptor;
    // Serializes every operation on m_acceptor after listen(): the accept
    // initiations and the close. Same reason RpcConnection has one for its
    // stream — asio acceptors are "Shared objects: Unsafe".
    boost::asio::strand<boost::asio::any_io_executor> m_strand;
    std::shared_ptr<IWireCodec>       m_codec;
    IncomingCallHandler*              m_handler;
    std::string                       m_host;
    uint16_t                          m_port;
    uint16_t                          m_boundPort = 0;

    std::mutex                                    m_mu;
    std::vector<std::shared_ptr<TcpConnection>>   m_conns;
    bool                                          m_stopped = false;
};

// -----------------------------------------------------------------------------
// RpcServer (TLS) — same as TCP but wraps every accepted socket in an
// asio::ssl::stream and completes the handshake before spinning up the
// RpcConnection.
// -----------------------------------------------------------------------------

using SslStream = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;
using SslConnection = RpcConnection<SslStream>;

class RpcServerSsl : public std::enable_shared_from_this<RpcServerSsl> {
public:
    RpcServerSsl(boost::asio::io_context& ioc,
                 const std::string& host,
                 uint16_t port,
                 boost::asio::ssl::context sslCtx,
                 std::shared_ptr<IWireCodec> codec,
                 IncomingCallHandler* handler);

    // See RpcServerTcp::start().
    bool start();
    uint16_t boundPort() const { return m_boundPort; }
    void stop();

private:
    // MUST run on m_strand — see closeAcceptorOnStrand().
    void doAccept();
    void closeAcceptor();
    void closeAcceptorOnStrand();

    boost::asio::ip::tcp::acceptor   m_acceptor;
    // See RpcServerTcp::m_strand.
    boost::asio::strand<boost::asio::any_io_executor> m_strand;
    boost::asio::ssl::context        m_sslCtx;
    std::shared_ptr<IWireCodec>      m_codec;
    IncomingCallHandler*             m_handler;
    std::string                      m_host;
    uint16_t                         m_port;
    uint16_t                         m_boundPort = 0;

    std::mutex                                    m_mu;
    std::vector<std::shared_ptr<SslConnection>>   m_conns;
    bool                                          m_stopped = false;
};

} // namespace logos::plain

#endif // LOGOS_PLAIN_RPC_SERVER_H
