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

// A dial gives up after this; a failed redial is not retried sooner than kRedialInterval.
constexpr std::chrono::milliseconds kDialDeadline{5000};
constexpr std::chrono::milliseconds kRedialInterval{250};
// What a zero-timeout caller (the subscription retry tick) waits for a fresh dial: enough for loopback.
constexpr int kFreshDialWaitMs = 20;
// connectToHost() gives up after this even if the deadline has not fired (a busy io thread, a hung DNS lookup).
constexpr std::chrono::milliseconds kConnectWait = kDialDeadline + std::chrono::milliseconds(500);

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

// True inside a handler on the shared io thread, e.g. a user event callback.
bool onIoThread() { return sharedIo().get_executor().running_in_this_thread(); }

// A blocking connect on the calling thread, with no deadline. Only for the io thread.
std::shared_ptr<RpcConnectionBase> connectInline(const LogosTransportConfig& cfg, std::string& why)
{
    auto& ioc = sharedIo();
    try {
        boost::asio::ip::tcp::resolver resolver(ioc);
        const auto endpoints = resolver.resolve(cfg.host, std::to_string(cfg.port));
        if (cfg.protocol == LogosProtocol::Tcp) {
            TcpStream socket(ioc);
            boost::asio::connect(socket, endpoints);
            auto conn = std::make_shared<TcpConnection>(std::move(socket), makeCodec(cfg.codec), nullptr);
            conn->start();
            return conn;
        }
        if (cfg.protocol == LogosProtocol::TcpSsl) {
            auto ctx = buildClientSslCtx(cfg);
            SslStream stream(ioc, ctx);
            configureTlsClient(stream, cfg);
            boost::asio::connect(stream.lowest_layer(), endpoints);
            stream.handshake(boost::asio::ssl::stream_base::client);
            auto conn = std::make_shared<SslConnection>(std::move(stream), makeCodec(cfg.codec), nullptr);
            conn->start();
            return conn;
        }
        why = "unsupported protocol";
    } catch (const std::exception& e) {
        why = e.what();
    }
    return nullptr;
}

} // anonymous namespace

// One connection attempt, run entirely on the io thread. Its handlers own it, so it outlives
// a transport that abandons it; cancel() makes sure nobody inherits what it connected.
struct PlainTransportConnection::Dial : std::enable_shared_from_this<Dial> {
    explicit Dial(LogosTransportConfig c)
        : cfg(std::move(c)), startedAt(std::chrono::steady_clock::now()) {}

    void start()
    {
        auto self = shared_from_this();
        boost::asio::post(sharedIo(), [self] { self->resolve(); });
    }

    // Finishes the attempt now, as a failure; the io thread closes what it opened later.
    void cancel(std::string why = "abandoned")
    {
        std::shared_ptr<RpcConnectionBase> orphan;
        {
            std::lock_guard<std::mutex> g(mu);
            cancelled = true;
            done = true;
            if (error.empty()) error = std::move(why);
            orphan = std::move(conn);
        }
        cv.notify_all();
        if (orphan) orphan->stop("dial abandoned");
        auto self = shared_from_this();
        boost::asio::post(sharedIo(), [self] { self->abortIo(); });
    }

    bool finished() const { std::lock_guard<std::mutex> g(mu); return done; }
    bool failed() const { std::lock_guard<std::mutex> g(mu); return done && !conn; }
    std::string failure() const { std::lock_guard<std::mutex> g(mu); return error; }

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
            if (ec) return;
            self->noteFailure("timed out after " + std::to_string(kDialDeadline.count()) + " ms");
            self->abortIo();
        });
        resolver = std::make_shared<boost::asio::ip::tcp::resolver>(sharedIo());
        resolver->async_resolve(cfg.host, std::to_string(cfg.port),
            [self](const boost::system::error_code& ec,
                   boost::asio::ip::tcp::resolver::results_type endpoints) {
                if (ec || self->isCancelled()) { self->fail("resolve", ec); return; }
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
                    if (ec) { self->fail("connect", ec); return; }
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
                noteFailure(std::string("TLS setup: ") + e.what());
                finish(nullptr);
                return;
            }
            boost::asio::async_connect(ssl->lowest_layer(), endpoints,
                [self](const boost::system::error_code& ec, const boost::asio::ip::tcp::endpoint&) {
                    if (ec) { self->fail("connect", ec); return; }
                    self->ssl->async_handshake(boost::asio::ssl::stream_base::client,
                        [self](const boost::system::error_code& hec) {
                            if (hec) { self->fail("handshake", hec); return; }
                            auto c = std::make_shared<SslConnection>(
                                std::move(*self->ssl), makeCodec(self->cfg.codec), nullptr);
                            c->start();
                            self->finish(std::move(c));
                        });
                });
            return;
        }
        noteFailure("unsupported protocol");
        finish(nullptr);
    }

    // The first reason wins: a deadline or a cancel explains the aborted operation that follows.
    void noteFailure(std::string why)
    {
        std::lock_guard<std::mutex> g(mu);
        if (error.empty()) error = std::move(why);
    }

    void fail(const char* phase, const boost::system::error_code& ec)
    {
        noteFailure(std::string(phase) + ": " + ec.message());
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
        if (orphan) orphan->stop("dial abandoned");
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
    std::string                        error;   // why it produced nothing

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
    // The io thread cannot wait on a Dial it has to run itself, so it connects inline, unbounded as before.
    std::string why;
    auto conn = onIoThread() ? connectInline(m_cfg, why) : awaitDial(why);

    std::shared_ptr<RpcConnectionBase> previous;
    bool connected = false;
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (conn) {
            previous = std::exchange(m_conn, std::move(conn));
            m_connected = true;
        }
        connected = m_connected;
    }
    if (previous) previous->stop("replaced by a new connection");
    if (!connected) qWarning() << "PlainTransportConnection::connectToHost failed:" << why.c_str();
    return connected;
}

std::shared_ptr<RpcConnectionBase> PlainTransportConnection::awaitDial(std::string& why)
{
    std::shared_ptr<Dial> dial;
    {
        std::lock_guard<std::mutex> g(m_mu);
        // A failed dial is replaced at once: this caller paces its own retries.
        if (!m_dial || m_dial->failed()) {
            m_dial = std::make_shared<Dial>(m_cfg);
            m_dial->start();
        }
        dial = m_dial;
    }
    if (!dial->waitFinished(static_cast<int>(kConnectWait.count())))
        dial->cancel("unfinished after " + std::to_string(kConnectWait.count()) + " ms");

    std::lock_guard<std::mutex> g(m_mu);
    auto conn = dial->take();
    if (conn && m_dial == dial) m_dial.reset();
    if (!conn) why = dial->failure();
    return conn;
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
