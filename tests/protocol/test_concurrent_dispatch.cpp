// Proves per-module concurrent dispatch (concurrency:"multi") with NO provider
// ABI change — concurrency is owned by the module, behind the ordinary
// callMethod, exactly as a generated "multi" glue does it:
//
//   - multi  : callMethod does NOT block. It hands slow() to a worker and
//              returns a PENDING SENTINEL ({pendingCallKey: callId}) at once, so
//              the dispatch thread is free to take the next call. The worker
//              records peak overlap, then pushes the result as a
//              callCompleteEvent([callId, result]) over the event listener.
//              The plain consumer detects the sentinel and awaits the
//              completion.                                      ⇒ overlap (peak 2)
//   - single : callMethod runs slow() inline, blocking the dispatch thread until
//              it returns the result directly (no sentinel).    ⇒ serial (peak 1)
//
// The host (ModuleProxy / PlainTransportHost) is unchanged from master: it just
// returns whatever callMethod returned and forwards whatever events the provider
// emits. The callers run on their own threads (blocking in the consumer) while
// the main thread pumps the event loop so the worker's completion event — which
// ModuleProxy marshals onto the source thread — is delivered.

#include <gtest/gtest.h>

#include "logos_async_dispatch.h"
#include "logos_object.h"
#include "logos_provider_interface.h"
#include "logos_transport_config.h"
#include "module_proxy.h"

#include "plain_transport_connection.h"
#include "plain_transport_host.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

using namespace logos::plain;

namespace {

// Records the peak number of slow() handlers running at the same time.
class SlowProvider : public LogosProviderObject {
public:
    explicit SlowProvider(bool multi) : m_multi(multi) {}

    QVariant callMethod(const QString& method, const QVariantList& args) override
    {
        if (method != QLatin1String("slow")) return QVariant();
        const int ms = args.value(0).toInt();

        // single: run inline → blocks the dispatch thread → calls serialize.
        if (!m_multi) return runSlow(ms);

        // multi: defer. Spawn a worker, return a pending sentinel immediately so
        // the dispatch thread is freed; the worker emits the completion event.
        const QString callId = QStringLiteral("lc-%1").arg(
            static_cast<qulonglong>(m_callCounter.fetch_add(1, std::memory_order_relaxed)));
        std::thread([this, callId, ms]() {
            const int result = runSlow(ms);
            if (m_eventCb)
                m_eventCb(logos::callCompleteEvent(), QVariantList{ callId, QVariant(result) });
        }).detach();
        QVariantMap pending;
        pending[logos::pendingCallKey()] = callId;
        return pending;
    }

    QJsonArray getMethods() override { return QJsonArray{}; }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    void setEventListener(EventCallback cb) override { m_eventCb = std::move(cb); }
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("slow"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }

    int maxConcurrent() const { return m_maxSeen.load(); }

private:
    int runSlow(int ms)
    {
        const int now = ++m_inFlight;
        int prev = m_maxSeen.load();
        while (now > prev && !m_maxSeen.compare_exchange_weak(prev, now)) { /* retry */ }
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        --m_inFlight;
        return ms;
    }

    bool m_multi;
    EventCallback m_eventCb;
    std::atomic<std::uint64_t> m_callCounter{0};
    std::atomic<int> m_inFlight{0};
    std::atomic<int> m_maxSeen{0};
};

QCoreApplication* ensureApp()
{
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance())
        new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

}  // namespace

class ConcurrentDispatchTest : public ::testing::Test {
protected:
    void SetUp() override { ensureApp(); }

    // Fire two concurrent slow() calls and return the peak observed overlap.
    int peakOverlap(bool multi)
    {
        LogosTransportConfig cfg;
        cfg.protocol = LogosProtocol::Tcp;
        cfg.host = "127.0.0.1";
        cfg.port = 0;

        auto host = std::make_unique<PlainTransportHost>(cfg);
        EXPECT_TRUE(host->start());

        SlowProvider provider(multi);
        ModuleProxy proxy(&provider);
        proxy.saveToken(QStringLiteral("core"), QStringLiteral("tok"));
        EXPECT_TRUE(host->publishObject("slow_mod", &proxy));

        const QString endpoint = host->endpoint();
        const uint16_t port = endpoint.mid(endpoint.lastIndexOf(':') + 1).toUShort();

        LogosTransportConfig ccfg = cfg;
        ccfg.port = port;
        auto conn = std::make_unique<PlainTransportConnection>(ccfg);
        EXPECT_TRUE(conn->connectToHost());

        LogosObject* obj = conn->requestObject("slow_mod", 2000);
        EXPECT_NE(obj, nullptr);
        if (!obj) return -1;

        std::atomic<int> done{0};
        auto caller = [&]() {
            obj->callMethod(QStringLiteral("tok"), QStringLiteral("slow"),
                            QVariantList{ 300 }, 5000);
            done.fetch_add(1);
        };
        std::thread t1(caller), t2(caller);

        // Pump the host event loop until both callers return (or a generous cap).
        for (int i = 0; i < 800 && done.load() < 2; ++i) {
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        t1.join();
        t2.join();
        EXPECT_EQ(done.load(), 2);

        const int peak = provider.maxConcurrent();
        obj->release();
        host.reset();
        return peak;
    }
};

TEST_F(ConcurrentDispatchTest, MultiProviderOverlaps)
{
    EXPECT_EQ(peakOverlap(/*multi=*/true), 2);
}

TEST_F(ConcurrentDispatchTest, SingleProviderSerializes)
{
    EXPECT_EQ(peakOverlap(/*multi=*/false), 1);
}
