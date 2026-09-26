#include "logos_protocol_plain_session.h"

#include "implementations/plain/cbor_codec.h"
#include "implementations/plain/io_context_pool.h"
#include "implementations/plain/json_codec.h"
#include "logos_codec.h"
#include "logos_reserved_events.h"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include <algorithm>
#include <array>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

namespace logos::plain::abi {

using json = nlohmann::json;

namespace {

using Clock = std::chrono::steady_clock;

std::shared_ptr<IWireCodec> sessionCodec(LogosWireCodec codec)
{
    if (codec == LogosWireCodec::Cbor) return std::make_shared<CborCodec>();
    return std::make_shared<JsonCodec>();
}

const char* codecName(LogosWireCodec codec)
{
    return codec == LogosWireCodec::Cbor ? "cbor" : "json";
}

std::string b64url(const unsigned char* data, std::size_t size)
{
    return logos::b64UrlEncode(std::vector<std::uint8_t>(data, data + size));
}

std::string spkiPin(X509* cert)
{
    unsigned char* der = nullptr;
    const int size = i2d_X509_PUBKEY(X509_get_X509_PUBKEY(cert), &der);
    if (size <= 0) return {};
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    const bool ok = EVP_Digest(der, static_cast<std::size_t>(size), hash, &length, EVP_sha256(),
                               nullptr) == 1;
    OPENSSL_free(der);
    return ok ? "sha256:" + b64url(hash, length) : std::string();
}

std::string certDer64(X509* cert)
{
    unsigned char* der = nullptr;
    const int size = i2d_X509(cert, &der);
    if (size <= 0) return {};
    std::string out = b64url(der, static_cast<std::size_t>(size));
    OPENSSL_free(der);
    return out;
}

// Per-connection ex_data: whether an unanchored chain may pass, and the chain it
// verified to when one did.
void freeChain(void*, void* ptr, CRYPTO_EX_DATA*, int, long, void*)
{
    if (ptr) sk_X509_pop_free(static_cast<STACK_OF(X509)*>(ptr), X509_free);
}

int unanchoredIndex()
{
    static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

int selfChainIndex()
{
    static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, &freeChain);
    return index;
}

void allowUnanchored(SSL* ssl)
{
    static int marker = 1;
    SSL_set_ex_data(ssl, unanchoredIndex(), &marker);
}

bool isAnchored(SSL* ssl) { return SSL_get_ex_data(ssl, selfChainIndex()) == nullptr; }

// The anchors first. Where the connection allows it, a chain that fails them
// may still pass when it is self-consistent: the leaf and, last, the
// self-signed CA that issued it, checked for the same purpose.
int verifyPresentedChain(X509_STORE_CTX* ctx, void*)
{
    if (X509_verify_cert(ctx) == 1) return 1;
    auto* ssl = static_cast<SSL*>(
        X509_STORE_CTX_get_ex_data(ctx, SSL_get_ex_data_X509_STORE_CTX_idx()));
    if (!ssl || !SSL_get_ex_data(ssl, unanchoredIndex())) return 0;
    X509* leaf = X509_STORE_CTX_get0_cert(ctx);
    STACK_OF(X509)* presented = X509_STORE_CTX_get0_untrusted(ctx);
    const int count = presented ? sk_X509_num(presented) : 0;
    X509* root = count == 2 ? sk_X509_value(presented, 1) : nullptr;
    if (!leaf || !root || root == leaf || X509_self_signed(root, 1) != 1
        || X509_check_ca(root) < 1) {
        X509_STORE_CTX_set_error(ctx, X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY);
        return 0;
    }
    X509_STORE* own = X509_STORE_new();
    X509_STORE_CTX* second = X509_STORE_CTX_new();
    int ok = 0;
    if (own && second && X509_STORE_add_cert(own, root) == 1
        && X509_STORE_CTX_init(second, own, leaf, presented) == 1) {
        X509_STORE_CTX_set_flags(second, X509_V_FLAG_X509_STRICT);
        X509_STORE_CTX_set_purpose(second, SSL_is_server(ssl) ? X509_PURPOSE_SSL_CLIENT
                                                               : X509_PURPOSE_SSL_SERVER);
        ok = X509_verify_cert(second) == 1;
        if (ok) SSL_set_ex_data(ssl, selfChainIndex(), X509_STORE_CTX_get1_chain(second));
        X509_STORE_CTX_set_error(ctx, ok ? X509_V_OK : X509_STORE_CTX_get_error(second));
    }
    X509_STORE_CTX_free(second);
    X509_STORE_free(own);
    return ok;
}

// RFC 9266: 32 bytes, label EXPORTER-Channel-Binding, no context.
std::string exporterOf(SSL* ssl)
{
    unsigned char out[32];
    static const char label[] = "EXPORTER-Channel-Binding";
    if (SSL_export_keying_material(ssl, out, sizeof out, label, sizeof label - 1, nullptr, 0, 0)
        != 1)
        return {};
    return b64url(out, sizeof out);
}

json verifiedChain(SSL* ssl)
{
    json chain = json::array();
    auto* verified = static_cast<STACK_OF(X509)*>(SSL_get_ex_data(ssl, selfChainIndex()));
    if (!verified) verified = SSL_get0_verified_chain(ssl);
    for (int i = 0; verified && i < sk_X509_num(verified); ++i)
        chain.push_back(certDer64(sk_X509_value(verified, i)));
    return chain;
}

// A JSON object with no key repeated at any depth; anything else is refused.
std::optional<json> strictObject(const std::string& text)
{
    std::vector<std::set<std::string>> keys;
    bool duplicate = false;
    auto callback = [&](int, json::parse_event_t event, json& parsed) {
        if (event == json::parse_event_t::object_start) keys.emplace_back();
        else if (event == json::parse_event_t::object_end && !keys.empty()) keys.pop_back();
        else if (event == json::parse_event_t::key && !keys.empty()
                 && !keys.back().insert(parsed.get<std::string>()).second)
            duplicate = true;
        return true;
    };
    json value = json::parse(text, callback, false);
    if (duplicate || value.is_discarded() || !value.is_object()) return std::nullopt;
    return value;
}

struct StoreDeleter {
    void operator()(X509_STORE* store) const { X509_STORE_free(store); }
};
using Store = std::shared_ptr<X509_STORE>;

Store storeFromPem(const std::string& pem)
{
    if (pem.empty()) return {};
    Store store(X509_STORE_new(), StoreDeleter());
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    int count = 0;
    while (bio && store) {
        X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        if (!cert) break;
        if (X509_STORE_add_cert(store.get(), cert) == 1) ++count;
        X509_free(cert);
    }
    BIO_free(bio);
    ERR_clear_error();
    if (!count) return {};
    X509_STORE_set_flags(store.get(), X509_V_FLAG_X509_STRICT);
    return store;
}

// TLS 1.3 only, no tickets, no session cache, no early data, and a
// certificate required from the peer.
std::shared_ptr<boost::asio::ssl::context> sessionContext(bool server, const TlsCredential& credential)
{
    auto context = std::make_shared<boost::asio::ssl::context>(
        server ? boost::asio::ssl::context::tls_server : boost::asio::ssl::context::tls_client);
    SSL_CTX* native = context->native_handle();
    if (!SSL_CTX_set_min_proto_version(native, TLS1_3_VERSION)
        || !SSL_CTX_set_max_proto_version(native, TLS1_3_VERSION)
        || !SSL_CTX_set1_groups_list(native, "X25519:P-256")
        || !SSL_CTX_set_ciphersuites(native, "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:"
                                             "TLS_CHACHA20_POLY1305_SHA256"))
        throw std::runtime_error("session TLS policy setup failed");
    SSL_CTX_set_options(native, SSL_OP_NO_TICKET);
    SSL_CTX_set_num_tickets(native, 0);
    SSL_CTX_set_session_cache_mode(native, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_max_early_data(native, 0);
    context->use_certificate_chain(boost::asio::buffer(credential.certChainPem));
    context->use_private_key(boost::asio::buffer(credential.keyPem), boost::asio::ssl::context::pem);
    if (SSL_CTX_check_private_key(native) != 1)
        throw std::runtime_error("the session certificate and key do not match");
    SSL_CTX_set_verify(native, SSL_VERIFY_PEER | (server ? SSL_VERIFY_FAIL_IF_NO_PEER_CERT : 0),
                       nullptr);
    SSL_CTX_set_cert_verify_callback(native, &verifyPresentedChain, nullptr);
    return context;
}

std::vector<std::uint8_t> sessionFrame(MessageType tag, const json& body)
{
    const std::string text = body.dump(-1, ' ', false, json::error_handler_t::replace);
    return encodeFrame(tag, std::vector<std::uint8_t>(text.begin(), text.end()),
                       kPreAuthFrameLimit);
}

std::uint32_t be32(const std::uint8_t* p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8)
           | std::uint32_t(p[3]);
}

std::int64_t intField(const json& object, const char* key, std::int64_t fallback)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_number_integer() ? it->get<std::int64_t>() : fallback;
}

// The negotiated frame limit: the smaller side, never below 64 KiB.
std::uint32_t negotiatedFrame(std::uint32_t mine, std::int64_t theirs)
{
    constexpr std::uint32_t kFloor = 64 * 1024;
    if (theirs < kFloor) return mine;
    return std::min<std::uint32_t>(mine, static_cast<std::uint32_t>(
        std::min<std::int64_t>(theirs, kMaxFrameLength)));
}

} // namespace

