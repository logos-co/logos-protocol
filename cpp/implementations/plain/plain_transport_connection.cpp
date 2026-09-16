#include "plain_transport_connection.h"

#include "cbor_codec.h"
#include "io_context_pool.h"
#include "json_codec.h"
#include "plain_logos_object.h"
#include "rpc_server.h"

#include <QDebug>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/steady_timer.hpp>

#include <openssl/ssl.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <utility>

namespace logos::plain {

namespace {

// A redial gives up after this; a failed one is not retried sooner than kRedialInterval.
constexpr std::chrono::milliseconds kDialDeadline{5000};
constexpr std::chrono::milliseconds kRedialInterval{250};
// What a zero-timeout caller (the subscription retry tick) waits for a fresh dial: enough for loopback.
constexpr int kFreshDialWaitMs = 20;

std::shared_ptr<IWireCodec> makeCodec(LogosWireCodec kind)
{
    switch (kind) {
    case LogosWireCodec::Cbor: return std::make_shared<CborCodec>();
    case LogosWireCodec::Json:
    default:                   return std::make_shared<JsonCodec>();
    }
}

boost::asio::ssl::context buildClientSslCtx(const LogosTransportConfig& cfg)
{
    boost::asio::ssl::context ctx(boost::asio::ssl::context::tls_client);
    ctx.set_options(boost::asio::ssl::context::default_workarounds
                    | boost::asio::ssl::context::no_sslv2
                    | boost::asio::ssl::context::no_sslv3);
    if (!cfg.caFile.empty())
        ctx.load_verify_file(cfg.caFile);
    ctx.set_verify_mode(cfg.verifyPeer
        ? boost::asio::ssl::verify_peer
        : boost::asio::ssl::verify_none);
    return ctx;
}

void configureTlsClient(SslStream& stream, const LogosTransportConfig& cfg)
{
    // Set SNI: TLS clients must advertise the target host name
    // in the ClientHello so the server picks the right cert
    // (and so any intermediate proxy can route correctly). Without
    // this, vhost-style deployments would terminate the handshake.
    // Cast through the OpenSSL macro because Asio doesn't expose
    // SNI configuration at the wrapper level.
    if (!SSL_set_tlsext_host_name(stream.native_handle(), cfg.host.c_str())) {
        qWarning() << "PlainTransportConnection: SSL_set_tlsext_host_name failed";
    }
    // Verify the peer's certificate name matches the host we
    // dialed when verifyPeer is on. verify_peer alone only
    // validates the chain — without host-name verification a
    // valid cert for a *different* name would still pass, which
    // is exactly the MITM hole verify_peer is meant to close.
    if (cfg.verifyPeer) {
        stream.set_verify_callback(boost::asio::ssl::host_name_verification(cfg.host));
    }
}

boost::asio::io_context& sharedIo() { return IoContextPool::shared().ioContext(); }

} // anonymous namespace

// One background connection attempt, run entirely on the io thread. Its handlers own it, so it
// outlives a transport that abandons it; cancel() makes sure nobody inherits what it connected.
struct PlainTransportConnection::Dial : std::enable_shared_from_this<Dial> {
    explicit Dial(LogosTransportConfig c)
        : cfg(std::move(c)), startedAt(std::chrono::steady_clock::now()) {}

    void start()
    {
        auto self = shared_from_this();
        boost::asio::post(sharedIo(), [self] { self->resolve(); });
    }

    void cancel()
    {
        std::shared_ptr<RpcConnectionBase> orphan;
        {
            std::lock_guard<std::mutex> g(mu);
            cancelled = true;
            orphan = std::move(conn);
        }
        if (orphan) orphan->stop("redial abandoned");
        auto self = shared_from_this();
        boost::asio::post(sharedIo(), [self] { self->abortIo(); });
    }

    bool finished() const { std::lock_guard<std::mutex> g(mu); return done; }

    bool mayRetry() const
    {
        std::lock_guard<std::mutex> g(mu);
        return done && !conn && std::chrono::steady_clock::now() - startedAt >= kRedialInterval;
    }

    std::shared_ptr<RpcConnectionBase> take() { std::lock_guard<std::mutex> g(mu); return std::move(conn); }

