#include "plain_transport_host.h"

#include "cbor_codec.h"
#include "io_context_pool.h"
#include "json_codec.h"
#include "qvariant_rpc_value.h"

#include "../../module_proxy.h"

#include <QDebug>
#include <QMetaObject>

#include <atomic>
#include <chrono>
#include <future>

#include <boost/asio/post.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/version.hpp>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/opensslv.h>
#include <openssl/x509.h>
#include <openssl/evp.h>

namespace logos::plain {

namespace {

std::shared_ptr<IWireCodec> makeCodec(LogosWireCodec kind)
{
    switch (kind) {
    case LogosWireCodec::Cbor: return std::make_shared<CborCodec>();
    case LogosWireCodec::Json:
    default:                   return std::make_shared<JsonCodec>();
    }
}

// Print the last OpenSSL error to qWarning, clearing the error stack.
// Used after any SSL_CTX_set_* call that returned 0 — silent failures
// were how we missed the cipher-list misconfig for multiple rounds.
void dumpSslErrors(const char* where)
{
    unsigned long e;
    while ((e = ERR_get_error()) != 0) {
        char buf[256];
        ERR_error_string_n(e, buf, sizeof(buf));
        qWarning() << "buildSslCtx/" << where << ":" << buf;
    }
}

boost::asio::ssl::context buildSslCtx(const LogosTransportConfig& cfg, bool server)
{
    // One-time diagnostic: print Boost + OpenSSL build-vs-runtime
    // versions on first call. We've spent multiple rounds on
    // TLS configuration that *appeared* to take effect (strings
    // baked into the binary) but didn't change runtime behaviour;
    // a version mismatch between compile-time headers and runtime
    // libs would do that, and this prints the smoking gun.
    static bool versionsLogged = false;
    if (!versionsLogged) {
        versionsLogged = true;
        qInfo().nospace()
            << "buildSslCtx versions: "
            << "Boost build=" << BOOST_VERSION
            << " (" << BOOST_LIB_VERSION << "), "
            << "OpenSSL build=0x" << Qt::hex << OPENSSL_VERSION_NUMBER
            << Qt::dec << " (" << OPENSSL_VERSION_TEXT << "), "
            << "runtime=" << OpenSSL_version(OPENSSL_VERSION);
    }

    qInfo().nospace() << "buildSslCtx: cfg.certFile='"
        << QString::fromStdString(cfg.certFile)
        << "' cfg.keyFile='"
        << QString::fromStdString(cfg.keyFile)
        << "' cfg.caFile='"
        << QString::fromStdString(cfg.caFile)
        << "' role=" << (server ? "server" : "client");

    boost::asio::ssl::context ctx(server
        ? boost::asio::ssl::context::tls_server
        : boost::asio::ssl::context::tls_client);
    ctx.set_options(boost::asio::ssl::context::default_workarounds
                    | boost::asio::ssl::context::no_sslv2
                    | boost::asio::ssl::context::no_sslv3
                    | boost::asio::ssl::context::single_dh_use);

    // Require TLS 1.2+. Check return values on every set_* below and
    // dump any pending OpenSSL errors — previous silent-fail behaviour
    // was the root cause of multiple debugging rounds landing no fix.
    if (!SSL_CTX_set_min_proto_version(ctx.native_handle(), TLS1_2_VERSION)) {
        qWarning() << "buildSslCtx: SSL_CTX_set_min_proto_version(TLS1_2) failed";
        dumpSslErrors("set_min_proto_version");
    }
    if (!SSL_CTX_set_max_proto_version(ctx.native_handle(), TLS1_3_VERSION)) {
        qWarning() << "buildSslCtx: SSL_CTX_set_max_proto_version(TLS1_3) failed";
        dumpSslErrors("set_max_proto_version");
    }
    if (!SSL_CTX_set1_groups_list(ctx.native_handle(),
                                  "X25519:P-256:P-384:P-521")) {
        qWarning() << "buildSslCtx: SSL_CTX_set1_groups_list failed";
        dumpSslErrors("set1_groups_list");
    }
    if (!SSL_CTX_set_ciphersuites(ctx.native_handle(),
            "TLS_AES_128_GCM_SHA256:"
            "TLS_AES_256_GCM_SHA384:"
            "TLS_CHACHA20_POLY1305_SHA256")) {
        qWarning() << "buildSslCtx: SSL_CTX_set_ciphersuites failed";
        dumpSslErrors("set_ciphersuites");
    }
    if (!SSL_CTX_set_cipher_list(ctx.native_handle(),
            "ECDHE+AESGCM:ECDHE+CHACHA20:"
            "DHE+AESGCM:DHE+CHACHA20:"
            "!aNULL:!MD5:!DSS:!RC4:!3DES")) {
        qWarning() << "buildSslCtx: SSL_CTX_set_cipher_list failed";
        dumpSslErrors("set_cipher_list");
    }

    // Log what stuck. If min/max_proto read back as 0, the platform
    // doesn't support bounded proto versions (very old OpenSSL) and
    // nothing we did above will have capped anything. Cipher count is
    // the second-most-likely silent failure: a non-zero count from
    // SSL_CTX_get_ciphers means the TLS 1.2 cipher list got applied.
    {
        auto* sk = SSL_CTX_get_ciphers(ctx.native_handle());
        const int n = sk ? sk_SSL_CIPHER_num(sk) : 0;
        QString first;
        if (n > 0) {
            const SSL_CIPHER* c = sk_SSL_CIPHER_value(sk, 0);
            first = QString::fromLatin1(SSL_CIPHER_get_name(c));
        }
        qInfo().nospace() << "buildSslCtx: role="
            << (server ? "server" : "client")
            << " min_proto=0x" << Qt::hex
            << SSL_CTX_get_min_proto_version(ctx.native_handle())
            << " max_proto=0x"
            << SSL_CTX_get_max_proto_version(ctx.native_handle())
            << " options=0x"
            << SSL_CTX_get_options(ctx.native_handle())
            << Qt::dec
            << " cipher_count=" << n
            << " first_cipher=" << first;
    }

    if (!cfg.certFile.empty()) {
        ctx.use_certificate_chain_file(cfg.certFile);
        dumpSslErrors("use_certificate_chain_file");
    }
    if (!cfg.keyFile.empty()) {
        ctx.use_private_key_file(cfg.keyFile, boost::asio::ssl::context::pem);
        dumpSslErrors("use_private_key_file");
    }
    if (!cfg.caFile.empty()) {
        ctx.load_verify_file(cfg.caFile);
        dumpSslErrors("load_verify_file");
    }
    if (cfg.verifyPeer && !server) {
        ctx.set_verify_mode(boost::asio::ssl::verify_peer);
    } else if (!server) {
        ctx.set_verify_mode(boost::asio::ssl::verify_none);
    }

    // Final check: is a cert + matching key actually attached to the
    // SSL_CTX? "no shared cipher" / "unsupported protocol" can both
    // result from a server that has no usable cert at all (no PKI
    // cipher suites can negotiate without one). use_certificate_*
    // / use_private_key_* throw on outright failure but can leave the
    // ctx in a "loaded but the CTX-level slot is empty" state if the
    // file's first PEM block was something other than a CERTIFICATE.
    {
        X509* serverCert = SSL_CTX_get0_certificate(ctx.native_handle());
        EVP_PKEY* serverKey = SSL_CTX_get0_privatekey(ctx.native_handle());
        const int checkOk = SSL_CTX_check_private_key(ctx.native_handle());
        qInfo().nospace() << "buildSslCtx: cert_attached="
            << (serverCert ? "yes" : "no")
            << " key_attached=" << (serverKey ? "yes" : "no")
            << " check_private_key=" << checkOk;
        if (!checkOk) dumpSslErrors("SSL_CTX_check_private_key");
    }
    return ctx;
}

} // anonymous namespace

PlainTransportHost::PlainTransportHost(LogosTransportConfig cfg)
    : m_cfg(std::move(cfg))
{
}

PlainTransportHost::~PlainTransportHost()
{
    // The two stop() calls below are blocking: they tear down all
    // open RpcConnection sessions, each of which calls
    // IncomingCallHandler::onConnectionClosed() — which re-acquires
    // `m_mu` to remove its publisher mapping. Holding m_mu across
    // stop() therefore self-deadlocks.
    //
    // Move the published map and the listeners out under the lock,
    // then drop it before driving the shutdowns. Once we've moved
    // them, no other thread can reach this object's data through
    // m_published / m_tcp / m_ssl.
    decltype(m_published) published;
    decltype(m_tcp) tcp;
    decltype(m_ssl) ssl;
    {
        std::lock_guard<std::mutex> g(m_mu);
        published = std::move(m_published);
        tcp = std::move(m_tcp);
        ssl = std::move(m_ssl);
    }
    for (auto& [name, pub] : published) {
        QObject::disconnect(pub.eventConn);
    }
    if (tcp) tcp->stop();
    if (ssl) ssl->stop();

    // Quiesce the I/O thread before this host (an IncomingCallHandler) is
    // destroyed. Server-side RpcConnections hold a RAW `IncomingCallHandler*`
    // back to us; a read completion racing this teardown runs
    // RpcConnection::fail() on the I/O thread, which calls
    // m_handler->onConnectionClosed(this). stop() above closes the sockets but
    // does NOT wait for an already-executing fail() — so without this barrier
    // the handler can be freed mid-call (a use-after-free that surfaced as a
    // flaky SIGSEGV, including on macOS CI in CallErrorAfterAcquireTest right
    // after a preceding live-host test tore its PlainTransportHost down).
    // There is a single shared I/O thread, so a task posted now runs only after
    // every in-flight/queued connection handler has completed; blocking on it
    // guarantees no callback still references this host. Skip when we're ON
    // the I/O thread (the in-flight handler is our own caller) to avoid
    // self-deadlock.
    if (tcp || ssl) {
        auto& ioc = IoContextPool::shared().ioContext();
        if (!ioc.get_executor().running_in_this_thread()) {
            // The promise is shared, NOT captured by reference. The wait below is
            // bounded, so on timeout this frame returns while the posted task is
            // still queued — a by-reference capture would then set_value() on a
            // destroyed stack object, which is the very failure mode this barrier
            // exists to prevent.
            auto drained = std::make_shared<std::promise<void>>();
            auto fut = drained->get_future();
            boost::asio::post(ioc, [drained] { drained->set_value(); });
            // Bounded so a wedged I/O thread cannot hang teardown — but a timeout
            // means the barrier did NOT hold and we are about to free an
            // IncomingCallHandler a connection may still call back into. Say so:
            // silently proceeding is how this class of crash stays unexplained.
            if (fut.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
                qWarning() << "PlainTransportHost: I/O drain timed out after 5s;"
                           << "tearing down anyway — a connection callback may still"
                           << "reference this host (see the barrier comment above)";
            }
        }
    }
}

bool PlainTransportHost::start()
{
    std::lock_guard<std::mutex> g(m_mu);
    if (m_started) return true;
    auto codec = makeCodec(m_cfg.codec);
    auto& ioc  = IoContextPool::shared().ioContext();

    if (m_cfg.protocol == LogosProtocol::Tcp) {
        m_tcp = std::make_shared<RpcServerTcp>(ioc, m_cfg.host, m_cfg.port, codec, this);
        if (!m_tcp->start()) {
            qCritical() << "PlainTransportHost: TCP bind failed on"
                        << QString::fromStdString(m_cfg.host) << m_cfg.port;
            m_tcp.reset();
            return false;
        }
        m_boundPort = m_tcp->boundPort();
    } else if (m_cfg.protocol == LogosProtocol::TcpSsl) {
        try {
            auto ctx = buildSslCtx(m_cfg, /*server=*/true);
            m_ssl = std::make_shared<RpcServerSsl>(ioc, m_cfg.host, m_cfg.port,
                                                   std::move(ctx), codec, this);
            if (!m_ssl->start()) {
                qCritical() << "PlainTransportHost: TLS bind failed";
                m_ssl.reset();
                return false;
            }
            m_boundPort = m_ssl->boundPort();
        } catch (const std::exception& e) {
            qCritical() << "PlainTransportHost: SSL context setup failed:" << e.what();
            return false;
        }
    } else {
        qCritical() << "PlainTransportHost: unsupported protocol";
        return false;
    }
    m_started = true;
    return true;
}

QString PlainTransportHost::endpoint() const
{
    std::lock_guard<std::mutex> g(m_mu);
    if (m_boundPort == 0) return QString();
    return QString("tcp://%1:%2")
        .arg(QString::fromStdString(m_cfg.host))
        .arg(m_boundPort);
}

QString PlainTransportHost::bindUrl(const QString& /*instanceId*/,
                                    const QString& /*moduleName*/)
{
    // One PlainTransportHost listens on a single host:port and serves every
    // published module over the same socket; URL is independent of module.
    return endpoint();
}

bool PlainTransportHost::publishObject(const QString& name, QObject* object)
{
    if (!object) return false;
    auto* proxy = qobject_cast<ModuleProxy*>(object);
    if (!proxy) {
        qWarning() << "PlainTransportHost::publishObject: expected ModuleProxy for"
                   << name << "(plain transport only publishes ModuleProxy for now)";
        return false;
    }

    std::lock_guard<std::mutex> g(m_mu);
    Published pub;
    pub.object = object;
    const std::string stdName = name.toStdString();

    // Hook the QObject's eventResponse(QString, QVariantList) signal so every
    // Q_INVOKABLE-style event emission fans out to subscribed connections.
    pub.eventConn = QObject::connect(proxy, &ModuleProxy::eventResponse,
        [this, stdName](const QString& eventName, const QVariantList& data) {
            EventMessage msg;
            msg.object    = stdName;
            msg.eventName = eventName.toStdString();
            msg.data      = qvariantListToRpcList(data);
            fanOutEvent(stdName, std::move(msg));
        });

    m_published[stdName] = std::move(pub);
    return true;
}

void PlainTransportHost::unpublishObject(const QString& name)
{
    std::lock_guard<std::mutex> g(m_mu);
    auto it = m_published.find(name.toStdString());
    if (it == m_published.end()) return;
    QObject::disconnect(it->second.eventConn);
    m_published.erase(it);
}

void PlainTransportHost::fanOutEvent(const std::string& name, EventMessage msg)
{
    std::vector<EventSink> sinks;
    {
        std::lock_guard<std::mutex> g(m_mu);
        auto it = m_published.find(name);
        if (it == m_published.end()) return;
        // Named subscribers + wildcard ("") subscribers get the event.
        for (auto which : {msg.eventName, std::string{}}) {
            auto evtIt = it->second.sinksByEvent.find(which);
            if (evtIt == it->second.sinksByEvent.end()) continue;
            for (auto& [key, sink] : evtIt->second) sinks.push_back(sink);
        }
    }
    for (auto& sink : sinks) {
        try { sink(msg); } catch (...) {}
    }
}

void PlainTransportHost::onCall(const CallMessage& req, CallReply reply)
{
    QObject* obj = nullptr;
    {
        std::lock_guard<std::mutex> g(m_mu);
        auto it = m_published.find(req.object);
        if (it != m_published.end()) obj = it->second.object;
    }
    if (!obj) {
        ResultMessage res; res.id = req.id; res.ok = false;
        res.err = "object not published: " + req.object;
        res.errCode = "MODULE_NOT_LOADED";
        reply(std::move(res));
        return;
    }

    QString   authToken  = QString::fromStdString(req.authToken);
    QString   methodName = QString::fromStdString(req.method);
    QVariantList args    = rpcListToQVariantList(req.args);
    uint64_t id = req.id;

    // The wire this call arrived on, so the provider can enforce local_only
    // tokens. This host only ever serves the remote plain protocols (LocalSocket
    // is handled by RemoteTransportHost). Default to the non-local label so an
    // unexpected protocol fails closed for a local_only token.
    QString transportProtocol = QStringLiteral("tcp");
    if (m_cfg.protocol == LogosProtocol::TcpSsl)
        transportProtocol = QStringLiteral("tcp_ssl");

    QMetaObject::invokeMethod(obj, [obj, authToken, methodName, args, transportProtocol, id, reply]() {
        QVariant ret;
        bool ok = QMetaObject::invokeMethod(obj, "callRemoteMethod",
                                            Qt::DirectConnection,
                                            Q_RETURN_ARG(QVariant, ret),
                                            Q_ARG(QString, authToken),
                                            Q_ARG(QString, methodName),
                                            Q_ARG(QVariantList, args),
                                            Q_ARG(QString, transportProtocol));
        ResultMessage res;
        res.id = id;
        if (ok) {
            res.ok = true;
            res.value = qvariantToRpcValue(ret);
        } else {
            res.ok = false;
            res.err = "callRemoteMethod failed";
            res.errCode = "METHOD_FAILED";
        }
        reply(std::move(res));
    }, Qt::QueuedConnection);
}

void PlainTransportHost::onMethods(const MethodsMessage& req, MethodsReply reply)
{
    QObject* obj = nullptr;
    {
        std::lock_guard<std::mutex> g(m_mu);
        auto it = m_published.find(req.object);
        if (it != m_published.end()) obj = it->second.object;
    }
    if (!obj) {
        MethodsResultMessage res; res.id = req.id; res.ok = false;
        res.err = "object not published";
        reply(std::move(res));
        return;
    }
    uint64_t id = req.id;
    QMetaObject::invokeMethod(obj, [obj, id, reply]() {
        QJsonArray arr;
        QMetaObject::invokeMethod(obj, "getPluginMethods",
                                  Qt::DirectConnection,
                                  Q_RETURN_ARG(QJsonArray, arr));
        MethodsResultMessage res;
        res.id = id;
        res.ok = true;
        res.methods = methodsFromJsonArray(arr);
        reply(std::move(res));
    }, Qt::QueuedConnection);
}

void PlainTransportHost::onSubscribe(const SubscribeMessage& req, EventSink sink,
                                     const void* connectionId)
{
    std::lock_guard<std::mutex> g(m_mu);
    auto it = m_published.find(req.object);
    if (it == m_published.end()) return;
    // Sinks are keyed by the originating connection so that
    // onUnsubscribe / onConnectionClosed can remove only sinks
    // belonging to that connection — sub/unsub frames don't carry a
    // subscriber id on the wire.
    it->second.sinksByEvent[req.eventName][connectionId] = std::move(sink);
}

void PlainTransportHost::onUnsubscribe(const UnsubscribeMessage& req,
                                       const void* connectionId)
{
    std::lock_guard<std::mutex> g(m_mu);
    auto it = m_published.find(req.object);
    if (it == m_published.end()) return;
    auto evtIt = it->second.sinksByEvent.find(req.eventName);
    if (evtIt == it->second.sinksByEvent.end()) return;
    // Only drop the requesting connection's sink — other clients
    // subscribed to the same (object, event) keep theirs. Previously
    // this erased the entire eventName entry, taking every other
    // subscriber down with it.
    evtIt->second.erase(connectionId);
    if (evtIt->second.empty()) it->second.sinksByEvent.erase(evtIt);
}

void PlainTransportHost::onConnectionClosed(const void* connectionId)
{
    std::lock_guard<std::mutex> g(m_mu);
    // Sweep every published object's per-event sink table and drop any
    // entries belonging to the closed connection. Without this, a
    // crashing/disconnecting client (which never sends Unsubscribe)
    // leaves dead sinks in the table forever.
    for (auto& [_name, pub] : m_published) {
        for (auto evtIt = pub.sinksByEvent.begin(); evtIt != pub.sinksByEvent.end(); ) {
            evtIt->second.erase(connectionId);
            if (evtIt->second.empty()) evtIt = pub.sinksByEvent.erase(evtIt);
            else ++evtIt;
        }
    }
}

void PlainTransportHost::onToken(const TokenMessage& req)
{
    QObject* obj = nullptr;
    {
        std::lock_guard<std::mutex> g(m_mu);
        // Route token to the module matching req.moduleName if we host
        // it; otherwise the first published module (matches today's behavior
        // for the single-published-object provider pattern).
        auto it = m_published.find(req.moduleName);
        if (it != m_published.end()) obj = it->second.object;
        else if (!m_published.empty()) obj = m_published.begin()->second.object;
    }
    if (!obj) return;
    QString authToken  = QString::fromStdString(req.authToken);
    QString moduleName = QString::fromStdString(req.moduleName);
    QString token      = QString::fromStdString(req.token);
    QMetaObject::invokeMethod(obj, "informModuleToken",
                              Qt::QueuedConnection,
                              Q_ARG(QString, authToken),
                              Q_ARG(QString, moduleName),
                              Q_ARG(QString, token));
}

} // namespace logos::plain