bool parseSessionOptions(const std::string& text, SessionOptions& out, std::string& error)
{
    const auto value = strictObject(text.empty() ? "{}" : text);
    if (!value) {
        error = "session options are not a JSON object";
        return false;
    }
    SessionOptions options = out;
    for (const auto& [key, item] : value->items()) {
        if (!item.is_number_integer() || item.get<std::int64_t>() < 0) {
            error = "session option '" + key + "' is not a non-negative integer";
            return false;
        }
        const auto n = item.get<std::int64_t>();
        if (key == "max_frame") options.maxFrame = static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(n, 64 * 1024, kMaxFrameLength));
        else if (key == "handshake_timeout_ms") options.handshakeTimeout = std::chrono::milliseconds(n);
        else if (key == "hello_timeout_ms") options.helloTimeout = std::chrono::milliseconds(n);
        else if (key == "keepalive_ms") options.keepaliveInterval = std::chrono::milliseconds(n);
        else if (key == "keepalive_timeout_ms") options.keepaliveTimeout = std::chrono::milliseconds(n);
        else if (key == "max_lifetime_ms") options.maxLifetime = std::chrono::milliseconds(n);
        else if (key == "max_sessions") options.maxSessions = static_cast<std::size_t>(n);
        else if (key == "max_pending") options.maxPending = static_cast<std::size_t>(n);
        else if (key == "max_pending_per_address") options.maxPendingPerAddress = static_cast<std::size_t>(n);
        else if (key == "max_concurrent_auth") options.maxConcurrentAuth = static_cast<std::size_t>(std::max<std::int64_t>(1, n));
        else if (key == "port_min" || key == "port_max") {
            if (n > 0xFFFF) {
                error = "session option '" + key + "' is not a port";
                return false;
            }
            (key == "port_min" ? options.portMin : options.portMax) = static_cast<std::uint16_t>(n);
        } else {
            error = "unknown session option '" + key + "'";
            return false;
        }
    }
    if ((options.portMin == 0) != (options.portMax == 0) || options.portMin > options.portMax) {
        error = "port_min and port_max form a range or are both absent";
        return false;
    }
    out = options;
    return true;
}

