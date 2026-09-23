#include "implementations/qt_remote_plain/qtro_transport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace logos::plain;
using namespace logos::qt_remote_plain;

namespace {

std::string transportSocketPath()
{
    static std::atomic<unsigned> serial{0};
#ifdef _WIN32
    return "local:logos_qtro_transport_" + std::to_string(::_getpid())
        + "_" + std::to_string(serial.fetch_add(1));
#else
    return "/tmp/logos_qtro_transport_" + std::to_string(::getpid())
        + "_" + std::to_string(serial.fetch_add(1));
#endif
}

TEST(QtRemotePlainTransportTest, PlainClientAndServerCallAndEmitWithoutQt)
{
    const std::string path = transportSocketPath();
    Server server;
    Server::Object object;
    object.name = "fixture";
    object.definition = {
        "Fixture",
        {{"eventResponse(QString,QVariantList)", {"eventName", "data"}}},
        {{"echo(QString)", "QString", {"value"}}},
        {},
    };
    object.invoke = [](std::int32_t method, const std::vector<Variant>& arguments) {
        if (method != 0 || arguments.size() != 1)
            throw std::runtime_error("unexpected invocation");
        return Variant::fromRpc(RpcValue{"plain:" + arguments[0].value.asString()});
    };
    ASSERT_TRUE(server.publish(std::move(object)));
    std::string error;
    ASSERT_TRUE(server.start(path, &error)) << error;

    Client client;
    ASSERT_TRUE(client.connect(path, std::chrono::seconds(2), &error)) << error;
    ASSERT_TRUE(client.acquire("fixture", std::chrono::seconds(2), &error)) << error;
    const auto result = client.call("fixture", "echo(QString)",
        {Variant::fromRpc(RpcValue{"hello"})}, std::chrono::seconds(2), &error);
    ASSERT_TRUE(result.has_value()) << error;
    EXPECT_EQ(result->value.asString(), "plain:hello");

    std::mutex eventMutex;
    std::condition_variable eventReady;
    std::string eventValue;
    client.setEventHandler([&](const std::string& name, std::int32_t index,
                               std::vector<Variant> arguments) {
        if (name != "fixture" || index != 0 || arguments.size() != 2) return;
        std::lock_guard<std::mutex> lock(eventMutex);
        eventValue = arguments[1].value.asList().items.at(0).asString();
        eventReady.notify_all();
    });
    RpcList payload;
    payload.items.emplace_back("payload");
    ASSERT_TRUE(server.emitSignal("fixture", 0,
        {Variant::fromRpc(RpcValue{"tick"}),
         Variant::fromRpc(RpcValue{std::move(payload)})}, &error)) << error;
    std::unique_lock<std::mutex> lock(eventMutex);
    ASSERT_TRUE(eventReady.wait_for(lock, std::chrono::seconds(2), [&] {
        return !eventValue.empty();
    }));
    EXPECT_EQ(eventValue, "payload");
    lock.unlock();

    client.close();
    server.stop();
}

Server::Object echoFixture()
{
    Server::Object object;
    object.name = "fixture";
    object.definition = {"Fixture", {}, {{"echo(QString)", "QString", {"value"}}}, {}};
    object.invoke = [](std::int32_t, const std::vector<Variant>& arguments) {
        return Variant::fromRpc(RpcValue{"plain:" + arguments.at(0).value.asString()});
    };
    return object;
}

// Detector: the client tried once, so a provider still starting (or
// restarting) failed the call instantly where QtRO would have waited for it.
TEST(QtRemotePlainTransportTest, AClientReachesAServerThatStartsWithinItsTimeout)
{
    const std::string path = transportSocketPath();
    Server server;
    ASSERT_TRUE(server.publish(echoFixture()));
    std::thread starter([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        std::string error;
        EXPECT_TRUE(server.start(path, &error)) << error;
    });

    Client client;
    std::string error;
    const bool connected = client.connect(path, std::chrono::seconds(3), &error);
    starter.join();
    ASSERT_TRUE(connected) << error;
    ASSERT_TRUE(client.acquire("fixture", std::chrono::seconds(2), &error)) << error;
    const auto result = client.call("fixture", "echo(QString)",
        {Variant::fromRpc(RpcValue{"late"})}, std::chrono::seconds(2), &error);
    ASSERT_TRUE(result.has_value()) << error;
    EXPECT_EQ(result->value.asString(), "plain:late");
    client.close();
    server.stop();
}

TEST(QtRemotePlainTransportTest, AClientGivesUpOnAnAbsentServerAtItsDeadline)
{
    const std::string path = transportSocketPath();
    Client client;
    std::string error;
    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(client.connect(path, std::chrono::milliseconds(400), &error));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_GE(elapsed, std::chrono::milliseconds(350));
    EXPECT_LT(elapsed, std::chrono::seconds(2));
    EXPECT_FALSE(error.empty());
}

// Detector: a call that looked its object up after a RemoveObject raced it
// threw std::out_of_range, which the C ABI turned into a terminate.
TEST(QtRemotePlainTransportTest, ACallRacingAnUnpublishFailsInsteadOfThrowing)
{
    const std::string path = transportSocketPath();
    Server server;
    ASSERT_TRUE(server.publish(echoFixture()));
    std::string error;
    ASSERT_TRUE(server.start(path, &error)) << error;
    Client client;
    ASSERT_TRUE(client.connect(path, std::chrono::seconds(2), &error)) << error;

    std::atomic<bool> churning{true};
    std::thread churn([&] {
        while (churning) {
            server.unpublish("fixture");
            (void)server.publish(echoFixture());
        }
    });
    for (int i = 0; i < 500; ++i) {
        std::string callError;
        EXPECT_NO_THROW((void)client.call("fixture", "echo(QString)",
            {Variant::fromRpc(RpcValue{"x"})}, std::chrono::milliseconds(200), &callError));
    }
    churning = false;
    churn.join();
    client.close();
    server.stop();
}

TEST(QtRemotePlainTransportTest, RelativeNamesUseTheProcessTempDirectory)
{
#ifdef _WIN32
    EXPECT_EQ(localSocketPath("local:logos_test"), R"(\\.\pipe\logos_test)");
    EXPECT_EQ(localSocketPath(R"(local:\\.\pipe\logos_test)"),
              R"(\\.\pipe\logos_test)");
#else
    const char* temp = std::getenv("TMPDIR");
    std::string expected = temp && *temp ? temp : "";
#ifdef __APPLE__
    if (expected.empty()) {
        const std::size_t required = ::confstr(_CS_DARWIN_USER_TEMP_DIR, nullptr, 0);
        if (required > 1) {
            std::string buffer(required, '\0');
            if (::confstr(_CS_DARWIN_USER_TEMP_DIR, buffer.data(), required) > 0)
                expected = buffer.c_str();
        }
    }
#endif
    if (expected.empty()) expected = "/tmp";
    while (expected.size() > 1 && expected.back() == '/') expected.pop_back();
    EXPECT_EQ(localSocketPath("local:logos_test"), expected + "/logos_test");
    EXPECT_EQ(localSocketPath("local:/tmp/logos_test"), "/tmp/logos_test");
#endif
}

TEST(QtRemotePlainTransportTest, StopWaitsForAnOutstandingHandler)
{
    const std::string path = transportSocketPath();
    Server server;
    std::mutex handlerMutex;
    std::condition_variable handlerChanged;
    bool entered = false;
    bool release = false;
    Server::Object object;
    object.name = "fixture";
    object.definition = {
        "Fixture", {}, {{"block()", "QString", {}}}, {},
    };
    object.invoke = [&](std::int32_t, const std::vector<Variant>&) {
        std::unique_lock<std::mutex> lock(handlerMutex);
        entered = true;
        handlerChanged.notify_all();
        handlerChanged.wait(lock, [&] { return release; });
        return Variant::fromRpc(RpcValue{"done"});
    };
    ASSERT_TRUE(server.publish(std::move(object)));
    std::string error;
    ASSERT_TRUE(server.start(path, &error)) << error;

    Client client;
    ASSERT_TRUE(client.connect(path, std::chrono::seconds(2), &error)) << error;
    ASSERT_TRUE(client.acquire("fixture", std::chrono::seconds(2), &error)) << error;
    std::thread call([&] {
        std::string callError;
        (void)client.call("fixture", "block()", {}, std::chrono::seconds(2), &callError);
    });
    {
        std::unique_lock<std::mutex> lock(handlerMutex);
        ASSERT_TRUE(handlerChanged.wait_for(lock, std::chrono::seconds(1), [&] {
            return entered;
        }));
    }

    std::atomic<bool> stopped{false};
    std::thread stopper([&] {
        server.stop();
        stopped = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(stopped.load());
    {
        std::lock_guard<std::mutex> lock(handlerMutex);
        release = true;
    }
    handlerChanged.notify_all();
    stopper.join();
    EXPECT_TRUE(stopped.load());
    call.join();
    client.close();
}

TEST(QtRemotePlainTransportTest, ConcurrentCallsOnOneClientCanOverlap)
{
    const std::string path = transportSocketPath();
    Server server;
    std::mutex handlerMutex;
    std::condition_variable handlerChanged;
    bool slowEntered = false;
    bool releaseSlow = false;
    Server::Object object;
    object.name = "fixture";
    object.definition = {
        "Fixture", {},
        {{"slow()", "QString", {}}, {"quick()", "QString", {}}}, {},
    };
    object.invoke = [&](std::int32_t index, const std::vector<Variant>&) {
        if (index == 1) return Variant::fromRpc(RpcValue{"quick"});
        std::unique_lock<std::mutex> lock(handlerMutex);
        slowEntered = true;
        handlerChanged.notify_all();
        handlerChanged.wait(lock, [&] { return releaseSlow; });
        return Variant::fromRpc(RpcValue{"slow"});
    };
    ASSERT_TRUE(server.publish(std::move(object)));
    std::string error;
    ASSERT_TRUE(server.start(path, &error)) << error;
    Client client;
    ASSERT_TRUE(client.connect(path, std::chrono::seconds(2), &error)) << error;
    ASSERT_TRUE(client.acquire("fixture", std::chrono::seconds(2), &error)) << error;

    std::optional<Variant> slow;
    std::thread slowCall([&] {
        std::string slowError;
        slow = client.call("fixture", "slow()", {}, std::chrono::seconds(2), &slowError);
    });
    {
        std::unique_lock<std::mutex> lock(handlerMutex);
        ASSERT_TRUE(handlerChanged.wait_for(lock, std::chrono::seconds(1), [&] {
            return slowEntered;
        }));
    }
    const auto quick = client.call(
        "fixture", "quick()", {}, std::chrono::milliseconds(500), &error);
    ASSERT_TRUE(quick.has_value()) << error;
    EXPECT_EQ(quick->value.asString(), "quick");

    {
        std::lock_guard<std::mutex> lock(handlerMutex);
        releaseSlow = true;
    }
    handlerChanged.notify_all();
    slowCall.join();
    ASSERT_TRUE(slow.has_value());
    EXPECT_EQ(slow->value.asString(), "slow");
    client.close();
    server.stop();
}

} // namespace
