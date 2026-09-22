#include "implementations/qt_remote_plain/qtro_transport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <string>

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

TEST(QtRemotePlainTransportTest, RelativeNamesUseTheProcessTempDirectory)
{
#ifdef _WIN32
    EXPECT_EQ(localSocketPath("local:logos_test"), R"(\\.\pipe\logos_test)");
    EXPECT_EQ(localSocketPath(R"(local:\\.\pipe\logos_test)"),
              R"(\\.\pipe\logos_test)");
#else
    const char* temp = std::getenv("TMPDIR");
    std::string expected = temp && *temp ? temp : "/tmp";
    while (expected.size() > 1 && expected.back() == '/') expected.pop_back();
    EXPECT_EQ(localSocketPath("local:logos_test"), expected + "/logos_test");
    EXPECT_EQ(localSocketPath("local:/tmp/logos_test"), "/tmp/logos_test");
#endif
}

} // namespace
