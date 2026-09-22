#include "implementations/qt_remote_plain/qt_remote_plain_transport.h"
#include "logos_mode.h"
#include "logos_object.h"
#include "logos_provider_interface.h"
#include "logos_types.h"
#include "logos_transport_config_json.h"
#include "logos_transport_factory.h"
#include "module_proxy.h"
#include "token_manager.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QThread>

#include <atomic>
#include <chrono>
#include <future>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

class AdapterProvider final : public LogosProviderObject {
public:
    QVariant callMethod(const QString& methodName, const QVariantList& args) override
    {
        if (methodName == QStringLiteral("echo") && args.size() == 1)
            return QStringLiteral("adapter:") + args.front().toString();
        return {};
    }
    QJsonArray getMethods() override { return {}; }
    void setEventListener(EventCallback callback) override { listener = std::move(callback); }
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("fixture"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }
    bool informModuleToken(const QString& moduleName, const QString& token) override
    {
        informedModule = moduleName;
        informedToken = token;
        return true;
    }

    void fire(const QString& name, const QVariantList& data) { if (listener) listener(name, data); }
    QString informedModule;
    QString informedToken;
    EventCallback listener;
};

std::string adapterSocket()
{
#ifdef _WIN32
    return "local:logos_qt_remote_plain_adapter";
#else
    return "local:/tmp/logos_qt_remote_plain_adapter_" + std::to_string(::getpid());
#endif
}

TEST(QtRemotePlainAdapterTest, ConfigRoundTripsAndFactorySelectsAQtFreeConnection)
{
    LogosTransportConfig config;
    config.protocol = LogosProtocol::QtRemotePlain;
    const auto json = logos::transportSetToJsonString({config});
    EXPECT_NE(json.find("qt_remote_plain"), std::string::npos);
    const auto parsed = logos::transportSetFromJsonString(json);
    ASSERT_EQ(parsed.size(), 1u);
    EXPECT_EQ(parsed.front().protocol, LogosProtocol::QtRemotePlain);

    LogosModeConfig::setMode(LogosMode::Remote);
    auto connection = LogosTransportFactory::createConnection(config, "local:missing");
    ASSERT_NE(connection, nullptr);
    EXPECT_FALSE(LogosTransportFactory::needsQtEventLoop(config));
}

#ifndef _WIN32
TEST(QtRemotePlainAdapterTest, ExistingModuleProxyCallsTokensAndEventsUseThePlainWire)
{
    qRegisterMetaType<LogosResult>("LogosResult");
    AdapterProvider provider;
    TokenManager& tokens = TokenManager::instance();
    tokens.clearAllTokens();
    tokens.adoptCredential(QStringLiteral("secret"));
    ModuleProxy proxy(&provider, nullptr, &tokens);
    ASSERT_TRUE(proxy.saveToken(QStringLiteral("caller"), QStringLiteral("secret")));

    const QString url = QString::fromStdString(adapterSocket());
    logos::qt_remote_plain::QtRemotePlainTransportHost host(url);
    ASSERT_TRUE(host.publishObject(QStringLiteral("fixture"), &proxy));
    logos::qt_remote_plain::QtRemotePlainTransportConnection connection(url);
    ASSERT_TRUE(connection.connectToHost());
    LogosObject* object = connection.requestObject(QStringLiteral("fixture"), 1000);
    ASSERT_NE(object, nullptr);

    auto calls = std::async(std::launch::async, [object] {
        const QVariant echo = object->callMethod(
            QStringLiteral("secret"), QStringLiteral("echo"),
            {QStringLiteral("hello")}, 1000);
        const bool informed = object->informModuleToken(
            QStringLiteral("secret"), QStringLiteral("peer"),
            QStringLiteral("peer-token"), 1000);
        return std::make_pair(echo, informed);
    });
    QEventLoop callLoop;
    QTimer callPoll;
    callPoll.setInterval(5);
    QObject::connect(&callPoll, &QTimer::timeout, &callLoop, [&] {
        if (calls.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            callLoop.quit();
    });
    QTimer::singleShot(3000, &callLoop, &QEventLoop::quit);
    callPoll.start();
    callLoop.exec();
    callPoll.stop();
    ASSERT_EQ(calls.wait_for(std::chrono::milliseconds(0)), std::future_status::ready);
    const auto [echo, informed] = calls.get();
    EXPECT_EQ(echo.toString(), QStringLiteral("adapter:hello"));
    EXPECT_TRUE(informed);
    EXPECT_EQ(provider.informedModule, QStringLiteral("peer"));
    EXPECT_EQ(provider.informedToken, QStringLiteral("peer-token"));

    std::atomic<bool> received{false};
    object->onEvent(QStringLiteral("tick"), [&](const QString&, const QVariantList& data) {
        received = data == QVariantList{QStringLiteral("payload")};
    });
    provider.fire(QStringLiteral("tick"), {QStringLiteral("payload")});
    QEventLoop eventLoop;
    QTimer eventPoll;
    eventPoll.setInterval(5);
    QObject::connect(&eventPoll, &QTimer::timeout, &eventLoop, [&] {
        if (received.load()) eventLoop.quit();
    });
    QTimer::singleShot(1000, &eventLoop, &QEventLoop::quit);
    eventPoll.start();
    eventLoop.exec();
    EXPECT_TRUE(received.load());
    object->release();
    tokens.clearAllTokens();
}
#endif

} // namespace