    bool waitFinished(int ms)
    {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, std::chrono::milliseconds(ms), [this] { return done; });
    }

private:
    // ── io thread only below ──
    void resolve()
    {
        if (isCancelled()) { finish(nullptr); return; }
        auto self = shared_from_this();
        deadline = std::make_shared<boost::asio::steady_timer>(sharedIo(), kDialDeadline);
        deadline->async_wait([self](const boost::system::error_code& ec) {
            if (!ec) self->abortIo();
        });
        resolver = std::make_shared<boost::asio::ip::tcp::resolver>(sharedIo());
        resolver->async_resolve(cfg.host, std::to_string(cfg.port),
            [self](const boost::system::error_code& ec,
                   boost::asio::ip::tcp::resolver::results_type endpoints) {
                if (ec || self->isCancelled()) { self->finish(nullptr); return; }
                self->connect(endpoints);
            });
    }

    void connect(const boost::asio::ip::tcp::resolver::results_type& endpoints)
    {
        auto self = shared_from_this();
        if (cfg.protocol == LogosProtocol::Tcp) {
            tcp = std::make_shared<TcpStream>(sharedIo());
            boost::asio::async_connect(*tcp, endpoints,
                [self](const boost::system::error_code& ec, const boost::asio::ip::tcp::endpoint&) {
                    if (ec) { self->finish(nullptr); return; }
                    auto c = std::make_shared<TcpConnection>(
                        std::move(*self->tcp), makeCodec(self->cfg.codec), nullptr);
                    c->start();
                    self->finish(std::move(c));
                });
            return;
        }
        if (cfg.protocol == LogosProtocol::TcpSsl) {
            try {
                auto ctx = buildClientSslCtx(cfg);
                ssl = std::make_shared<SslStream>(sharedIo(), ctx);
                configureTlsClient(*ssl, cfg);
            } catch (const std::exception& e) {
                qWarning() << "PlainTransportConnection: redial TLS setup failed:" << e.what();
                finish(nullptr);
                return;
            }
            boost::asio::async_connect(ssl->lowest_layer(), endpoints,
                [self](const boost::system::error_code& ec, const boost::asio::ip::tcp::endpoint&) {
                    if (ec) { self->finish(nullptr); return; }
                    self->ssl->async_handshake(boost::asio::ssl::stream_base::client,
                        [self](const boost::system::error_code& hec) {
                            if (hec) { self->finish(nullptr); return; }
                            auto c = std::make_shared<SslConnection>(
                                std::move(*self->ssl), makeCodec(self->cfg.codec), nullptr);
                            c->start();
                            self->finish(std::move(c));
                        });
                });
            return;
        }
        finish(nullptr);
    }

    void finish(std::shared_ptr<RpcConnectionBase> c)
    {
        std::shared_ptr<RpcConnectionBase> orphan;
        {
            std::lock_guard<std::mutex> g(mu);
            if (done || cancelled) orphan = std::move(c);
            else conn = std::move(c);
            done = true;
        }
        cv.notify_all();
        if (orphan) orphan->stop("redial abandoned");
        if (deadline) deadline->cancel();
    }

    void abortIo()
    {
        boost::system::error_code ignored;
        if (resolver) resolver->cancel();
        if (tcp) tcp->close(ignored);
        if (ssl) ssl->lowest_layer().close(ignored);
        if (deadline) deadline->cancel();
    }

    bool isCancelled() const { std::lock_guard<std::mutex> g(mu); return cancelled; }

    const LogosTransportConfig cfg;
    const std::chrono::steady_clock::time_point startedAt;

    mutable std::mutex                 mu;
    std::condition_variable            cv;
    bool                               done = false;
    bool                               cancelled = false;
    std::shared_ptr<RpcConnectionBase> conn;

    std::shared_ptr<boost::asio::ip::tcp::resolver> resolver;
    std::shared_ptr<TcpStream>                      tcp;
    std::shared_ptr<SslStream>                      ssl;
    std::shared_ptr<boost::asio::steady_timer>      deadline;
};

PlainTransportConnection::PlainTransportConnection(LogosTransportConfig cfg)
    : m_cfg(std::move(cfg))
{
}

PlainTransportConnection::~PlainTransportConnection()
{
    std::shared_ptr<RpcConnectionBase> conn;
    std::shared_ptr<Dial> dial;
    {
        std::lock_guard<std::mutex> g(m_mu);
        conn = std::move(m_conn);
        dial = std::move(m_dial);
    }
    if (dial) dial->cancel();
    if (conn) conn->stop("connection destroyed");
}

