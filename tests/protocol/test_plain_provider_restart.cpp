// A provider served over plain TCP that dies and comes back on the same port: the consumer must
// report the loss, re-arm on a new connection, and count it as a new establishment.

#include <gtest/gtest.h>

#include "logos_api_client.h"
#include "logos_api_consumer.h"
#include "logos_mode.h"
#include "logos_protocol.h"
#include "logos_provider_interface.h"
#include "logos_subscription_state.h"
#include "logos_transport_config.h"
#include "module_proxy.h"
#include "plain_transport_connection.h"
#include "plain_transport_host.h"
#include "token_manager.h"

#include <QCoreApplication>
#include <QString>
#include <QTemporaryDir>
#include <QVariantList>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

QCoreApplication* ensureApp() {
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance())
        new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

class EchoProvider : public LogosProviderObject {
public:
    EventCallback emitFn;
    QVariant callMethod(const QString& method, const QVariantList& args) override {
        if (method == QLatin1String("echo") && !args.isEmpty()) return args.first();
        return QVariant();
    }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    QJsonArray getMethods() override { return QJsonArray{}; }
    void setEventListener(EventCallback cb) override { emitFn = std::move(cb); }
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("echo_module"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }
};

// One provider instance; its events carry its tag. The proxy stays on the test thread (see the matrix fixture).
struct Provider {
    EchoProvider echo;
    ModuleProxy proxy{&echo};
    QString tag;
    explicit Provider(QString t) : tag(std::move(t)) {
        proxy.saveToken(QStringLiteral("caller"), QStringLiteral("tok"));
    }
    void emitEvent() { if (echo.emitFn) echo.emitFn(QStringLiteral("ev"), QVariantList{tag}); }
};

LogosTransportConfig tcpConfig(uint16_t port)
{
    LogosTransportConfig cfg;
    cfg.protocol = LogosProtocol::Tcp;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    return cfg;
}

// Server side when cert/key are given, client side (no peer verification) when they are not.
LogosTransportConfig tlsConfig(uint16_t port, const std::string& cert = {}, const std::string& key = {})
{
    LogosTransportConfig cfg = tcpConfig(port);
    cfg.protocol = LogosProtocol::TcpSsl;
    cfg.certFile = cert;
    cfg.keyFile = key;
    cfg.verifyPeer = false;
    return cfg;
}

// A throwaway self-signed P-256 certificate, so the TLS path needs no fixture files.
bool writeSelfSignedCert(const std::string& certPath, const std::string& keyPath)
{
    EVP_PKEY* pkey = EVP_EC_gen("P-256");
    X509* x509 = X509_new();
    bool ok = pkey && x509;
    if (ok) {
        ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
        X509_gmtime_adj(X509_getm_notBefore(x509), 0);
        X509_gmtime_adj(X509_getm_notAfter(x509), 3600);
        X509_set_pubkey(x509, pkey);
        X509_NAME* name = X509_get_subject_name(x509);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("127.0.0.1"), -1, -1, 0);
        X509_set_issuer_name(x509, name);
        ok = X509_sign(x509, pkey, EVP_sha256()) > 0;
    }
    if (ok) {
        FILE* k = std::fopen(keyPath.c_str(), "wb");
        FILE* c = std::fopen(certPath.c_str(), "wb");
        ok = k && c && PEM_write_PrivateKey(k, pkey, nullptr, nullptr, 0, nullptr, nullptr)
             && PEM_write_X509(c, x509);
        if (k) std::fclose(k);
        if (c) std::fclose(c);
    }
    X509_free(x509);
    EVP_PKEY_free(pkey);
    return ok;
}

