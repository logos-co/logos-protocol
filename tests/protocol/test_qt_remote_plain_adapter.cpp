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
#include <QJsonValue>
#include <QRemoteObjectHost>
#include <QTimer>
#include <QThread>

#include <atomic>
#include <chrono>
#include <functional>
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

class RealQroProvider final : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE QVariant callRemoteMethod(const QString&, const QString& method,
                                          const QVariantList& args)
    {
        ++calls;
        if (method == QStringLiteral("deferred")) {
            const QString id = QStringLiteral("call-%1").arg(++nextId);
            QTimer::singleShot(20, this, [this, id] {
                emit eventResponse(QStringLiteral("__logos_call_complete__"),
                                   {id, QStringLiteral("completed")});
            });
            return QVariantMap{{QStringLiteral("__logos_pending_call__"), id}};
        }
        if (method == QStringLiteral("never"))
            return QVariantMap{{QStringLiteral("__logos_pending_call__"),
                                QStringLiteral("never")}};
        if (method == QStringLiteral("undefined"))
            return QVariant::fromValue(QJsonValue(QJsonValue::Undefined));
        if (method == QStringLiteral("nestedResult"))
            return QVariantList{QVariant::fromValue(LogosResult{true, 42, QVariant{}})};
        if (method == QStringLiteral("argumentType") && !args.isEmpty())
            return QString::fromLatin1(args[0].typeName());
        return QStringLiteral("immediate");
    }

    std::atomic<int> calls{0};

signals:
    void eventResponse(const QString&, const QVariantList&);

private:
    int nextId = 0;
};

std::string adapterSocket()
{
#ifdef _WIN32
    return "local:logos_qt_remote_plain_adapter";
#else
    return "local:/tmp/logos_qt_remote_plain_adapter_" + std::to_string(::getpid());
#endif
}

QString realQroSocket()
{
    static std::atomic<unsigned> serial{0};
    return QStringLiteral("local:logos_qt_remote_plain_real_%1_%2")
        .arg(QCoreApplication::applicationPid()).arg(serial.fetch_add(1));
}

bool spinUntil(const std::function<bool()>& done, int timeoutMs = 3000)
{
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(5);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (done()) loop.quit();
    });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();
    return done();
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

TEST(QtRemotePlainAdapterTest, RealQroCallsKeepDeferredControlAndNestedTypes)
{
    qRegisterMetaType<LogosResult>("LogosResult");
    RealQroProvider provider;
    QRemoteObjectHost host;
    const QString url = realQroSocket();
    ASSERT_TRUE(host.setHostUrl(QUrl(url)));
    ASSERT_TRUE(host.enableRemoting(&provider, QStringLiteral("fixture")));

    logos::qt_remote_plain::QtRemotePlainTransportConnection connection(url);
    auto acquisition = std::async(std::launch::async, [&] {
        return connection.requestObject(QStringLiteral("fixture"), 1000);
    });
    ASSERT_TRUE(spinUntil([&] {
        return acquisition.wait_for(std::chrono::milliseconds(0))
            == std::future_status::ready;
    }));
    LogosObject* object = acquisition.get();
    ASSERT_NE(object, nullptr);

    const auto invoke = [&](const QString& method, const QVariantList& args = {}) {
        auto call = std::async(std::launch::async, [&, method, args] {
            return object->callMethod(QStringLiteral("secret"), method, args, 500);
        });
        EXPECT_TRUE(spinUntil([&] {
            return call.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
        }));
        return call.get();
    };

    const QJsonValue undefined = invoke(QStringLiteral("undefined")).value<QJsonValue>();
    EXPECT_TRUE(undefined.isUndefined());
    EXPECT_FALSE(undefined.isNull());

    const QVariantList nested = invoke(QStringLiteral("nestedResult")).toList();
    ASSERT_EQ(nested.size(), 1);
    EXPECT_STREQ(nested[0].typeName(), "LogosResult");
    EXPECT_EQ(nested[0].value<LogosResult>().value.toInt(), 42);
    EXPECT_EQ(invoke(QStringLiteral("argumentType"),
                     {QVariant::fromValue(LogosResult{true, 42, QVariant{}})}).toString(),
              QStringLiteral("LogosResult"));

    std::promise<std::pair<QVariant, logos::CallError>> eventResult;
    auto eventFuture = eventResult.get_future();
    object->onEvent(QStringLiteral("tick"), [&](const QString&, const QVariantList&) {
        logos::CallError error;
        QVariant value = dynamic_cast<LogosObjectErrorChannel*>(object)->callMethodWithError(
            QStringLiteral("secret"), QStringLiteral("deferred"), {}, 500, &error);
        eventResult.set_value({std::move(value), std::move(error)});
    });
    emit provider.eventResponse(QStringLiteral("tick"), {});
    ASSERT_TRUE(spinUntil([&] {
        return eventFuture.wait_for(std::chrono::milliseconds(0))
            == std::future_status::ready;
    }));
    const auto [eventValue, eventError] = eventFuture.get();
    EXPECT_EQ(eventValue.toString(), QStringLiteral("completed"));
    EXPECT_TRUE(eventError.ok());
    object->release();
}