bool validateCredential(const TlsCredential& credential, std::string& error)
{
    if (credential.empty()) {
        error = "the credential needs a certificate chain and a key";
        return false;
    }
    try {
        sessionContext(true, credential);
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

bool isSessionCaller(const json& caller)
{
    if (!caller.is_object()) return false;
    const auto kind = caller.find("kind");
    const auto name = caller.find("name");
    if (kind == caller.end() || !kind->is_string() || name == caller.end() || !name->is_string()
        || name->get<std::string>().empty())
        return false;
    const std::string n = name->get<std::string>();
    if (n == "@runtime" || n == "core" || n == "capability_module") return false;
    if (*kind == "operator") return true;
    if (*kind != "remote") return false;
    const auto peer = caller.find("peer");
    return peer != caller.end() && peer->is_string() && !peer->get<std::string>().empty();
}

bool sessionMatches(const json& filter, const json& caller, const json& meta)
{
    if (!filter.is_object()) return false;
    for (const auto& [key, wanted] : filter.items()) {
        if (key == "generation_below") {
            const auto generation = meta.find("generation");
            if (!wanted.is_number_integer() || generation == meta.end()
                || !generation->is_number_integer()
                || generation->get<std::int64_t>() >= wanted.get<std::int64_t>())
                return false;
            continue;
        }
        const bool inMeta = meta.is_object() && meta.contains(key) && meta[key] == wanted;
        const bool inCaller = caller.is_object() && caller.contains(key) && caller[key] == wanted;
        if (!inMeta && !inCaller) return false;
    }
    return true;
}

// ── Server ──────────────────────────────────────────────────────────────────

namespace {

struct Pending;

struct SessionRecord {
    std::weak_ptr<SslConnection> connection;
    std::string caller;
    json callerDoc;
    json meta;
    Clock::time_point expires;
    std::shared_ptr<boost::asio::steady_timer> timer;
};

} // namespace

struct SessionEndpoint::Impl : std::enable_shared_from_this<SessionEndpoint::Impl> {
    Impl(SessionEndpoint* owner, LogosTransportConfig cfg, SessionOptions opts,
         std::function<std::string()> anchors, Authenticator auth, SessionCall callHandler,
         MethodsHandler methodsHandler, std::function<std::size_t()> capacity,
         GatePlaceSource reserveSource)
        : outer(owner)
        , io(IoContextPool::shared().ioContext())
        , config(std::move(cfg))
        , options(opts)
        , anchorsPem(std::move(anchors))
        , authenticate(std::move(auth))
        , call(std::move(callHandler))
        , methods(std::move(methodsHandler))
        , reserve(std::move(reserveSource))
        , workers(std::move(capacity))
        , authWorkers([n = opts.maxConcurrentAuth] { return n; })
        , acceptor(io)
        , acceptorStrand(boost::asio::make_strand(acceptor.get_executor()))
        , codec(sessionCodec(config.codec))
    {
    }

    Store currentStore()
    {
        const std::string pem = anchorsPem ? anchorsPem() : std::string();
        std::lock_guard<std::mutex> lock(mutex);
        if (pem != anchorsSeen || (!store && !pem.empty())) {
            anchorsSeen = pem;
            store = storeFromPem(pem);
        }
        return store;
    }

    void acceptLoop();
    void admit(boost::asio::ip::tcp::socket socket);
    void finish(const std::shared_ptr<Pending>& pending);
    void onHandshake(const std::shared_ptr<Pending>& pending, const boost::system::error_code& ec);
    void readHello(const std::shared_ptr<Pending>& pending);
    void onHello(const std::shared_ptr<Pending>& pending, json hello);
    void onAuthenticated(const std::shared_ptr<Pending>& pending, const json& hello,
                         const std::string& reply);
    void refuse(const std::shared_ptr<Pending>& pending, const std::string& code);
    void accept(const std::shared_ptr<Pending>& pending, json caller, json meta,
                std::chrono::milliseconds lifetime, std::uint32_t maxFrame);
    void armLifetime(const void* key, std::shared_ptr<boost::asio::steady_timer> timer,
                     Clock::time_point when);

    SessionEndpoint* outer;
    boost::asio::io_context& io;
    LogosTransportConfig config;
    SessionOptions options;
    std::function<std::string()> anchorsPem;
    Authenticator authenticate;
    SessionCall call;
    MethodsHandler methods;
    GatePlaceSource reserve;
    EndpointWorkers workers;
    EndpointWorkers authWorkers;
    SinkTable sinks;
    boost::asio::ip::tcp::acceptor acceptor;
    boost::asio::strand<boost::asio::any_io_executor> acceptorStrand;
    std::shared_ptr<IWireCodec> codec;
    std::atomic<std::uint16_t> boundPort{0};
    std::atomic<bool> admitUnanchored{false};

    std::mutex mutex;
    std::shared_ptr<boost::asio::ssl::context> context;
    std::string anchorsSeen;
    Store store;
    bool stopped = false;
    std::size_t pendingTotal = 0;
    std::map<std::string, std::size_t> pendingByAddress;
    std::set<std::shared_ptr<Pending>> pending;
    std::map<const void*, SessionRecord> sessions;
};

namespace {

struct Pending {
    std::shared_ptr<SessionEndpoint::Impl> impl;
    std::shared_ptr<boost::asio::ssl::context> context;
    std::unique_ptr<SslStream> stream;
    boost::asio::steady_timer deadline;
    std::string address;
    std::string remote;
    std::array<std::uint8_t, 5> header{};
    std::vector<std::uint8_t> body;
    json chain = json::array();
    std::string exporter;
    bool anchored = true;
    std::vector<std::uint8_t> ack;
    bool done = false;

    explicit Pending(boost::asio::io_context& io) : deadline(io) {}
};

} // namespace

void SessionEndpoint::Impl::acceptLoop()
{
    auto self = shared_from_this();
    acceptor.async_accept(boost::asio::bind_executor(acceptorStrand,
        [self](const boost::system::error_code& ec, boost::asio::ip::tcp::socket socket) {
            if (ec) return; // closed by stop()
            self->admit(std::move(socket));
            self->acceptLoop();
        }));
}

void SessionEndpoint::Impl::admit(boost::asio::ip::tcp::socket socket)
{
    boost::system::error_code ec;
    const auto peer = socket.remote_endpoint(ec);
    if (ec) return;
    const std::string address = peer.address().to_string();
    auto pending = std::make_shared<Pending>(io);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopped || !context || pendingTotal >= options.maxPending
            || pendingByAddress[address] >= options.maxPendingPerAddress
            || sessions.size() >= options.maxSessions) {
            socket.close(ec);
            return;
        }
        ++pendingTotal;
        ++pendingByAddress[address];
        pending->context = context;
        this->pending.insert(pending);
    }
    pending->impl = shared_from_this();
    pending->address = address;
    pending->remote = address + ":" + std::to_string(peer.port());
    pending->stream = std::make_unique<SslStream>(std::move(socket), *pending->context);
    SSL* ssl = pending->stream->native_handle();
    const Store trust = currentStore();
    // No anchors: the handshake fails, since nothing can verify.
    if (trust) SSL_set1_verify_cert_store(ssl, trust.get());
    SSL_set_purpose(ssl, X509_PURPOSE_SSL_CLIENT);
    if (admitUnanchored.load()) allowUnanchored(ssl);
    pending->deadline.expires_after(options.handshakeTimeout);
    pending->deadline.async_wait([pending](const boost::system::error_code& waited) {
        if (!waited) pending->impl->finish(pending);
    });
    pending->stream->async_handshake(boost::asio::ssl::stream_base::server,
        [pending](const boost::system::error_code& handshake) {
            pending->impl->onHandshake(pending, handshake);
        });
}

