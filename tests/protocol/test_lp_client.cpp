#include <gtest/gtest.h>

#include "logos_protocol.h"
#include "logos_mock.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QVariant>

#include <atomic>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

// Test-support hook (defined in logos_protocol.cpp, not part of the public C
// ABI): the thread the lp client's QObjects are affined to.
QThread* lp_client_owner_thread(lp_client* client);

// Exercises the consumer C ABI end-to-end over the mock transport — the
// same calls any non-C++ SDK makes. No C++ SDK types cross these tests'
// assertions: everything goes through lp_* and JSON strings.

namespace {

struct LpClientGuard {
    explicit LpClientGuard(lp_client* c) : client(c) {}
    ~LpClientGuard() { lp_client_destroy(client); }
    lp_client* client;
};

nlohmann::json parsed(const char* json)
{
    return nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
}

} // namespace

class LpClientTest : public ::testing::Test {
protected:
    void SetUp() override { m_mock = new LogosMockSetup(); }
    void TearDown() override { delete m_mock; }
    LogosMockSetup* m_mock = nullptr;
};

TEST_F(LpClientTest, ModeRoundTrip)
{
    // LogosMockSetup already switched to mock; the C ABI must see it.
    EXPECT_STREQ(lp_get_mode(), "mock");
    EXPECT_EQ(lp_set_mode("local"), LP_OK);
    EXPECT_STREQ(lp_get_mode(), "local");
    EXPECT_EQ(lp_set_mode("mock"), LP_OK);
    EXPECT_EQ(lp_set_mode("bogus"), LP_ERR_INVALID_ARG);
    EXPECT_EQ(lp_set_mode(nullptr), LP_ERR_INVALID_ARG);
}

TEST_F(LpClientTest, InvokeReturnsMockedValue)
{
    m_mock->when("test_module", "getValue").thenReturn(QVariant(42));

    lp_client* client = lp_client_create("test_module", "origin", nullptr, nullptr);
    ASSERT_NE(client, nullptr);
    LpClientGuard guard(client);

    char* result = nullptr;
    char* error = nullptr;
    ASSERT_EQ(lp_invoke(client, "getValue", nullptr, 0, &result, &error), LP_OK);
    EXPECT_EQ(error, nullptr);
    ASSERT_NE(result, nullptr);

    nlohmann::json j = parsed(result);
    ASSERT_TRUE(j.is_number());
    EXPECT_DOUBLE_EQ(j.get<double>(), 42.0);
    lp_string_free(result);

    EXPECT_TRUE(m_mock->wasCalled("test_module", "getValue"));
}

TEST_F(LpClientTest, InvokePassesJsonArgs)
{
    m_mock->when("mod", "echo").thenReturn(QVariant("hello back"));

    lp_client* client = lp_client_create("mod", "origin", nullptr, nullptr);
    ASSERT_NE(client, nullptr);
    LpClientGuard guard(client);

    char* result = nullptr;
    ASSERT_EQ(lp_invoke(client, "echo", R"(["hello", 7, true])", 0, &result, nullptr),
              LP_OK);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(parsed(result).get<std::string>(), "hello back");
    lp_string_free(result);

    const QVariantList args = m_mock->lastArgs("mod", "echo");
    ASSERT_EQ(args.size(), 3);
    EXPECT_EQ(args[0].toString(), "hello");
    EXPECT_EQ(args[1].toLongLong(), 7);
    EXPECT_EQ(args[2].toBool(), true);
}

TEST_F(LpClientTest, BytesRoundTripThroughTheCAbi)
{
    // {"_bytes": base64url} in args must reach the callee as QByteArray,
    // and QByteArray results must come back tagged — NUL included.
    const QByteArray payload("x\0y", 3);
    m_mock->when("mod", "store").thenReturn(QVariant(payload));

    lp_client* client = lp_client_create("mod", "origin", nullptr, nullptr);
    ASSERT_NE(client, nullptr);
    LpClientGuard guard(client);

    char* result = nullptr;
    ASSERT_EQ(lp_invoke(client, "store", R"([{"_bytes":"eAB5"}])", 0, &result, nullptr),
              LP_OK);
    ASSERT_NE(result, nullptr);

    nlohmann::json j = parsed(result);
    ASSERT_TRUE(j.is_object());
    ASSERT_TRUE(j.contains("_bytes"));
    EXPECT_EQ(j["_bytes"].get<std::string>(), "eAB5");  // "x\0y" base64url
    lp_string_free(result);

    const QVariantList args = m_mock->lastArgs("mod", "store");
    ASSERT_EQ(args.size(), 1);
    EXPECT_EQ(args[0].toByteArray(), payload);
}