TEST(QtRemotePlainAdapterTest, AsyncResultsReturnToQtThreadAndRespectRelease)
{
    RealQroProvider provider;
    QRemoteObjectHost host;
    const QString url = realQroSocket();
    ASSERT_TRUE(host.setHostUrl(QUrl(url)));
    ASSERT_TRUE(host.enableRemoting(&provider, QStringLiteral("fixture")));
    logos::qt_remote_plain::QtRemotePlainTransportConnection connection(url);

    const auto acquire = [&] {
        auto pending = std::async(std::launch::async, [&] {
            return connection.requestObject(QStringLiteral("fixture"), 1000);
        });
        EXPECT_TRUE(spinUntil([&] {
            return pending.wait_for(std::chrono::milliseconds(0))
                == std::future_status::ready;
        }));
        return pending.get();
    };
    LogosObject* object = acquire();
    ASSERT_NE(object, nullptr);

    struct Delivery {
        QVariant value;
        logos::CallError error;
        bool onQtThread = false;
    };
    const auto invokeAsync = [&](const QString& method, int timeoutMs) {
        auto promise = std::make_shared<std::promise<Delivery>>();
        auto future = promise->get_future();
        dynamic_cast<LogosObjectErrorChannel*>(object)->callMethodAsyncWithError(
            QStringLiteral("secret"), method, {}, timeoutMs,
            [promise](QVariant value, const logos::CallError& error) {
                promise->set_value({std::move(value), error,
                    QThread::currentThread() == QCoreApplication::instance()->thread()});
            });
        EXPECT_TRUE(spinUntil([&] {
            return future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
        }));
        return future.get();
    };

    const Delivery immediate = invokeAsync(QStringLiteral("immediate"), 500);
    EXPECT_TRUE(immediate.onQtThread);
    EXPECT_EQ(immediate.value.toString(), QStringLiteral("immediate"));
    EXPECT_TRUE(immediate.error.ok());

    const Delivery deferred = invokeAsync(QStringLiteral("deferred"), 500);
    EXPECT_TRUE(deferred.onQtThread);
    EXPECT_EQ(deferred.value.toString(), QStringLiteral("completed"));
    EXPECT_TRUE(deferred.error.ok());

    const Delivery timeout = invokeAsync(QStringLiteral("never"), 50);
    EXPECT_TRUE(timeout.onQtThread);
    EXPECT_FALSE(timeout.value.isValid());
    EXPECT_FALSE(timeout.error.ok());

    object->release();
    object = acquire();
    ASSERT_NE(object, nullptr);
    std::atomic<bool> delivered{false};
    const int callsBefore = provider.calls.load();
    object->callMethodAsync(QStringLiteral("secret"), QStringLiteral("immediate"), {}, 500,
                            [&](QVariant) { delivered = true; });
    object->release();
    ASSERT_TRUE(spinUntil([&] { return provider.calls.load() > callsBefore; }));
    QEventLoop settle;
    QTimer::singleShot(100, &settle, &QEventLoop::quit);
    settle.exec();
    EXPECT_FALSE(delivered.load());
}
#endif

} // namespace

#include "test_qt_remote_plain_adapter.moc"