void SessionEndpoint::Impl::finish(const std::shared_ptr<Pending>& pending)
{
    if (pending->done) return;
    pending->done = true;
    pending->deadline.cancel();
    if (pending->stream) {
        boost::system::error_code ignore;
        pending->stream->lowest_layer().close(ignore);
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (this->pending.erase(pending)) {
        --pendingTotal;
        if (--pendingByAddress[pending->address] == 0) pendingByAddress.erase(pending->address);
    }
}

void SessionEndpoint::Impl::onHandshake(const std::shared_ptr<Pending>& pending,
                                        const boost::system::error_code& ec)
{
    if (pending->done) return;
    SSL* ssl = pending->stream->native_handle();
    if (ec || SSL_get_verify_result(ssl) != X509_V_OK || !SSL_get0_peer_certificate(ssl)) {
        finish(pending);
        return;
    }
    pending->chain = verifiedChain(ssl);
    pending->exporter = exporterOf(ssl);
    pending->anchored = isAnchored(ssl);
    if (pending->chain.empty() || pending->exporter.empty()) {
        finish(pending);
        return;
    }
    pending->deadline.expires_after(options.helloTimeout);
    pending->deadline.async_wait([pending](const boost::system::error_code& waited) {
        if (!waited) pending->impl->finish(pending);
    });
    readHello(pending);
}

void SessionEndpoint::Impl::readHello(const std::shared_ptr<Pending>& pending)
{
    boost::asio::async_read(*pending->stream, boost::asio::buffer(pending->header),
        [pending](const boost::system::error_code& ec, std::size_t) {
            auto& impl = *pending->impl;
            if (pending->done) return;
            const std::uint32_t length = be32(pending->header.data());
            const auto tag = static_cast<MessageType>(pending->header[4]);
            if (ec || tag != MessageType::Hello || length < 3 || length > kPreAuthFrameLimit) {
                impl.finish(pending);
                return;
            }
            pending->body.resize(length - 1);
            boost::asio::async_read(*pending->stream, boost::asio::buffer(pending->body),
                [pending](const boost::system::error_code& bodyError, std::size_t) {
                    auto& impl = *pending->impl;
                    if (pending->done) return;
                    const auto hello = bodyError ? std::nullopt
                        : strictObject(std::string(pending->body.begin(), pending->body.end()));
                    if (!hello) {
                        impl.finish(pending);
                        return;
                    }
                    impl.onHello(pending, *hello);
                });
        });
}

void SessionEndpoint::Impl::onHello(const std::shared_ptr<Pending>& pending, json hello)
{
    if (intField(hello, "wire", 0) != kSessionWireVersion) {
        refuse(pending, "WIRE_VERSION");
        return;
    }
    const auto codecIt = hello.find("codec");
    const std::string theirCodec = codecIt != hello.end() && codecIt->is_string()
        ? codecIt->get<std::string>() : std::string("json");
    if (theirCodec != codecName(config.codec)) {
        refuse(pending, "CODEC");
        return;
    }
    json request = {{"hello", hello},
                    {"peer_chain", pending->chain},
                    {"exporter", pending->exporter},
                    {"anchored", pending->anchored},
                    {"remote", pending->remote},
                    {"protocol", "tls_tcp"},
                    {"port", boundPort.load()}};
    const std::string text = request.dump(-1, ' ', false, json::error_handler_t::replace);
    authWorkers.business([pending, hello = std::move(hello), text] {
        std::string reply;
        try {
            if (pending->impl->authenticate) reply = pending->impl->authenticate(text);
        } catch (...) {
            reply.clear();
        }
        boost::asio::post(pending->impl->io, [pending, hello, reply] {
            pending->impl->onAuthenticated(pending, hello, reply);
        });
    });
}

void SessionEndpoint::Impl::onAuthenticated(const std::shared_ptr<Pending>& pending,
                                            const json& hello, const std::string& reply)
{
    if (pending->done) return;
    const auto answer = strictObject(reply);
    if (!answer || answer->contains("error") || !answer->contains("caller")
        || !isSessionCaller((*answer)["caller"])) {
        refuse(pending, "NOT_AUTHORISED");
        return;
    }
    const std::int64_t requested = intField(*answer, "lifetime_ms", 0);
    if (requested <= 0) {
        refuse(pending, "NOT_AUTHORISED");
        return;
    }
    const auto lifetime = std::min(std::chrono::milliseconds(requested), options.maxLifetime);
    json meta = answer->contains("session") && (*answer)["session"].is_object()
        ? (*answer)["session"] : json::object();
    const std::uint32_t maxFrame = negotiatedFrame(options.maxFrame, intField(hello, "max_frame", 0));
    pending->ack = sessionFrame(MessageType::HelloAck,
        json{{"wire", kSessionWireVersion},
             {"ok", true},
             {"max_frame", maxFrame},
             {"lifetime_ms", lifetime.count()},
             {"keepalive_ms", options.keepaliveInterval.count()}});
    json caller = (*answer)["caller"];
    boost::asio::async_write(*pending->stream, boost::asio::buffer(pending->ack),
        [pending, caller, meta, lifetime, maxFrame](const boost::system::error_code& ec,
                                                    std::size_t) {
            if (ec) {
                pending->impl->finish(pending);
                return;
            }
            pending->impl->accept(pending, caller, meta, lifetime, maxFrame);
        });
}

void SessionEndpoint::Impl::refuse(const std::shared_ptr<Pending>& pending, const std::string& code)
{
    if (pending->done) return;
    pending->ack = sessionFrame(MessageType::HelloAck,
        json{{"wire", kSessionWireVersion}, {"ok", false}, {"error", code}});
    boost::asio::async_write(*pending->stream, boost::asio::buffer(pending->ack),
        [pending](const boost::system::error_code&, std::size_t) {
            pending->impl->finish(pending);
        });
}

void SessionEndpoint::Impl::accept(const std::shared_ptr<Pending>& pending, json caller, json meta,
                                   std::chrono::milliseconds lifetime, std::uint32_t maxFrame)
{
    if (pending->done) return;
    std::shared_ptr<SslConnection> connection;
    auto timer = std::make_shared<boost::asio::steady_timer>(io);
    const Clock::time_point expires = Clock::now() + lifetime;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopped || sessions.size() >= options.maxSessions) {
            // Counted as pending until finish() below.
        } else {
            connection = std::make_shared<SslConnection>(std::move(*pending->stream), codec, outer);
            SessionRecord record;
            record.connection = connection;
            record.callerDoc = caller;
            record.caller = caller.dump(-1, ' ', false, json::error_handler_t::replace);
            record.meta = std::move(meta);
            record.expires = expires;
            record.timer = timer;
            sessions[static_cast<const void*>(connection.get())] = std::move(record);
        }
    }
    if (!connection) {
        finish(pending);
        return;
    }
    pending->stream.reset();
    finish(pending);
    connection->setMaxFrameLength(maxFrame);
    connection->enableKeepalive(options.keepaliveInterval, options.keepaliveTimeout);
    connection->start();
    armLifetime(connection.get(), timer, expires);
}