TEST_F(LpClientTest, InvokeAsyncDeliversResult)
{
    m_mock->when("mod", "compute").thenReturn(QVariant(7));

    lp_client* client = lp_client_create("mod", "origin", nullptr, nullptr);
    ASSERT_NE(client, nullptr);
    LpClientGuard guard(client);

    struct Capture {
        std::atomic<bool> done{false};
        int ok = 0;
        std::string json;
    } capture;

    auto cb = [](int ok, const char* json, void* user_data) {
        auto* c = static_cast<Capture*>(user_data);
        c->ok = ok;
        c->json = json ? json : "";
        c->done = true;
    };

    ASSERT_EQ(lp_invoke_async(client, "compute", "[]", 0, cb, &capture), LP_OK);

    QElapsedTimer timer;
    timer.start();
    while (!capture.done && timer.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    ASSERT_TRUE(capture.done) << "async result not delivered within 5s";
    EXPECT_EQ(capture.ok, 1);
    EXPECT_DOUBLE_EQ(parsed(capture.json.c_str()).get<double>(), 7.0);
}

TEST_F(LpClientTest, InvalidArgsJsonIsRejected)
{
    lp_client* client = lp_client_create("mod", "origin", nullptr, nullptr);
    ASSERT_NE(client, nullptr);
    LpClientGuard guard(client);

    char* result = nullptr;
    char* error = nullptr;
    EXPECT_EQ(lp_invoke(client, "m", "not json", 0, &result, &error),
              LP_ERR_INVALID_ARG);
    EXPECT_EQ(result, nullptr);
    ASSERT_NE(error, nullptr);
    nlohmann::json e = parsed(error);
    EXPECT_EQ(e["code"].get<std::string>(), "invalid_args");
    EXPECT_FALSE(e["message"].get<std::string>().empty());
    lp_string_free(error);

    // A JSON value that is not an array is rejected too.
    EXPECT_EQ(lp_invoke(client, "m", R"({"a":1})", 0, nullptr, nullptr),
              LP_ERR_INVALID_ARG);
}

TEST_F(LpClientTest, NullArgumentsAreRejected)
{
    EXPECT_EQ(lp_client_create(nullptr, "origin", nullptr, nullptr), nullptr);
    EXPECT_EQ(lp_client_create("", "origin", nullptr, nullptr), nullptr);
    EXPECT_EQ(lp_client_create("t", nullptr, nullptr, nullptr), nullptr);

    char* error = nullptr;
    EXPECT_EQ(lp_invoke(nullptr, "m", "[]", 0, nullptr, &error),
              LP_ERR_INVALID_ARG);
    ASSERT_NE(error, nullptr);
    lp_string_free(error);

    EXPECT_EQ(lp_invoke_async(nullptr, "m", "[]", 0,
                              [](int, const char*, void*) {}, nullptr),
              LP_ERR_INVALID_ARG);
    EXPECT_EQ(lp_subscribe(nullptr, "e", [](const char*, const char*, void*) {},
                           nullptr),
              nullptr);
    lp_unsubscribe(nullptr);   // no-op
    lp_client_destroy(nullptr);  // no-op
}

TEST_F(LpClientTest, MalformedTransportJsonFailsCreate)
{
    EXPECT_EQ(lp_client_create("t", "o", "not json", nullptr), nullptr);
    EXPECT_EQ(lp_client_create("t", "o", nullptr, "not json"), nullptr);
}

TEST_F(LpClientTest, TokenStoreRoundTrip)
{
    EXPECT_EQ(lp_token_save("some_module", "tok-123"), LP_OK);
    char* token = lp_token_get("some_module");
    ASSERT_NE(token, nullptr);
    EXPECT_STREQ(token, "tok-123");
    lp_string_free(token);

    EXPECT_EQ(lp_token_get("module_without_token"), nullptr);
    EXPECT_EQ(lp_token_save(nullptr, "t"), LP_ERR_INVALID_ARG);
    EXPECT_EQ(lp_token_get(nullptr), nullptr);
}

TEST_F(LpClientTest, ProviderGroundworkSurface)
{
    // Provider C ABI: constructible + callbacks registrable, but serving is
    // explicitly deferred (LP_ERR_UNSUPPORTED) until module authoring lands.
    lp_provider* provider = lp_provider_create("my_module", nullptr);
    ASSERT_NE(provider, nullptr);

    auto dispatch = [](const char*, const char*, void*) -> char* { return nullptr; };
    EXPECT_EQ(lp_provider_register(provider, dispatch, nullptr, nullptr, nullptr),
              LP_OK);
    EXPECT_EQ(lp_provider_emit_event(provider, "e", "[]"), LP_ERR_UNSUPPORTED);
    EXPECT_EQ(lp_provider_save_token(provider, "m", "t"), LP_ERR_UNSUPPORTED);
    lp_provider_destroy(provider);

    EXPECT_EQ(lp_provider_create(nullptr, nullptr), nullptr);
    EXPECT_EQ(lp_provider_register(nullptr, dispatch, nullptr, nullptr, nullptr),
              LP_ERR_INVALID_ARG);
    lp_provider_destroy(nullptr);  // no-op
}

// A client created off the main thread (as a "multi" module does — its glue
// dispatches each call on a worker std::thread, and the first outbound call
// lazily creates the client there) must still be owned by the application's
// event-loop thread. Otherwise a synchronous outbound call to another "multi"
// module would wait for the deferred completion in a nested QEventLoop on a
// loop-less worker and time out. See lp_client_create in logos_protocol.cpp.
TEST_F(LpClientTest, ClientCreatedOffMainThreadIsOwnedByAppThread)
{
    QThread* const appThread = QCoreApplication::instance()->thread();
    ASSERT_EQ(QThread::currentThread(), appThread);  // the test runs on it

    lp_client* client = nullptr;
    QThread* workerThread = nullptr;
    std::thread worker([&]() {
        workerThread = QThread::currentThread();
        client = lp_client_create("test_module", "origin", nullptr, nullptr);
    });
    worker.join();

    ASSERT_NE(client, nullptr);
    LpClientGuard guard(client);
    EXPECT_NE(workerThread, appThread);                       // it really was off-thread
    EXPECT_EQ(lp_client_owner_thread(client), appThread);     // …yet re-homed to the app thread
    EXPECT_NE(lp_client_owner_thread(client), workerThread);
}