// A provider process stand-in: destroying it closes every connection, like the process exiting.
struct Host {
    logos::plain::PlainTransportHost host;
    uint16_t port = 0;
    Host(const QString& mod, Provider& p, const LogosTransportConfig& cfg) : host(cfg) {
        EXPECT_TRUE(host.start());
        const QString ep = host.endpoint();
        port = ep.mid(ep.lastIndexOf(':') + 1).toUShort();
        EXPECT_TRUE(host.publishObject(mod, &p.proxy));
    }
    Host(const QString& mod, Provider& p, uint16_t wantPort) : Host(mod, p, tcpConfig(wantPort)) {}
};

// Accepts TCP and never answers, so a TLS client's handshake hangs. Counts accepts and client closes.
class SilentListener {
public:
    SilentListener()
        : m_guard(boost::asio::make_work_guard(m_ioc))
        , m_acceptor(m_ioc, {boost::asio::ip::address_v4::loopback(), 0})
    {
        acceptNext();
        m_thread = std::thread([this] { m_ioc.run(); });
    }
    ~SilentListener()
    {
        m_guard.reset();
        m_ioc.stop();
        m_thread.join();
    }
    uint16_t port() const { return m_acceptor.local_endpoint().port(); }
    int accepted() const { return m_accepted.load(); }
    int closed() const { return m_closed.load(); }

private:
    void acceptNext()
    {
        m_acceptor.async_accept([this](const boost::system::error_code& ec,
                                       boost::asio::ip::tcp::socket s) {
            if (ec) return;
            m_accepted.fetch_add(1);
            auto sock = std::make_shared<boost::asio::ip::tcp::socket>(std::move(s));
            drain(sock, std::make_shared<std::array<char, 1024>>());
            acceptNext();
        });
    }
    void drain(std::shared_ptr<boost::asio::ip::tcp::socket> sock,
               std::shared_ptr<std::array<char, 1024>> buf)
    {
        sock->async_read_some(boost::asio::buffer(*buf),
            [this, sock, buf](const boost::system::error_code& ec, std::size_t) {
                if (ec) { m_closed.fetch_add(1); return; }
                drain(sock, buf);
            });
    }

    boost::asio::io_context m_ioc;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> m_guard;
    boost::asio::ip::tcp::acceptor m_acceptor;
    std::thread m_thread;
    std::atomic<int> m_accepted{0};
    std::atomic<int> m_closed{0};
};

template <typename Fn>
bool pumpUntil(Fn done, int budgetMs) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    while (std::chrono::steady_clock::now() < end) {
        if (done()) return true;
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

void pump(int ms) { pumpUntil([] { return false; }, ms); }

template <typename Delivered>
bool emitUntil(Provider& p, Delivered delivered, int budgetMs) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    while (std::chrono::steady_clock::now() < end) {
        p.emitEvent();
        if (pumpUntil(delivered, 25)) return true;
    }
    return delivered();
}

struct Seen {
    bool isEvent = false;
    LogosSubscriptionEvent edge = LogosSubscriptionEvent::Armed;
    quint64 gen = 0;   // the edge's generation, or the one current when the event arrived
    QString text;      // reason, or the event's tag
};

struct Log {
    std::vector<Seen> items;
    std::function<void(LogosSubscriptionEvent, quint64, const QString&)> status() {
        return [this](LogosSubscriptionEvent e, quint64 g, const QString& r) {
            items.push_back({false, e, g, r});
        };
    }
    template <typename Client>
    std::function<void(const QString&, const QVariantList&)> events(Client* c, const QString& mod) {
        return [this, c, mod](const QString&, const QVariantList& data) {
            items.push_back({true, LogosSubscriptionEvent::Armed, c->subscriptionGeneration(mod),
                             data.value(0).toString()});
        };
    }
    bool sawEvent(const QString& tag) const {
        for (const Seen& s : items) if (s.isEvent && s.text == tag) return true;
        return false;
    }
    int edges(LogosSubscriptionEvent e) const {
        int n = 0;
        for (const Seen& s : items) n += (!s.isEvent && s.edge == e);
        return n;
    }
    std::string describe() const {
        std::string out;
        for (const Seen& s : items) {
            if (s.isEvent) continue;
            out += (s.edge == LogosSubscriptionEvent::Armed ? "ARMED(" :
                    s.edge == LogosSubscriptionEvent::Lost ? "LOST(" :
                    s.edge == LogosSubscriptionEvent::Held ? "HELD(" : "ABANDONED(")
                 + std::to_string(s.gen) + ") ";
        }
        return out.empty() ? "<no edges>" : out;
    }
};