void SessionEndpoint::Impl::armLifetime(const void* key,
                                        std::shared_ptr<boost::asio::steady_timer> timer,
                                        Clock::time_point when)
{
    auto self = shared_from_this();
    timer->expires_at(when);
    timer->async_wait([self, key, timer](const boost::system::error_code& ec) {
        if (ec) return;
        std::shared_ptr<SslConnection> connection;
        Clock::time_point expires;
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            const auto found = self->sessions.find(key);
            if (found == self->sessions.end() || found->second.timer != timer) return;
            expires = found->second.expires;
            connection = found->second.connection.lock();
        }
        if (expires <= Clock::now()) {
            if (connection) connection->stop("session expired");
            return;
        }
        self->armLifetime(key, timer, expires);
    });
}

SessionEndpoint::SessionEndpoint(LogosTransportConfig config, SessionOptions options,
                                 TlsCredential credential, std::function<std::string()> anchorsPem,
                                 Authenticator authenticate, SessionCall call,
                                 MethodsHandler methods, std::function<std::size_t()> capacity,
                                 GatePlaceSource reserve)
    : impl_(std::make_shared<Impl>(this, std::move(config), options, std::move(anchorsPem),
                                   std::move(authenticate), std::move(call), std::move(methods),
                                   std::move(capacity), std::move(reserve)))
{
    if (!credential.empty()) impl_->context = sessionContext(true, credential);
}

SessionEndpoint::~SessionEndpoint() { stop(); }

bool SessionEndpoint::replaceCredential(TlsCredential credential, std::string& error)
{
    try {
        auto context = sessionContext(true, credential);
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->context = std::move(context);
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

void SessionEndpoint::setUnanchoredAdmission(bool enabled)
{
    impl_->admitUnanchored = enabled;
}

bool SessionEndpoint::start(std::string& error)
{
    auto& impl = *impl_;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.context) {
            error = "tls_tcp needs a TLS credential (lp_provider_set_tls_credential)";
            return false;
        }
    }
    boost::system::error_code ec;
    const auto address = boost::asio::ip::make_address(impl.config.host, ec);
    if (ec) {
        error = "tls_tcp host is not an IP address: " + impl.config.host;
        return false;
    }
    std::vector<std::uint16_t> ports;
    if (impl.options.portMin) {
        for (std::uint32_t p = impl.options.portMin; p <= impl.options.portMax; ++p)
            ports.push_back(static_cast<std::uint16_t>(p));
    } else {
        ports.push_back(impl.config.port);
    }
    for (const std::uint16_t port : ports) {
        boost::asio::ip::tcp::endpoint endpoint(address, port);
        impl.acceptor.open(endpoint.protocol(), ec);
        if (ec) break;
        impl.acceptor.set_option(boost::asio::socket_base::reuse_address(true), ec);
        impl.acceptor.bind(endpoint, ec);
        if (!ec) impl.acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
        if (!ec) {
            impl.boundPort = impl.acceptor.local_endpoint().port();
            auto self = impl_;
            boost::asio::dispatch(impl.acceptorStrand, [self] { self->acceptLoop(); });
            return true;
        }
        boost::system::error_code ignore;
        impl.acceptor.close(ignore);
    }
    error = "tls_tcp could not listen: " + ec.message();
    return false;
}

void SessionEndpoint::stop()
{
    auto impl = impl_;
    std::vector<std::shared_ptr<SslConnection>> connections;
    std::vector<std::shared_ptr<Pending>> pending;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (impl->stopped) return;
        impl->stopped = true;
        for (auto& [key, record] : impl->sessions)
            if (auto connection = record.connection.lock()) connections.push_back(connection);
        pending.assign(impl->pending.begin(), impl->pending.end());
    }
    auto& io = impl->io;
    const bool onIo = io.get_executor().running_in_this_thread();
    auto runOnIo = [&](std::function<void()> job) {
        if (onIo) {
            job();
            return;
        }
        auto done = std::make_shared<std::promise<void>>();
        auto waited = done->get_future();
        boost::asio::post(io, [job = std::move(job), done] {
            job();
            done->set_value();
        });
        waited.wait();
    };
    runOnIo([impl] {
        boost::system::error_code ignore;
        impl->acceptor.close(ignore);
    });
    runOnIo([impl, pending] {
        for (const auto& p : pending) impl->finish(p);
    });
    for (const auto& connection : connections) connection->stop("endpoint stopped");
    // Handlers already on the I/O thread finish before the callbacks go.
    runOnIo([] {});
    impl->workers.stop();
    impl->authWorkers.stop();
    impl->sinks.clear();
    std::lock_guard<std::mutex> lock(impl->mutex);
    for (auto& [key, record] : impl->sessions) {
        auto timer = record.timer;
        boost::asio::post(io, [timer] { timer->cancel(); });
    }
    impl->sessions.clear();
}

void SessionEndpoint::emit(const std::string& object, const std::string& event,
                           const std::vector<RpcValue>& data)
{
    // A session never carries the completion channel or another reserved event.
    if (logos::isReservedEventName(event)) return;
    impl_->sinks.emit(object, event, data);
}