bool PlainTransportConnection::connectToHost()
{
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (m_connected) return true;
    }

    auto& ioc = sharedIo();
    auto codec = makeCodec(m_cfg.codec);
    std::shared_ptr<RpcConnectionBase> conn;

    try {
        boost::asio::ip::tcp::resolver resolver(ioc);
        auto endpoints = resolver.resolve(m_cfg.host, std::to_string(m_cfg.port));

        if (m_cfg.protocol == LogosProtocol::Tcp) {
            boost::asio::ip::tcp::socket socket(ioc);
            boost::asio::connect(socket, endpoints);
            auto tcpConn = std::make_shared<TcpConnection>(std::move(socket), codec, nullptr);
            tcpConn->start();
            conn = tcpConn;
        } else if (m_cfg.protocol == LogosProtocol::TcpSsl) {
            auto ctx = buildClientSslCtx(m_cfg);
            SslStream stream(ioc, ctx);
            configureTlsClient(stream, m_cfg);
            boost::asio::connect(stream.lowest_layer(), endpoints);
            stream.handshake(boost::asio::ssl::stream_base::client);
            auto sslConn = std::make_shared<SslConnection>(std::move(stream), codec, nullptr);
            sslConn->start();
            conn = sslConn;
        } else {
            qCritical() << "PlainTransportConnection: unsupported protocol";
            return false;
        }
    } catch (const std::exception& e) {
        qWarning() << "PlainTransportConnection::connectToHost failed:" << e.what();
        return false;
    }

    std::shared_ptr<RpcConnectionBase> previous;
    {
        std::lock_guard<std::mutex> g(m_mu);
        previous = std::exchange(m_conn, conn);
        m_connected = true;
    }
    if (previous) previous->stop("replaced by a new connection");
    return true;
}

bool PlainTransportConnection::isConnected() const
{
    return liveConnection(0, false) != nullptr;
}

bool PlainTransportConnection::reconnect()
{
    std::shared_ptr<RpcConnectionBase> conn;
    std::shared_ptr<Dial> dial;
    {
        std::lock_guard<std::mutex> g(m_mu);
        conn = std::move(m_conn);
        dial = std::move(m_dial);
        m_connected = false;
    }
    if (dial) dial->cancel();
    if (conn) conn->stop("reconnecting");
    return connectToHost();
}

std::shared_ptr<RpcConnectionBase> PlainTransportConnection::liveConnection(int waitMs,
                                                                           bool waitForRunningDial) const
{
    std::shared_ptr<RpcConnectionBase> dead;
    std::shared_ptr<Dial> waitOn;
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (m_conn && m_conn->isOpen()) return m_conn;
        if (m_dial && m_dial->finished()) {
            if (auto fresh = m_dial->take()) {
                dead = std::exchange(m_conn, std::move(fresh));
                m_connected = true;
                m_dial.reset();
            } else if (m_dial->mayRetry()) {
                m_dial.reset();
            }
        }
        if (!(m_conn && m_conn->isOpen())) {
            if (!m_dial) {
                m_dial = std::make_shared<Dial>(m_cfg);
                m_dial->start();
                waitOn = m_dial;
            } else if (waitForRunningDial && !m_dial->finished()) {
                waitOn = m_dial;
            }
        }
    }
    if (dead) dead->stop("replaced after the peer went away");
    // A second pass adopts what the wait produced; the finished dial cannot start another.
    if (waitOn && waitMs > 0 && waitOn->waitFinished(waitMs))
        return liveConnection(0, false);
    std::lock_guard<std::mutex> g(m_mu);
    return (m_conn && m_conn->isOpen()) ? m_conn : nullptr;
}

LogosObject* PlainTransportConnection::requestObject(const QString& objectName, int timeoutMs)
{
    // A call waits on the redial within its own budget; the retry tick (timeout 0) barely waits.
    const int waitMs = timeoutMs > 0
        ? std::min<int>(timeoutMs, static_cast<int>(kDialDeadline.count()))
        : kFreshDialWaitMs;
    auto conn = liveConnection(waitMs, timeoutMs > 0);
    if (!conn) return nullptr;
    return new PlainLogosObject(objectName.toStdString(), conn);
}

QString PlainTransportConnection::endpointUrl(const QString& /*instanceId*/,
                                              const QString& /*moduleName*/)
{
    return QString("tcp://%1:%2")
        .arg(QString::fromStdString(m_cfg.host))
        .arg(m_cfg.port);
}

} // namespace logos::plain