constexpr int kBudgetMs = 15000;

} // anonymous namespace

class PlainProviderRestartTest : public ::testing::Test {
protected:
    void SetUp() override { ensureApp(); LogosModeConfig::setMode(LogosMode::Remote); }
};

// DETECTOR: the provider's host goes away and a new one takes its port.
TEST_F(PlainProviderRestartTest, ARestartIsReportedAndReArmed)
{
    const QString mod = QStringLiteral("plain_restart_module");
    Provider p1(QStringLiteral("p1")), p2(QStringLiteral("p2"));
    auto host = std::make_unique<Host>(mod, p1, 0);
    const uint16_t port = host->port;
    ASSERT_NE(port, 0);

    LogosAPIConsumer consumer(mod, QStringLiteral("caller"), &TokenManager::instance(), tcpConfig(port));
    Log log;
    consumer.setSubscriptionStatusCallback(mod, log.status());
    const quint64 id = consumer.onEventWhenAvailable(mod, QStringLiteral("ev"), log.events(&consumer, mod));
    ASSERT_NE(id, 0u);
    ASSERT_EQ(consumer.subscriptionGeneration(mod), 1u);
    ASSERT_TRUE(emitUntil(p1, [&] { return log.sawEvent(p1.tag); }, kBudgetMs)) << "control never delivered";

    host.reset();
    host = std::make_unique<Host>(mod, p2, port);
    ASSERT_EQ(host->port, port) << "the replacement could not take the port";

    const bool delivered = emitUntil(p2, [&] { return log.sawEvent(p2.tag); }, kBudgetMs);
    EXPECT_TRUE(delivered) << "the restarted provider's events never arrived; edges: " << log.describe()
                           << " generation " << consumer.subscriptionGeneration(mod);
    EXPECT_EQ(log.edges(LogosSubscriptionEvent::Lost), 1) << log.describe();
    EXPECT_EQ(consumer.subscriptionGeneration(mod), 2u) << log.describe();
    EXPECT_EQ(consumer.eventSubscriptionState(id), LogosSubscriptionState::Armed);
    for (const Seen& s : log.items) {
        if (!s.isEvent && s.edge == LogosSubscriptionEvent::Lost)
            EXPECT_EQ(s.text, QStringLiteral("provider_unavailable"));
        if (s.isEvent && s.text == p2.tag) EXPECT_EQ(s.gen, 2u);
    }
}

// DETECTOR: while the provider is gone the subscription is tracked as pending, not armed, and it re-arms later.
TEST_F(PlainProviderRestartTest, AProviderGoneForAWhileIsPendingThenReArms)
{
    const QString mod = QStringLiteral("plain_down_module");
    Provider p1(QStringLiteral("p1")), p2(QStringLiteral("p2"));
    auto host = std::make_unique<Host>(mod, p1, 0);
    const uint16_t port = host->port;

    LogosAPIConsumer consumer(mod, QStringLiteral("caller"), &TokenManager::instance(), tcpConfig(port));
    Log log;
    consumer.setSubscriptionStatusCallback(mod, log.status());
    const quint64 id = consumer.onEventWhenAvailable(mod, QStringLiteral("ev"), log.events(&consumer, mod));
    ASSERT_TRUE(emitUntil(p1, [&] { return log.sawEvent(p1.tag); }, kBudgetMs));

    host.reset();
    EXPECT_TRUE(pumpUntil([&] { return log.edges(LogosSubscriptionEvent::Lost) == 1; }, kBudgetMs))
        << "a provider that went away was never reported; edges: " << log.describe();
    pump(2500);   // several retry intervals with nothing listening
    EXPECT_EQ(consumer.eventSubscriptionState(id), LogosSubscriptionState::Pending)
        << "a subscription with no provider behind it still reports Armed";
    EXPECT_EQ(log.edges(LogosSubscriptionEvent::Lost), 1) << log.describe();

    host = std::make_unique<Host>(mod, p2, port);
    EXPECT_TRUE(emitUntil(p2, [&] { return log.sawEvent(p2.tag); }, kBudgetMs))
        << "never re-armed once the provider returned; edges: " << log.describe();
    EXPECT_EQ(consumer.subscriptionGeneration(mod), 2u) << log.describe();
}

