// Regression: an lp client created from a worker thread must still work over
// the Qt-affine (qt_remote / LocalSocket) transport.
//
// Bug: lp_client_create() constructed the LogosAPIClient — and with it the
// QRemoteObjectNode and its QLocalSocket — on the CALLING thread, which then
// became the client's owner thread. Callers reach lp_client_create through a
// lazily-created wrapper (the generated bind_<iface>() → LpClient::ensure()),
// so the first thread to make an outbound call owns the transport for the rest
// of the process. A module whose first call comes from a worker — an HTTP
// handler, a timer thread — bound its whole transport to a thread that only
// pumps events while it is already blocked inside a call. Replica acquisition
// then never completed: every requestObject() burned its full 20s timeout and
// returned nullptr, so the call yielded an empty result (openmetrics-module:
// a single GET /metrics took 40s and silently dropped a module's payload).
//
// Fix: for Qt-affine transports the client is constructed on the Qt main
// thread regardless of which thread calls, so the existing per-call marshal
// (logos::runOnOwnerThread) lands on a thread that actually runs an event
// loop.
//
// Pre-fix this test fails by timing out (the worker's first call blocks ~20s
// per acquire, twice); post-fix the whole exchange takes tens of milliseconds.

#include <gtest/gtest.h>

#include "logos_instance.h"
#include "logos_mode.h"
#include "logos_protocol.h"
#include "logos_provider_interface.h"
#include "module_proxy.h"
#include "remote_transport.h"
#include "token_manager.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QString>
#include <QUuid>
#include <QVariantList>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace {

QCoreApplication* ensureApp() {
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance())
        new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

class PingProvider : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList&) override {
        if (method == QLatin1String("ping")) return QStringLiteral("ok");
        return QVariant();
    }
    bool informModuleToken(const QString& moduleName, const QString& token) override {
        if (m_proxy) m_proxy->saveToken(moduleName, token);
        return true;
    }
    QJsonArray getMethods() override { return QJsonArray{}; }
    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("worker_target"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }

    void bindProxy(ModuleProxy* p) { m_proxy = p; }
private:
    ModuleProxy* m_proxy = nullptr;
};

// Mints a token for the target on requestModule, like the real capability
// module — the handshake the client runs before its first call to a target.
class CapabilityProvider : public LogosProviderObject {
public:
    void bindTarget(ModuleProxy* targetProxy) { m_targetProxy = targetProxy; }

    QVariant callMethod(const QString& method, const QVariantList& args) override {
        if (method == QLatin1String("requestModule") && args.size() == 2) {
            const QString from = args.value(0).toString();
            const QString tok  = QUuid::createUuid().toString(QUuid::WithoutBraces);
            if (m_targetProxy) m_targetProxy->saveToken(from, tok);
            return tok;
        }
        return QVariant();
    }
    bool informModuleToken(const QString& moduleName, const QString& token) override {
        lastModule = moduleName;
        lastToken = token;
        return true;
    }
    QJsonArray getMethods() override { return QJsonArray{}; }
    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("capability_module"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }

    QString lastModule;
    QString lastToken;
private:
    ModuleProxy* m_targetProxy = nullptr;
};

} // namespace

class LpClientOwnerThreadTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensureApp();
        // Other suites in this binary switch the process mode; pin it so we
        // exercise the Qt-affine transport this regression is about.
        LogosModeConfig::setMode(LogosMode::Remote);
        TokenManager::instance().clearAllTokens();
    }
    void TearDown() override {
        TokenManager::instance().clearAllTokens();
    }

    void pumpEventLoop(int ms) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
};

// The whole point: the client is created AND used from a thread that never
// runs a Qt event loop, while the main thread runs one. The call must go
// through — the library marshals it — and must not sit on an acquire timeout.
TEST_F(LpClientOwnerThreadTest, ClientCreatedOnWorkerThreadCallsOverQtRemote)
{
    RemoteTransportHost capHost(LogosInstance::id("capability_module"));
    RemoteTransportHost targetHost(LogosInstance::id("worker_target"));

    PingProvider targetProvider;
    ModuleProxy  targetProxy(&targetProvider);
    targetProvider.bindProxy(&targetProxy);

    CapabilityProvider capProvider;
    ModuleProxy        capProxy(&capProvider);
    capProvider.bindTarget(&targetProxy);

    const QString bootstrapToken = QStringLiteral("bootstrap-tok-worker");
    TokenManager::instance().saveToken(QStringLiteral("capability_module"), bootstrapToken);
    ASSERT_TRUE(capProxy.saveToken(QStringLiteral("worker_origin"), bootstrapToken));

    ASSERT_TRUE(capHost.publishObject("capability_module", &capProxy));
    ASSERT_TRUE(targetHost.publishObject("worker_target", &targetProxy));

    std::atomic<bool> done{false};
    std::atomic<int> rc{-1};
    std::string result;
    std::string error;

    const auto started = std::chrono::steady_clock::now();
    std::thread worker([&]() {
        lp_client* client = lp_client_create("worker_target", "worker_origin",
                                             nullptr, nullptr);
        if (!client) { done = true; return; }

        char* out = nullptr;
        char* err = nullptr;
        rc = lp_invoke(client, "ping", "[]", 5000, &out, &err);
        if (out) { result = out; lp_string_free(out); }
        if (err) { error = err;  lp_string_free(err); }

        lp_client_destroy(client);
        done = true;
    });

    // The worker's call marshals onto this thread, so this thread must be the
    // one pumping. 15s is far below the 2 x 20s the pre-fix path would burn
    // and far above the tens of ms the fixed path needs.
    const auto deadline = started + std::chrono::seconds(15);
    while (!done.load() && std::chrono::steady_clock::now() < deadline)
        pumpEventLoop(10);

    const bool finished = done.load();
    worker.join();
    pumpEventLoop(50);   // let the deferred client delete run

    ASSERT_TRUE(finished)
        << "worker-thread call did not complete within 15s — the client was "
           "bound to a thread with no event loop and is sitting on replica "
           "acquire timeouts";
    EXPECT_EQ(rc.load(), LP_OK) << "error: " << error;
    EXPECT_EQ(result, "\"ok\"");

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    EXPECT_LT(elapsedMs, 5000)
        << "call took " << elapsedMs << "ms; a healthy in-process exchange is "
           "tens of ms, seconds means something is falling out on a timeout";
}