LogosTransportConfig SessionEndpoint::bound() const
{
    LogosTransportConfig out = impl_->config;
    out.port = impl_->boundPort.load();
    return out;
}

int SessionEndpoint::closeSessions(const json& filter, const std::string& reason)
{
    std::vector<std::shared_ptr<SslConnection>> matched;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (auto& [key, record] : impl_->sessions)
            if (sessionMatches(filter, record.callerDoc, record.meta))
                if (auto connection = record.connection.lock()) matched.push_back(connection);
    }
    for (const auto& connection : matched) connection->stop(reason);
    return static_cast<int>(matched.size());
}

int SessionEndpoint::extendSessions(const json& filter, std::chrono::milliseconds lifetime)
{
    const auto limit = std::min(lifetime, impl_->options.maxLifetime);
    const auto expires = Clock::now() + limit;
    int count = 0;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& [key, record] : impl_->sessions) {
        if (!sessionMatches(filter, record.callerDoc, record.meta)) continue;
        // The timer re-arms itself when it wakes before the new expiry.
        record.expires = std::max(record.expires, expires);
        ++count;
    }
    return count;
}

namespace {

ResultMessage refusal(const CallMessage& request, const std::string& why)
{
    ResultMessage reply;
    reply.id = request.id;
    reply.err = why;
    reply.errCode = "UNAUTHORIZED";
    return reply;
}

} // namespace

void SessionEndpoint::onCall(const CallMessage& request, CallReply reply)
{
    reply(refusal(request, "a session call must name its connection"));
}

void SessionEndpoint::onCall(const CallMessage& request, CallReply reply, const void* connectionId)
{
    std::string caller;
    bool mayCall = true;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto found = impl_->sessions.find(connectionId);
        if (found == impl_->sessions.end()) {
            reply(refusal(request, "no session on this connection"));
            return;
        }
        caller = found->second.caller;
        const json& meta = found->second.meta;
        mayCall = !(meta.is_object() && meta.contains("calls") && meta["calls"] == false);
    }
    // Tokens never cross a session: a remote peer manages no local credential.
    if (request.method == "informModuleToken" || request.method == "revokeModuleToken") {
        reply(refusal(request, "tokens are not accepted over a session"));
        return;
    }
    const bool control = isControlMethod(request.method);
    // Bound with "calls": false, a session may introspect and subscribe only.
    if (!mayCall && !control) {
        reply(refusal(request, "this session may not call methods"));
        return;
    }
    auto impl = impl_;
    std::shared_ptr<GatePlace> place = !control && impl->reserve ? impl->reserve() : nullptr;
    auto job = [impl, request, reply = std::move(reply), caller, place = std::move(place)]() mutable {
        try {
            reply(impl->call(request, caller, std::move(place)));
        } catch (const std::exception& ex) {
            ResultMessage failure;
            failure.id = request.id;
            failure.err = ex.what();
            failure.errCode = "METHOD_FAILED";
            reply(std::move(failure));
        }
    };
    if (control) impl_->workers.control(std::move(job));
    else impl_->workers.business(std::move(job));
}

void SessionEndpoint::onMethods(const MethodsMessage& request, MethodsReply reply)
{
    MethodsResultMessage failure;
    failure.id = request.id;
    failure.err = "a session request must name its connection";
    reply(std::move(failure));
}

void SessionEndpoint::onMethods(const MethodsMessage& request, MethodsReply reply,
                                const void* connectionId)
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->sessions.count(connectionId)) {
            MethodsResultMessage failure;
            failure.id = request.id;
            failure.err = "no session on this connection";
            reply(std::move(failure));
            return;
        }
    }
    auto impl = impl_;
    impl_->workers.control([impl, request, reply = std::move(reply)] {
        try {
            reply(impl->methods(request));
        } catch (const std::exception& ex) {
            MethodsResultMessage failure;
            failure.id = request.id;
            failure.err = ex.what();
            reply(std::move(failure));
        }
    });
}

void SessionEndpoint::onSubscribe(const SubscribeMessage& request, EventSink sink,
                                  const void* connectionId)
{
    if (logos::isReservedEventName(request.eventName)) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->sessions.count(connectionId)) return;
    }
    impl_->sinks.subscribe(request.object, request.eventName, connectionId, std::move(sink));
}

void SessionEndpoint::onUnsubscribe(const UnsubscribeMessage& request, const void* connectionId)
{
    impl_->sinks.unsubscribe(request.object, request.eventName, connectionId);
}

void SessionEndpoint::onConnectionClosed(const void* connectionId)
{
    impl_->sinks.dropConnection(connectionId);
    std::shared_ptr<boost::asio::steady_timer> timer;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto found = impl_->sessions.find(connectionId);
        if (found == impl_->sessions.end()) return;
        timer = found->second.timer;
        impl_->sessions.erase(found);
    }
    if (timer) boost::asio::post(impl_->io, [timer] { timer->cancel(); });
}

void SessionEndpoint::onToken(const TokenMessage&) {}

void SessionEndpoint::onToken(const TokenMessage&, const void* connectionId)
{
    // A token push over a session is a protocol violation: the connection goes.
    std::shared_ptr<SslConnection> connection;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto found = impl_->sessions.find(connectionId);
        if (found != impl_->sessions.end()) connection = found->second.connection.lock();
    }
    if (connection) connection->stop("token messages are refused over a session");
}

// ── Client ──────────────────────────────────────────────────────────────────

namespace {

struct Handshake {
    json chain;
    std::string exporter;
    std::string error;
    bool anchored = true;
};

struct Acknowledgement {
    json ack;
    std::string error;
};

class ClientAttempt : public std::enable_shared_from_this<ClientAttempt> {
public:
    // An empty pin means an unanchored dial: any self-consistent chain passes
    // and the hello hook decides.
    ClientAttempt(boost::asio::io_context& io, std::shared_ptr<boost::asio::ssl::context> context,
                  Store store, std::string host, std::string port, std::string pin)
        : io_(io), resolver_(io), context_(std::move(context)), store_(std::move(store)),
          stream_(std::make_unique<SslStream>(io, *context_)), host_(std::move(host)),
          port_(std::move(port)), pin_(std::move(pin))
    {
        SSL* ssl = stream_->native_handle();
        if (store_) SSL_set1_verify_cert_store(ssl, store_.get());
        SSL_set_purpose(ssl, X509_PURPOSE_SSL_SERVER);
        if (pin_.empty()) allowUnanchored(ssl);
        // No SNI: the peer is pinned, and a name would only leak.
    }