// DETECTOR: Manual holds across the restart and revives on rearm.
TEST_F(PlainProviderRestartTest, ManualHoldsAcrossARestart)
{
    const QString mod = QStringLiteral("plain_manual_module");
    Provider p1(QStringLiteral("p1")), p2(QStringLiteral("p2"));
    auto host = std::make_unique<Host>(mod, p1, 0);
    const uint16_t port = host->port;

    LogosAPIConsumer consumer(mod, QStringLiteral("caller"), &TokenManager::instance(), tcpConfig(port));
    consumer.setSubscriptionRestartPolicy(mod, LogosRestartPolicy::Manual);
    Log log;
    consumer.setSubscriptionStatusCallback(mod, log.status());
    const quint64 id = consumer.onEventWhenAvailable(mod, QStringLiteral("ev"), log.events(&consumer, mod));
    ASSERT_TRUE(emitUntil(p1, [&] { return log.sawEvent(p1.tag); }, kBudgetMs));

    host.reset();
    host = std::make_unique<Host>(mod, p2, port);
    EXPECT_TRUE(pumpUntil([&] { return log.edges(LogosSubscriptionEvent::Held) == 1; }, kBudgetMs))
        << "the restart was not reported; edges: " << log.describe();

    emitUntil(p2, [] { return false; }, 1500);
    EXPECT_FALSE(log.sawEvent(p2.tag)) << "a HELD subscription received the new provider's events";
    EXPECT_EQ(consumer.eventSubscriptionState(id), LogosSubscriptionState::Held);
    EXPECT_EQ(log.edges(LogosSubscriptionEvent::Lost), 0) << log.describe();

    EXPECT_TRUE(consumer.rearmSubscriptions(mod));
    EXPECT_TRUE(emitUntil(p2, [&] { return log.sawEvent(p2.tag); }, kBudgetMs))
        << "rearm did not revive it; edges: " << log.describe();
    EXPECT_EQ(consumer.subscriptionGeneration(mod), 2u) << log.describe();
}

