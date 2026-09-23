#include "logos_protocol.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QRemoteObjectHost>
#include <QUrl>
#include <QVariant>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <unistd.h>

using namespace std::chrono_literals;

namespace {

class QtProvider : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE QVariant callRemoteMethod(const QString&, const QString&,
                                          const QVariantList&)
    {
        return true;
    }

signals:
    void eventResponse(const QString&, const QVariantList&);
};

enum class CallbackKind { AsyncResult, SubscriptionStatus };
enum class QueuedWork { None, Event, Disconnect };

struct CallbackState {
    lp_client* client = nullptr;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool proceed = false;
    std::atomic<bool> done{false};
    std::atomic<int> success{-1};
    std::atomic<int> events{0};
};

bool spinUntil(const std::function<bool()>& done,
               std::chrono::milliseconds timeout = 2s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        if (done()) return true;
        std::this_thread::sleep_for(5ms);
    }
    QCoreApplication::processEvents();
    return done();
}

void destroyFromCallback(CallbackState* state)
{
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->entered = true;
        state->changed.notify_all();
        state->changed.wait(lock, [&] { return state->proceed; });
    }
    lp_client_destroy(state->client);
    state->client = nullptr;
    state->done = true;
}

void onAsyncResult(int success, const char*, void* userData)
{
    auto* state = static_cast<CallbackState*>(userData);
    state->success = success;
    destroyFromCallback(state);
}

void onStatus(int status, unsigned long long, const char*, void* userData)
{
    if (status == LP_SUB_ARMED)
        destroyFromCallback(static_cast<CallbackState*>(userData));
}

void ignoreStatus(int, unsigned long long, const char*, void*) {}

void onEvent(const char*, const char*, void* userData)
{
    ++static_cast<CallbackState*>(userData)->events;
}

void runCase(CallbackKind kind, QueuedWork queued)
{
    const std::string instance = "qro_callback_destroy_" + std::to_string(::getpid());
    ASSERT_EQ(::setenv("LOGOS_INSTANCE_ID", instance.c_str(), 1), 0);
    QtProvider provider;
    auto host = std::make_unique<QRemoteObjectHost>();
    ASSERT_TRUE(host->setHostUrl(QUrl(QString::fromStdString(
        "local:logos_fixture_" + instance))));
    ASSERT_TRUE(host->enableRemoting(&provider, QStringLiteral("fixture")));
    ASSERT_EQ(lp_token_save("fixture", "secret"), LP_OK);

    CallbackState state;
    state.client = lp_client_create("fixture", "caller", nullptr, nullptr);
    ASSERT_NE(state.client, nullptr);
    ASSERT_EQ(lp_client_set_subscription_status_cb(state.client,
        kind == CallbackKind::SubscriptionStatus ? onStatus : ignoreStatus,
        &state), 1);
    lp_subscription* subscription = lp_subscribe(
        state.client, "tick", onEvent, &state);
    ASSERT_NE(subscription, nullptr);
    if (kind == CallbackKind::AsyncResult) {
        ASSERT_TRUE(spinUntil([&] {
            return lp_client_subscription_generation(state.client) != 0;
        }));
        ASSERT_EQ(lp_invoke_async(state.client, "echo", "[]", 500,
                                  onAsyncResult, &state), LP_OK);
    }
    ASSERT_TRUE(spinUntil([&] {
        std::lock_guard<std::mutex> lock(state.mutex);
        return state.entered;
    }));
    if (queued == QueuedWork::Event)
        emit provider.eventResponse(QStringLiteral("tick"), {});
    else if (queued == QueuedWork::Disconnect)
        host.reset();
    (void)spinUntil([] { return false; }, 200ms);
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.proceed = true;
    }
    state.changed.notify_all();
    if (!spinUntil([&] { return state.done.load(); })) {
        ADD_FAILURE() << "Client destruction blocked behind a queued callback";
        std::_Exit(2); // This GTest case runs in its own process under CTest.
    }
    if (kind == CallbackKind::AsyncResult)
        EXPECT_EQ(state.success.load(), 1);
    (void)spinUntil([] { return false; }, 20ms);
    EXPECT_EQ(state.events.load(), 0);
    lp_unsubscribe(subscription);
}

TEST(QtRemotePlainRealQroTeardown, AsyncResultWithQueuedEvent)
{
    runCase(CallbackKind::AsyncResult, QueuedWork::Event);
}

TEST(QtRemotePlainRealQroTeardown, AsyncResultWithQueuedDisconnectStatus)
{
    runCase(CallbackKind::AsyncResult, QueuedWork::Disconnect);
}

TEST(QtRemotePlainRealQroTeardown, StatusWithQueuedEvent)
{
    runCase(CallbackKind::SubscriptionStatus, QueuedWork::Event);
}

TEST(QtRemotePlainRealQroTeardown, AsyncResultWithoutQueuedWork)
{
    runCase(CallbackKind::AsyncResult, QueuedWork::None);
}

TEST(QtRemotePlainRealQroTeardown, StatusWithoutQueuedWork)
{
    runCase(CallbackKind::SubscriptionStatus, QueuedWork::None);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

#include "test_qt_remote_plain_qro_callback_destroy.moc"