// A second client on the same worker thread must work too: the first call is
// what binds the owner thread, so anything that only fixes the first client
// (rather than the construction rule) would regress here.
TEST_F(LpClientOwnerThreadTest, SecondWorkerThreadClientAlsoWorks)
{
    RemoteTransportHost capHost(LogosInstance::id("capability_module"));
    RemoteTransportHost targetHost(LogosInstance::id("worker_target"));

    PingProvider targetProvider;
    ModuleProxy  targetProxy(&targetProvider);
    targetProvider.bindProxy(&targetProxy);

    CapabilityProvider capProvider;
    ModuleProxy        capProxy(&capProvider);
    capProvider.bindTarget(&targetProxy);

    const QString bootstrapToken = QStringLiteral("bootstrap-tok-worker2");
    TokenManager::instance().saveToken(QStringLiteral("capability_module"), bootstrapToken);
    ASSERT_TRUE(capProxy.saveToken(QStringLiteral("worker_origin"), bootstrapToken));

    ASSERT_TRUE(capHost.publishObject("capability_module", &capProxy));
    ASSERT_TRUE(targetHost.publishObject("worker_target", &targetProxy));

    std::atomic<bool> done{false};
    std::atomic<int> calls{0};

    const auto started = std::chrono::steady_clock::now();
    std::thread worker([&]() {
        for (int i = 0; i < 2; ++i) {
            lp_client* client = lp_client_create("worker_target", "worker_origin",
                                                 nullptr, nullptr);
            if (!client) break;
            char* out = nullptr;
            if (lp_invoke(client, "ping", "[]", 5000, &out, nullptr) == LP_OK
                && out && std::string(out) == "\"ok\"")
                calls.fetch_add(1);
            if (out) lp_string_free(out);
            lp_client_destroy(client);
        }
        done = true;
    });

    const auto deadline = started + std::chrono::seconds(15);
    while (!done.load() && std::chrono::steady_clock::now() < deadline)
        pumpEventLoop(10);

    const bool finished = done.load();
    worker.join();
    pumpEventLoop(50);

    ASSERT_TRUE(finished) << "worker-thread clients did not complete within 15s";
    EXPECT_EQ(calls.load(), 2);
}

// The same contract, reached through the public C ABI's token handoff.
// lp_inform_module_token lands on the 3-arg LogosAPIClient::informModuleToken,
// which was the one client entry point with no marshal — so a worker acquired
// capability_module's replica against a node owned by the main thread,
// waitForSource burned its full 20s, and the grant came back as a refusal.
// The header has promised the opposite all along (logos_protocol.h:29-31:
// "the library marshals to the handle's owner thread internally where required").
TEST_F(LpClientOwnerThreadTest, InformModuleTokenFromWorkerThreadReachesCapabilityModule)
{
    RemoteTransportHost capHost(LogosInstance::id("capability_module"));

    CapabilityProvider capProvider;
    ModuleProxy        capProxy(&capProvider);

    // ModuleProxy::informModuleToken accepts only this store's credential,
    // which TokenManager derives from the "capability_module" bootstrap key.
    const QString bootstrapToken = QStringLiteral("bootstrap-tok-inform");
    TokenManager::instance().saveToken(QStringLiteral("capability_module"), bootstrapToken);

    ASSERT_TRUE(capHost.publishObject("capability_module", &capProxy));

    std::atomic<bool> done{false};
    std::atomic<int> rc{-1};

    const auto started = std::chrono::steady_clock::now();
    std::thread worker([&]() {
        lp_client* client = lp_client_create("capability_module", "worker_origin",
                                             nullptr, nullptr);
        if (!client) { done = true; return; }

        rc = lp_inform_module_token(client,
                                    bootstrapToken.toUtf8().constData(),
                                    "granted_module", "granted-tok");
        lp_client_destroy(client);
        done = true;
    });

    const auto deadline = started + std::chrono::seconds(15);
    while (!done.load() && std::chrono::steady_clock::now() < deadline)
        pumpEventLoop(10);

    const bool finished = done.load();
    worker.join();
    pumpEventLoop(50);

    ASSERT_TRUE(finished)
        << "worker-thread lp_inform_module_token did not complete within 15s — "
           "it is acquiring capability_module's replica on a thread that owns "
           "neither the node nor an event loop";
    EXPECT_EQ(rc.load(), LP_OK);
    EXPECT_EQ(capProvider.lastModule, QStringLiteral("granted_module"));
    EXPECT_EQ(capProvider.lastToken, QStringLiteral("granted-tok"));

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    EXPECT_LT(elapsedMs, 5000)
        << "grant took " << elapsedMs << "ms; tens of ms is healthy, seconds "
           "means the acquire fell out on a timeout";
}