// DETECTOR: the C ABI over TCP sees the same thing.
TEST_F(PlainProviderRestartTest, LpClientOverTcpSeesTheRestart)
{
    const QString mod = QStringLiteral("plain_abi_module");
    Provider p1(QStringLiteral("p1")), p2(QStringLiteral("p2"));
    auto host = std::make_unique<Host>(mod, p1, 0);
    const uint16_t port = host->port;
    const std::string ep = "{\"protocol\":\"tcp\",\"host\":\"127.0.0.1\",\"port\":" + std::to_string(port) + "}";

    lp_client* client = lp_client_create(mod.toUtf8().constData(), "caller", ep.c_str(), ep.c_str());
    ASSERT_NE(client, nullptr);
    struct Abi {
        lp_client* client = nullptr;
        std::vector<int> states;
        std::vector<std::pair<std::string, unsigned long long>> events;
        bool sawP1 = false, sawP2 = false;
    } abi;
    abi.client = client;
    ASSERT_EQ(lp_client_set_subscription_status_cb(client,
        [](int state, unsigned long long, const char*, void* ud) {
            static_cast<Abi*>(ud)->states.push_back(state);
        }, &abi), 1);
    lp_subscription* sub = lp_subscribe(client, "ev",
        [](const char*, const char* data, void* ud) {
            auto* a = static_cast<Abi*>(ud);
            a->events.emplace_back(data, lp_client_subscription_generation(a->client));
            a->sawP1 = a->sawP1 || std::strstr(data, "\"p1\"");
            a->sawP2 = a->sawP2 || std::strstr(data, "\"p2\"");
        }, &abi);
    ASSERT_NE(sub, nullptr);
    ASSERT_TRUE(emitUntil(p1, [&] { return abi.sawP1; }, kBudgetMs));

    host.reset();
    host = std::make_unique<Host>(mod, p2, port);
    EXPECT_TRUE(emitUntil(p2, [&] { return abi.sawP2; }, kBudgetMs))
        << "the restarted provider's events never reached the C ABI";
    EXPECT_EQ(std::count(abi.states.begin(), abi.states.end(), LP_SUB_LOST), 1);
    EXPECT_EQ(lp_client_subscription_generation(client), 2ull);
    for (const auto& [payload, gen] : abi.events)
        if (payload.find("\"p2\"") != std::string::npos) EXPECT_EQ(gen, 2ull);

    lp_unsubscribe(sub);
    lp_client_destroy(client);
}

// DETECTOR: calls recover too — they share the connection the subscription lost.
TEST_F(PlainProviderRestartTest, CallsRecoverAfterARestart)
{
    const QString mod = QStringLiteral("plain_calls_module");
    TokenManager::instance().saveToken(mod, QStringLiteral("tok"));
    Provider p1(QStringLiteral("p1")), p2(QStringLiteral("p2"));
    auto host = std::make_unique<Host>(mod, p1, 0);
    const uint16_t port = host->port;
    const LogosTransportConfig cfg = tcpConfig(port);
    LogosAPIClient client(mod, QStringLiteral("caller"), &TokenManager::instance(), cfg, cfg);

    // Async on purpose: a sync call would block the thread the in-process provider dispatches on.
    auto echo = [&](int value) {
        auto answer = std::make_shared<std::pair<bool, QVariant>>(false, QVariant());
        client.invokeRemoteMethodAsync(mod, QStringLiteral("echo"), QVariantList{value},
            LogosAPIClient::AsyncResultErrorCallback(
                [answer](QVariant r, const logos::CallError&) { *answer = {true, r}; }),
            Timeout(3000));
        pumpUntil([&] { return answer->first; }, 5000);
        return answer->second;
    };
    ASSERT_EQ(echo(1).toInt(), 1) << "control call failed";

    host.reset();
    host = std::make_unique<Host>(mod, p2, port);
    bool recovered = false;
    for (int i = 0; i < 20 && !recovered; ++i) {
        recovered = echo(2).toInt() == 2;
        if (!recovered) pump(250);
    }
    EXPECT_TRUE(recovered) << "calls never reached the restarted provider";
}

// DETECTOR: the same restart over TLS, which redials through the asynchronous handshake.
TEST_F(PlainProviderRestartTest, ATlsRestartIsReportedAndReArmed)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string cert = (dir.path() + QStringLiteral("/cert.pem")).toStdString();
    const std::string key = (dir.path() + QStringLiteral("/key.pem")).toStdString();
    ASSERT_TRUE(writeSelfSignedCert(cert, key));

    const QString mod = QStringLiteral("plain_tls_module");
    Provider p1(QStringLiteral("p1")), p2(QStringLiteral("p2"));
    auto host = std::make_unique<Host>(mod, p1, tlsConfig(0, cert, key));
    const uint16_t port = host->port;
    ASSERT_NE(port, 0);

    LogosAPIConsumer consumer(mod, QStringLiteral("caller"), &TokenManager::instance(), tlsConfig(port));
    ASSERT_TRUE(consumer.isConnected()) << "control: the TLS handshake itself failed";
    Log log;
    consumer.setSubscriptionStatusCallback(mod, log.status());
    ASSERT_NE(consumer.onEventWhenAvailable(mod, QStringLiteral("ev"), log.events(&consumer, mod)), 0u);
    ASSERT_TRUE(emitUntil(p1, [&] { return log.sawEvent(p1.tag); }, kBudgetMs)) << "control never delivered";

    host.reset();
    host = std::make_unique<Host>(mod, p2, tlsConfig(port, cert, key));
    EXPECT_TRUE(emitUntil(p2, [&] { return log.sawEvent(p2.tag); }, kBudgetMs))
        << "never re-armed over TLS; edges: " << log.describe();
    EXPECT_EQ(log.edges(LogosSubscriptionEvent::Lost), 1) << log.describe();
    EXPECT_EQ(consumer.subscriptionGeneration(mod), 2u) << log.describe();
}