    std::future<Handshake> handshake()
    {
        auto future = handshake_.get_future();
        auto self = shared_from_this();
        boost::asio::post(io_, [self] { self->resolve(); });
        return future;
    }

    std::future<Acknowledgement> exchange(std::vector<std::uint8_t> hello)
    {
        auto future = ack_.get_future();
        auto self = shared_from_this();
        boost::asio::post(io_, [self, hello = std::move(hello)]() mutable {
            self->hello_ = std::move(hello);
            if (self->abandoned_) {
                self->finishAck({{}, "connection timed out"});
                return;
            }
            boost::asio::async_write(*self->stream_, boost::asio::buffer(self->hello_),
                [self](const boost::system::error_code& ec, std::size_t) {
                    if (ec) {
                        self->finishAck({{}, ec.message()});
                        return;
                    }
                    self->readAck();
                });
        });
        return future;
    }

    // On the I/O thread: the stream becomes the session's connection.
    std::future<std::shared_ptr<SslConnection>> adopt(std::shared_ptr<IWireCodec> codec,
                                                      std::uint32_t maxFrame,
                                                      std::chrono::milliseconds keepalive,
                                                      std::chrono::milliseconds keepaliveTimeout)
    {
        auto promise = std::make_shared<std::promise<std::shared_ptr<SslConnection>>>();
        auto future = promise->get_future();
        auto self = shared_from_this();
        boost::asio::post(io_, [self, promise, codec, maxFrame, keepalive, keepaliveTimeout] {
            if (self->abandoned_ || !self->stream_) {
                promise->set_value(nullptr);
                return;
            }
            auto connection = std::make_shared<SslConnection>(std::move(*self->stream_), codec, nullptr);
            self->stream_.reset();
            connection->setMaxFrameLength(maxFrame);
            connection->enableKeepalive(keepalive, keepaliveTimeout);
            connection->start();
            promise->set_value(connection);
        });
        return future;
    }

    void abandon()
    {
        auto self = shared_from_this();
        boost::asio::post(io_, [self] {
            self->abandoned_ = true;
            self->resolver_.cancel();
            if (self->stream_) {
                boost::system::error_code ignore;
                self->stream_->lowest_layer().close(ignore);
            }
            self->finishHandshake({{}, {}, "connection timed out"});
            self->finishAck({{}, "connection timed out"});
        });
    }

private:
    void resolve()
    {
        auto self = shared_from_this();
        resolver_.async_resolve(host_, port_,
            [self](const boost::system::error_code& ec,
                   boost::asio::ip::tcp::resolver::results_type addresses) {
                if (ec) {
                    self->finishHandshake({{}, {}, ec.message()});
                    return;
                }
                boost::asio::async_connect(self->stream_->lowest_layer(), addresses,
                    [self](const boost::system::error_code& connectError,
                           const boost::asio::ip::tcp::endpoint&) {
                        if (connectError) {
                            self->finishHandshake({{}, {}, connectError.message()});
                            return;
                        }
                        self->stream_->async_handshake(boost::asio::ssl::stream_base::client,
                            [self](const boost::system::error_code& handshake) {
                                self->onHandshake(handshake);
                            });
                    });
            });
    }

    void onHandshake(const boost::system::error_code& ec)
    {
        if (ec) {
            finishHandshake({{}, {}, "TLS handshake failed: " + ec.message()});
            return;
        }
        SSL* ssl = stream_->native_handle();
        X509* leaf = SSL_get0_peer_certificate(ssl);
        if (SSL_get_verify_result(ssl) != X509_V_OK || !leaf) {
            finishHandshake({{}, {}, "the peer's certificate does not chain to its anchor"});
            return;
        }
        if (!pin_.empty() && spkiPin(leaf) != pin_) {
            finishHandshake({{}, {}, "the peer's key is not the one pinned for it"});
            return;
        }
        finishHandshake({verifiedChain(ssl), exporterOf(ssl), {}, isAnchored(ssl)});
    }

    void readAck()
    {
        auto self = shared_from_this();
        boost::asio::async_read(*stream_, boost::asio::buffer(header_),
            [self](const boost::system::error_code& ec, std::size_t) {
                const std::uint32_t length = be32(self->header_.data());
                if (ec || static_cast<MessageType>(self->header_[4]) != MessageType::HelloAck
                    || length < 3 || length > kPreAuthFrameLimit) {
                    self->finishAck({{}, ec ? ec.message() : "the peer did not acknowledge"});
                    return;
                }
                self->body_.resize(length - 1);
                boost::asio::async_read(*self->stream_, boost::asio::buffer(self->body_),
                    [self](const boost::system::error_code& bodyError, std::size_t) {
                        const auto ack = bodyError ? std::nullopt
                            : strictObject(std::string(self->body_.begin(), self->body_.end()));
                        if (!ack) {
                            self->finishAck({{}, "the peer's acknowledgement is malformed"});
                            return;
                        }
                        self->finishAck({*ack, {}});
                    });
            });
    }

    void finishHandshake(Handshake result)
    {
        if (handshakeDone_) return;
        handshakeDone_ = true;
        handshake_.set_value(std::move(result));
    }

    void finishAck(Acknowledgement result)
    {
        if (ackDone_) return;
        ackDone_ = true;
        ack_.set_value(std::move(result));
    }