// DETECTOR: a redial stuck in its handshake never blocks a zero-timeout acquire, and its deadline frees the next one.
TEST_F(PlainProviderRestartTest, AStuckRedialNeverBlocksAndTimesOut)
{
    SilentListener silent;
    // No connectToHost(): its blocking handshake would hang on this listener.
    logos::plain::PlainTransportConnection conn(tlsConfig(silent.port()));
    using clock = std::chrono::steady_clock;
    const auto ms = [](clock::duration d) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
    };

    auto t0 = clock::now();
    EXPECT_EQ(conn.requestObject(QStringLiteral("m"), 0), nullptr);
    EXPECT_LT(ms(clock::now() - t0), 200) << "a zero-timeout acquire waited on the dial";
    ASSERT_TRUE(pumpUntil([&] { return silent.accepted() == 1; }, 5000)) << "the redial never connected";

    t0 = clock::now();
    EXPECT_EQ(conn.requestObject(QStringLiteral("m"), 300), nullptr);
    const auto waited = ms(clock::now() - t0);
    EXPECT_GE(waited, 250) << "a call did not wait for the dial in flight";
    EXPECT_LT(waited, 2000) << "a call waited past its own budget";

    EXPECT_FALSE(conn.isConnected());
    EXPECT_TRUE(pumpUntil([&] { conn.isConnected(); return silent.accepted() >= 2; }, 10000))
        << "the stuck redial was never abandoned, so no new attempt started";
    EXPECT_GE(silent.closed(), 1) << "the abandoned attempt left its socket open";
}

// CONTROL: a healthy connection is never a restart.
TEST_F(PlainProviderRestartTest, AHealthyConnectionIsNotARestart)
{
    const QString mod = QStringLiteral("plain_healthy_module");
    Provider p1(QStringLiteral("p1"));
    auto host = std::make_unique<Host>(mod, p1, 0);

    LogosAPIConsumer consumer(mod, QStringLiteral("caller"), &TokenManager::instance(), tcpConfig(host->port));
    Log log;
    consumer.setSubscriptionStatusCallback(mod, log.status());
    const quint64 id = consumer.onEventWhenAvailable(mod, QStringLiteral("ev"), log.events(&consumer, mod));
    ASSERT_TRUE(emitUntil(p1, [&] { return log.sawEvent(p1.tag); }, kBudgetMs));
    pump(2500);   // more than two liveness ticks
    const size_t before = log.items.size();
    EXPECT_TRUE(emitUntil(p1, [&] { return log.items.size() > before; }, kBudgetMs))
        << "a healthy subscription stopped delivering";

    EXPECT_EQ(log.edges(LogosSubscriptionEvent::Lost), 0) << log.describe();
    EXPECT_EQ(log.edges(LogosSubscriptionEvent::Armed), 1) << log.describe();
    EXPECT_EQ(consumer.subscriptionGeneration(mod), 1u);
    EXPECT_EQ(consumer.eventSubscriptionState(id), LogosSubscriptionState::Armed);
}