    boost::asio::io_context& io_;
    boost::asio::ip::tcp::resolver resolver_;
    std::shared_ptr<boost::asio::ssl::context> context_;
    Store store_;
    std::unique_ptr<SslStream> stream_;
    std::string host_;
    std::string port_;
    std::string pin_;
    std::vector<std::uint8_t> hello_;
    std::array<std::uint8_t, 5> header_{};
    std::vector<std::uint8_t> body_;
    std::promise<Handshake> handshake_;
    std::promise<Acknowledgement> ack_;
    bool handshakeDone_ = false; // I/O thread only
    bool ackDone_ = false;       // I/O thread only
    bool abandoned_ = false;     // I/O thread only
};

template <typename T>
bool waitUntil(std::future<T>& future, Clock::time_point deadline, const std::atomic<bool>* alive)
{
    for (;;) {
        if (alive && !alive->load()) return false;
        const auto next = std::min(deadline, Clock::now() + std::chrono::milliseconds(10));
        if (future.wait_until(next) == std::future_status::ready) return true;
        if (Clock::now() >= deadline) return false;
    }
}

std::optional<json> callHook(const std::function<std::string(const std::string&)>& hook,
                             const json& request, std::string& error)
{
    if (!hook) {
        error = "no session hook is set for this client";
        return std::nullopt;
    }
    std::string text;
    try {
        text = hook(request.dump(-1, ' ', false, json::error_handler_t::replace));
    } catch (...) {
        error = "the session hook failed";
        return std::nullopt;
    }
    auto reply = strictObject(text);
    if (!reply) {
        error = "the session hook's reply is not a JSON object";
        return std::nullopt;
    }
    if (reply->contains("error")) {
        const auto& why = (*reply)["error"];
        error = why.is_string() ? why.get<std::string>() : std::string("refused");
        return std::nullopt;
    }
    return reply;
}

} // namespace

std::shared_ptr<RpcConnectionBase> connectSession(const ClientSessionConfig& config,
                                                  std::chrono::milliseconds timeout,
                                                  std::string& error,
                                                  const std::atomic<bool>* alive)
try {
    const auto deadline = Clock::now() + timeout;
    if (timeout <= std::chrono::milliseconds::zero()) {
        error = "connection timed out";
        return {};
    }
    if (config.credential.empty()) {
        error = "tls_tcp needs a TLS credential (lp_client_set_tls_credential)";
        return {};
    }
    const auto route = callHook(config.dial,
        json{{"target", config.target}, {"timeout_ms", timeout.count()}}, error);
    if (!route) return {};
    std::vector<std::string> addresses;
    if (route->contains("addresses") && (*route)["addresses"].is_array())
        for (const auto& address : (*route)["addresses"])
            if (address.is_string()) addresses.push_back(address.get<std::string>());
    const std::int64_t port = intField(*route, "port", 0);
    const auto pinIt = route->find("server_pin");
    const auto anchorsIt = route->find("anchors");
    const auto unanchoredIt = route->find("unanchored");
    const bool unanchored = unanchoredIt != route->end() && unanchoredIt->is_boolean()
        && unanchoredIt->get<bool>();
    if (addresses.empty() || port <= 0 || port > 0xFFFF) {
        error = "the session hook named no address or port";
        return {};
    }
    if (unanchored ? (pinIt != route->end() || anchorsIt != route->end())
                   : (pinIt == route->end() || !pinIt->is_string()
                      || pinIt->get<std::string>().empty() || anchorsIt == route->end()
                      || !anchorsIt->is_string())) {
        error = unanchored ? "an unanchored dial takes no pin or anchors"
                           : "the session hook named no pin or anchors";
        return {};
    }
    const Store trust = unanchored ? Store() : storeFromPem(anchorsIt->get<std::string>());
    if (!unanchored && !trust) {
        error = "the session hook's anchors hold no certificate";
        return {};
    }
    const std::string pin = unanchored ? std::string() : pinIt->get<std::string>();
    auto context = sessionContext(false, config.credential);
    auto& io = IoContextPool::shared().ioContext();

    std::shared_ptr<ClientAttempt> attempt;
    Handshake handshake;
    for (const auto& address : addresses) {
        auto candidate = std::make_shared<ClientAttempt>(io, context, trust, address,
                                                         std::to_string(port), pin);
        auto future = candidate->handshake();
        if (!waitUntil(future, deadline, alive)) {
            candidate->abandon();
            error = alive && !alive->load() ? "client destroyed" : "connection timed out";
            return {};
        }
        handshake = future.get();
        if (handshake.error.empty()) {
            attempt = candidate;
            break;
        }
        candidate->abandon();
        error = handshake.error;
    }
    if (!attempt) return {};

    auto hello = callHook(config.hello,
        json{{"target", config.target}, {"peer_chain", handshake.chain},
             {"exporter", handshake.exporter}, {"anchored", handshake.anchored}}, error);
    if (!hello) {
        attempt->abandon();
        return {};
    }
    (*hello)["wire"] = kSessionWireVersion;
    (*hello)["max_frame"] = config.options.maxFrame;
    (*hello)["codec"] = codecName(config.codec);
    std::vector<std::uint8_t> frame;
    try {
        frame = sessionFrame(MessageType::Hello, *hello);
    } catch (const std::exception&) {
        attempt->abandon();
        error = "the Hello is too large";
        return {};
    }
    auto acknowledged = attempt->exchange(std::move(frame));
    if (!waitUntil(acknowledged, deadline, alive)) {
        attempt->abandon();
        error = alive && !alive->load() ? "client destroyed" : "connection timed out";
        return {};
    }
    const Acknowledgement ack = acknowledged.get();
    if (!ack.error.empty()) {
        attempt->abandon();
        error = ack.error;
        return {};
    }
    const auto okIt = ack.ack.find("ok");
    if (intField(ack.ack, "wire", 0) != kSessionWireVersion || okIt == ack.ack.end()
        || !okIt->is_boolean() || !okIt->get<bool>()) {
        attempt->abandon();
        const auto why = ack.ack.find("error");
        error = std::string("session refused: ")
            + (why != ack.ack.end() && why->is_string() ? why->get<std::string>() : "NOT_AUTHORISED");
        return {};
    }
    const std::uint32_t maxFrame = negotiatedFrame(config.options.maxFrame,
                                                   intField(ack.ack, "max_frame", 0));
    const auto keepalive = std::chrono::milliseconds(
        std::max<std::int64_t>(1000, intField(ack.ack, "keepalive_ms",
                                              config.options.keepaliveInterval.count())));
    auto adopted = attempt->adopt(sessionCodec(config.codec), maxFrame, keepalive,
                                  std::max(config.options.keepaliveTimeout, keepalive * 3));
    if (!waitUntil(adopted, deadline + std::chrono::seconds(1), alive)) {
        attempt->abandon();
        error = "connection timed out";
        return {};
    }
    auto connection = adopted.get();
    if (!connection) error = "connection timed out";
    return connection;
} catch (const std::exception& ex) {
    error = ex.what();
    return {};
}

} // namespace logos::plain::abi
