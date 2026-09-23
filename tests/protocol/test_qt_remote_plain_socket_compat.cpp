#include "implementations/qt_remote_plain/qtro_wire.h"
#include "implementations/qt_remote_plain/qtro_transport.h"
#include "logos_types.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonValue>
#include <QMetaObject>
#include <QRemoteObjectDynamicReplica>
#include <QRemoteObjectHost>
#include <QRemoteObjectNode>
#include <QRemoteObjectPendingCall>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include <atomic>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

using namespace logos::plain;
using namespace logos::qt_remote_plain;

namespace {

#ifndef _WIN32

class UniqueFd {
public:
    explicit UniqueFd(int fd = -1) : m_fd(fd) {}
    ~UniqueFd() { if (m_fd >= 0) ::close(m_fd); }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : m_fd(other.m_fd) { other.m_fd = -1; }
    int get() const { return m_fd; }

private:
    int m_fd;
};

void writeAll(int fd, const std::vector<std::uint8_t>& data)
{
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto count = ::send(fd, data.data() + offset, data.size() - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("socket write failed");
        offset += static_cast<std::size_t>(count);
    }
}

void readExact(int fd, std::uint8_t* output, std::size_t size)
{
    std::size_t offset = 0;
    while (offset < size) {
        const auto count = ::recv(fd, output + offset, size - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("socket read failed");
        offset += static_cast<std::size_t>(count);
    }
}

std::vector<std::uint8_t> readFrame(int fd)
{
    std::vector<std::uint8_t> frame(4);
    readExact(fd, frame.data(), 4);
    const std::uint32_t payload = static_cast<std::uint32_t>(frame[0])
        | (static_cast<std::uint32_t>(frame[1]) << 8)
        | (static_cast<std::uint32_t>(frame[2]) << 16)
        | (static_cast<std::uint32_t>(frame[3]) << 24);
    if (payload > (64u << 20)) throw std::runtime_error("oversized frame");
    frame.resize(4 + payload);
    readExact(fd, frame.data() + 4, payload);
    return frame;
}

std::string uniqueSocketPath(const char* suffix)
{
    static std::atomic<unsigned> serial{0};
    return "/tmp/logos_qtro_plain_" + std::to_string(::getpid()) + "_"
        + std::to_string(serial.fetch_add(1)) + "_" + suffix;
}

UniqueFd connectUnix(const std::string& path)
{
    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (fd.get() < 0) throw std::runtime_error("socket() failed");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path))
        throw std::runtime_error("socket path too long");
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        throw std::runtime_error("connect() failed: " + std::string(std::strerror(errno)));
    return fd;
}

class UnixListener {
public:
    explicit UnixListener(std::string path) : m_path(std::move(path)), m_fd(::socket(AF_UNIX, SOCK_STREAM, 0))
    {
        if (m_fd.get() < 0) throw std::runtime_error("socket() failed");
        ::unlink(m_path.c_str());
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (m_path.size() >= sizeof(address.sun_path))
            throw std::runtime_error("socket path too long");
        std::memcpy(address.sun_path, m_path.c_str(), m_path.size() + 1);
        if (::bind(m_fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            throw std::runtime_error("bind() failed");
        if (::listen(m_fd.get(), 4) != 0)
            throw std::runtime_error("listen() failed");
    }

    ~UnixListener() { ::unlink(m_path.c_str()); }

    UniqueFd acceptOne()
    {
        const int fd = ::accept(m_fd.get(), nullptr, nullptr);
        if (fd < 0) throw std::runtime_error("accept() failed");
        return UniqueFd(fd);
    }

private:
    std::string m_path;
    UniqueFd m_fd;
};

class QtWireFixture : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE QString echo(const QString& value) { return QStringLiteral("qt:") + value; }

signals:
    void tick(const QString& value);
};

class EventSink : public QObject {
    Q_OBJECT
public:
    QString name;
    QVariantList data;

public slots:
    void receive(const QString& eventName, const QVariantList& eventData)
    {
        name = eventName;
        data = eventData;
    }
};

struct PlainClientResult {
    ClassDefinition definition;
    std::int32_t replySerial = -1;
    Variant reply;
    std::int32_t eventSignalIndex = -1;
    std::vector<Variant> eventArgs;
};

TEST(QtRemotePlainSocketCompatTest, PlainClientCallsAndReceivesEventsFromQtHost)
{
    const std::string path = uniqueSocketPath("qt_host");
    const QUrl url(QStringLiteral("local:") + QString::fromStdString(path));
    QRemoteObjectHost host;
    ASSERT_TRUE(host.setHostUrl(url));
    QtWireFixture fixture;
    ASSERT_TRUE(host.enableRemoting(&fixture, QStringLiteral("fixture")));

    auto resultFuture = std::async(std::launch::async, [&] {
        PlainClientResult result;
        auto client = connectUnix(path);
        const Frame handshake = decodeFrame(readFrame(client.get()));
        if (handshake.type != PacketType::Handshake || handshake.name != "QtRO 2.0")
            throw std::runtime_error("bad server handshake");

        const Frame objects = decodeFrame(readFrame(client.get()));
        if (objects.type != PacketType::ObjectList)
            throw std::runtime_error("missing object list");
        Reader objectPayload(objects.payload);
        const auto objectCount = objectPayload.u32();
        const auto objectName = objectPayload.string().value_or("<null>");
        const auto typeName = objectPayload.string();
        const auto signature = objectPayload.bytes().value_or(std::vector<std::uint8_t>{});
        if (objectCount != 1 || objectName != "fixture" || typeName.has_value())
            throw std::runtime_error("unexpected object list: count="
                + std::to_string(objectCount) + " name=" + objectName
                + " type=" + typeName.value_or("<null>")
                + " signature=" + std::to_string(signature.size()));

        writeAll(client.get(), addObjectPacket("fixture"));
        const Frame init = decodeFrame(readFrame(client.get()));
        if (init.type != PacketType::InitDynamic || init.name != "fixture")
            throw std::runtime_error("missing dynamic definition");
        Reader initPayload(init.payload);
        result.definition = initPayload.classDefinition();
        if (initPayload.u32() != 0)
            throw std::runtime_error("unexpected properties");

        const auto method = std::find_if(result.definition.methodDefinitions.begin(),
                                         result.definition.methodDefinitions.end(),
            [](const MethodDefinition& value) { return value.signature == "echo(QString)"; });
        if (method == result.definition.methodDefinitions.end())
            throw std::runtime_error("echo method is absent");
        const auto methodIndex = static_cast<std::int32_t>(
            std::distance(result.definition.methodDefinitions.begin(), method));
        writeAll(client.get(), invokePacket("fixture", 0, methodIndex,
            {Variant::fromRpc(RpcValue{"hello"})}, 41));

        const Frame reply = decodeFrame(readFrame(client.get()));
        if (reply.type != PacketType::InvokeReply)
            throw std::runtime_error("missing invocation reply");
        Reader replyPayload(reply.payload);
        result.replySerial = replyPayload.i32();
        result.reply = replyPayload.variant();

        QMetaObject::invokeMethod(&fixture, [&fixture] {
            emit fixture.tick(QStringLiteral("event"));
        }, Qt::QueuedConnection);
        const Frame event = decodeFrame(readFrame(client.get()));
        if (event.type != PacketType::Invoke)
            throw std::runtime_error("missing signal invocation");
        Reader eventPayload(event.payload);
        if (eventPayload.i32() != 0)
            throw std::runtime_error("unexpected signal call kind");
        result.eventSignalIndex = eventPayload.i32();
        const auto count = eventPayload.u32();
        for (std::uint32_t i = 0; i < count; ++i)
            result.eventArgs.push_back(eventPayload.variant());
        return result;
    });

    QEventLoop loop;
    QTimer poll;
    poll.setInterval(5);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (resultFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            loop.quit();
    });
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();
    ASSERT_EQ(resultFuture.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    const PlainClientResult result = resultFuture.get();
    EXPECT_EQ(result.replySerial, 41);
    EXPECT_EQ(result.reply.type, MetaType::String);
    EXPECT_EQ(result.reply.value.asString(), "qt:hello");
    const auto signal = std::find_if(result.definition.signalDefinitions.begin(),
                                     result.definition.signalDefinitions.end(),
        [](const SignalDefinition& value) { return value.signature == "tick(QString)"; });
    ASSERT_NE(signal, result.definition.signalDefinitions.end());
    EXPECT_EQ(result.eventSignalIndex,
              std::distance(result.definition.signalDefinitions.begin(), signal));
    ASSERT_EQ(result.eventArgs.size(), 1u);
    EXPECT_EQ(result.eventArgs[0].value.asString(), "event");
}

TEST(QtRemotePlainSocketCompatTest, QtClientCallsAndReceivesEventsFromPlainServer)
{
    const std::string path = uniqueSocketPath("plain_host");
    UnixListener listener(path);
    std::promise<void> serverDone;
    auto serverResult = serverDone.get_future();
    std::thread server([&] {
        try {
            auto peer = listener.acceptOne();
            writeAll(peer.get(), handshakePacket());
            writeAll(peer.get(), objectListPacket({{"fixture", "ModuleProxy", {}}}));

            const Frame add = decodeFrame(readFrame(peer.get()));
            if (add.type != PacketType::AddObject || add.name != "fixture")
                throw std::runtime_error("unexpected AddObject");
            Reader addPayload(add.payload);
            if (!addPayload.boolean()) throw std::runtime_error("expected dynamic acquire");
            writeAll(peer.get(), initDynamicPacket("fixture", moduleProxyDefinition()));

            const Frame invoke = decodeFrame(readFrame(peer.get()));
            if (invoke.type != PacketType::Invoke || invoke.name != "fixture")
                throw std::runtime_error("unexpected Invoke");
            Reader body(invoke.payload);
            if (body.i32() != 0 || body.i32() != 0 || body.u32() != 3)
                throw std::runtime_error("unexpected callRemoteMethod invocation");
            const auto token = body.variant();
            const auto method = body.variant();
            const auto args = body.variant();
            if (token.value.asString() != "token" || method.value.asString() != "echo"
                || args.value.asList().items.at(0).asString() != "hello")
                throw std::runtime_error("unexpected call arguments");
            const auto serial = body.i32();
            (void)body.i32(); // property index
            writeAll(peer.get(), invokeReplyPacket("fixture", serial,
                Variant::fromRpc(RpcValue{"plain:hello"})));

            // eventResponse is signal index zero in the fixed definition.
            RpcList eventData;
            eventData.items.emplace_back("payload");
            writeAll(peer.get(), invokePacket("fixture", 0, 0,
                {Variant::fromRpc(RpcValue{"tick"}),
                 Variant::fromRpc(RpcValue{std::move(eventData)})}));
            serverDone.set_value();
        } catch (...) {
            serverDone.set_exception(std::current_exception());
        }
    });

    QRemoteObjectNode node;
    ASSERT_TRUE(node.connectToNode(QUrl(QStringLiteral("local:") + QString::fromStdString(path))));
    QSharedPointer<QRemoteObjectDynamicReplica> replica(node.acquireDynamic(QStringLiteral("fixture")));
    ASSERT_TRUE(replica->waitForSource(3000));

    EventSink sink;
    const QMetaObject::Connection eventConnection = QObject::connect(
        replica.data(), SIGNAL(eventResponse(QString,QVariantList)),
        &sink, SLOT(receive(QString,QVariantList)));
    ASSERT_TRUE(eventConnection);

    QRemoteObjectPendingCall pending;
    ASSERT_TRUE(QMetaObject::invokeMethod(
        replica.data(), "callRemoteMethod", Qt::DirectConnection,
        Q_RETURN_ARG(QRemoteObjectPendingCall, pending),
        Q_ARG(QString, QStringLiteral("token")),
        Q_ARG(QString, QStringLiteral("echo")),
        Q_ARG(QVariantList, QVariantList{QStringLiteral("hello")})));
    pending.waitForFinished(3000);
    ASSERT_TRUE(pending.isFinished());
    EXPECT_EQ(pending.returnValue().toString(), QStringLiteral("plain:hello"));

    QEventLoop loop;
    QTimer poll;
    poll.setInterval(5);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (!sink.name.isEmpty()) loop.quit();
    });
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();
    EXPECT_EQ(sink.name, QStringLiteral("tick"));
    ASSERT_EQ(sink.data.size(), 1);
    EXPECT_EQ(sink.data.first().toString(), QStringLiteral("payload"));

    ASSERT_EQ(serverResult.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_NO_THROW(serverResult.get());
    server.join();
}

TEST(QtRemotePlainSocketCompatTest, QtClientAndPlainServerPreserveNestedTypes)
{
    qRegisterMetaType<LogosResult>("LogosResult");
    const std::string path = uniqueSocketPath("plain_nested_types");
    Server server;
    std::string error;
    ASSERT_TRUE(server.start(path, &error)) << error;
    ASSERT_TRUE(server.publish({"fixture", moduleProxyDefinition(),
        [](std::int32_t index, const std::vector<Variant>& arguments) {
            if (index != 0 || arguments.size() != 3
                || !arguments[1].value.isString()) return Variant{};
            const std::string method = arguments[1].value.asString();
            if (method == "undefined")
                return Variant{MetaType::JsonValue, false, RpcValue{}, {}, true};
            if (method == "nestedResult") {
                Variant result = Variant::logosResult(true,
                    Variant::fromRpc(RpcValue{std::int64_t{42}}), Variant{});
                RpcList list;
                list.items.push_back(result.value);
                Variant container = Variant::fromRpc(RpcValue{std::move(list)});
                container.nestedValues[0] = std::move(result);
                return container;
            }
            if (method == "argumentType") {
                const Variant& callArgs = arguments[2];
                const bool isResult = callArgs.type == MetaType::VariantList
                    && callArgs.nestedValues.size() == 1
                    && callArgs.nestedValues[0].type == MetaType::User
                    && callArgs.nestedValues[0].customType == "LogosResult";
                return Variant::fromRpc(RpcValue{isResult ? "LogosResult" : "flattened"});
            }
            return Variant{};
        }}, &error)) << error;

    QRemoteObjectNode node;
    ASSERT_TRUE(node.connectToNode(
        QUrl(QStringLiteral("local:") + QString::fromStdString(path))));
    QSharedPointer<QRemoteObjectDynamicReplica> replica(
        node.acquireDynamic(QStringLiteral("fixture")));
    ASSERT_TRUE(replica->waitForSource(3000));

    const auto invoke = [&](const QString& method, const QVariantList& args = {}) {
        QRemoteObjectPendingCall pending;
        EXPECT_TRUE(QMetaObject::invokeMethod(
            replica.data(), "callRemoteMethod", Qt::DirectConnection,
            Q_RETURN_ARG(QRemoteObjectPendingCall, pending),
            Q_ARG(QString, QStringLiteral("token")),
            Q_ARG(QString, method),
            Q_ARG(QVariantList, args)));
        pending.waitForFinished(3000);
        EXPECT_TRUE(pending.isFinished());
        return pending.returnValue();
    };

    const QJsonValue undefined = invoke(QStringLiteral("undefined")).value<QJsonValue>();
    EXPECT_TRUE(undefined.isUndefined());

    const QVariantList nested = invoke(QStringLiteral("nestedResult")).toList();
    ASSERT_EQ(nested.size(), 1);
    EXPECT_STREQ(nested[0].typeName(), "LogosResult");
    const LogosResult nestedResult = nested[0].value<LogosResult>();
    EXPECT_TRUE(nestedResult.success);
    EXPECT_EQ(nestedResult.value.toInt(), 42);

    const LogosResult argument{true, 42, QVariant{}};
    EXPECT_EQ(invoke(QStringLiteral("argumentType"),
                     {QVariant::fromValue(argument)}).toString(),
              QStringLiteral("LogosResult"));
    server.stop();
}

TEST(QtRemotePlainSocketCompatTest, WaitingQtClientSeesBusinessObjectPublishedAfterInit)
{
    const std::string path = uniqueSocketPath("staged_plain_host");
    Server server;
    ASSERT_TRUE(server.publish({"fixture__handshake", moduleHandshakeProxyDefinition(),
        [](std::int32_t, const std::vector<Variant>&) {
            return Variant::fromRpc(RpcValue{true});
        }}));
    std::string error;
    ASSERT_TRUE(server.start(path, &error)) << error;

    QRemoteObjectNode node;
    ASSERT_TRUE(node.connectToNode(
        QUrl(QStringLiteral("local:") + QString::fromStdString(path))));
    QSharedPointer<QRemoteObjectDynamicReplica> replica(
        node.acquireDynamic(QStringLiteral("fixture")));
    EXPECT_FALSE(replica->waitForSource(100));

    ASSERT_TRUE(server.publish({"fixture", moduleProxyDefinition(),
        [](std::int32_t index, const std::vector<Variant>&) {
            if (index != 0) return Variant{};
            return Variant::fromRpc(RpcValue{"ready"});
        }}, &error)) << error;
    ASSERT_TRUE(replica->waitForSource(3000));

    QRemoteObjectPendingCall pending;
    ASSERT_TRUE(QMetaObject::invokeMethod(
        replica.data(), "callRemoteMethod", Qt::DirectConnection,
        Q_RETURN_ARG(QRemoteObjectPendingCall, pending),
        Q_ARG(QString, QStringLiteral("token")),
        Q_ARG(QString, QStringLiteral("ready")),
        Q_ARG(QVariantList, QVariantList{})));
    pending.waitForFinished(3000);
    ASSERT_TRUE(pending.isFinished());
    EXPECT_EQ(pending.returnValue().toString(), QStringLiteral("ready"));

    server.stop();
}

bool waitFor(const std::function<bool()>& ready, int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeoutMs);
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

QString callWhoAmI(QRemoteObjectDynamicReplica* replica)
{
    QRemoteObjectPendingCall pending;
    if (!QMetaObject::invokeMethod(
            replica, "callRemoteMethod", Qt::DirectConnection,
            Q_RETURN_ARG(QRemoteObjectPendingCall, pending),
            Q_ARG(QString, QStringLiteral("token")),
            Q_ARG(QString, QStringLiteral("whoami")),
            Q_ARG(QVariantList, QVariantList{})))
        return QStringLiteral("<invoke failed>");
    pending.waitForFinished(3000);
    return pending.isFinished() ? pending.returnValue().toString()
                                : QStringLiteral("<timeout>");
}

Server::Object answeringFixture(std::string answer)
{
    return {"fixture", moduleProxyDefinition(),
        [answer = std::move(answer)](std::int32_t index, const std::vector<Variant>&) {
            return index == 0 ? Variant::fromRpc(RpcValue{answer}) : Variant{};
        }};
}

// Detector: a re-attaching Qt replica asks with AddObject(isDynamic=false) and
// stays Suspect until it gets Init. The server used to send nothing.
TEST(QtRemotePlainSocketCompatTest, QtReplicaRecoversAfterPlainServerRestarts)
{
    const std::string path = uniqueSocketPath("restart");
    std::string error;
    auto first = std::make_unique<Server>();
    ASSERT_TRUE(first->publish(answeringFixture("first"), &error)) << error;
    ASSERT_TRUE(first->start(path, &error)) << error;

    QRemoteObjectNode node;
    ASSERT_TRUE(node.connectToNode(
        QUrl(QStringLiteral("local:") + QString::fromStdString(path))));
    QSharedPointer<QRemoteObjectDynamicReplica> replica(
        node.acquireDynamic(QStringLiteral("fixture")));
    ASSERT_TRUE(replica->waitForSource(3000));
    EXPECT_EQ(callWhoAmI(replica.data()), QStringLiteral("first"));

    first->stop();
    first.reset();
    ASSERT_TRUE(waitFor([&] {
        return replica->state() == QRemoteObjectReplica::Suspect;
    }, 3000));

    Server second;
    ASSERT_TRUE(second.publish(answeringFixture("second"), &error)) << error;
    ASSERT_TRUE(second.start(path, &error)) << error;
    EXPECT_TRUE(waitFor([&] {
        return replica->state() == QRemoteObjectReplica::Valid;
    }, 5000)) << "replica state " << replica->state();
    EXPECT_EQ(callWhoAmI(replica.data()), QStringLiteral("second"));
    second.stop();
}

TEST(QtRemotePlainSocketCompatTest, QtReplicaRecoversAfterPlainServerRepublishes)
{
    const std::string path = uniqueSocketPath("republish");
    std::string error;
    Server server;
    ASSERT_TRUE(server.publish(answeringFixture("first"), &error)) << error;
    ASSERT_TRUE(server.start(path, &error)) << error;

    QRemoteObjectNode node;
    ASSERT_TRUE(node.connectToNode(
        QUrl(QStringLiteral("local:") + QString::fromStdString(path))));
    QSharedPointer<QRemoteObjectDynamicReplica> replica(
        node.acquireDynamic(QStringLiteral("fixture")));
    ASSERT_TRUE(replica->waitForSource(3000));
    EXPECT_EQ(callWhoAmI(replica.data()), QStringLiteral("first"));

    server.unpublish("fixture");
    ASSERT_TRUE(waitFor([&] {
        return replica->state() == QRemoteObjectReplica::Suspect;
    }, 3000));
    ASSERT_TRUE(server.publish(answeringFixture("second"), &error)) << error;
    EXPECT_TRUE(waitFor([&] {
        return replica->state() == QRemoteObjectReplica::Valid;
    }, 5000)) << "replica state " << replica->state();
    EXPECT_EQ(callWhoAmI(replica.data()), QStringLiteral("second"));
    server.stop();
}

// Detector: Qt 6.9.2 dereferences a null metaobject when an event reaches a
// replica before its definition (qremoteobjectnode.cpp:1700).
TEST(QtRemotePlainSocketCompatTest, DefinitionAlwaysPrecedesEvents)
{
    const std::string path = uniqueSocketPath("definition_first");
    std::string error;
    Server server;
    ASSERT_TRUE(server.publish(answeringFixture("x"), &error)) << error;
    ASSERT_TRUE(server.start(path, &error)) << error;

    std::atomic<bool> emitting{true};
    std::thread emitter([&] {
        RpcList data;
        data.items.emplace_back("payload");
        const std::vector<Variant> arguments{Variant::fromRpc(RpcValue{"tick"}),
                                             Variant::fromRpc(RpcValue{data})};
        while (emitting) (void)server.emitSignal("fixture", 0, arguments);
    });

    int eventFirst = 0;
    for (int round = 0; round < 300; ++round) {
        auto peer = connectUnix(path);
        ASSERT_EQ(decodeFrame(readFrame(peer.get())).type, PacketType::Handshake);
        ASSERT_EQ(decodeFrame(readFrame(peer.get())).type, PacketType::ObjectList);
        writeAll(peer.get(), addObjectPacket("fixture"));
        if (decodeFrame(readFrame(peer.get())).type != PacketType::InitDynamic)
            ++eventFirst;
    }
    emitting = false;
    emitter.join();
    server.stop();
    EXPECT_EQ(eventFirst, 0);
}

// Detector: Qt drops a connection whose first frame is not the handshake and
// never retries that endpoint.
TEST(QtRemotePlainSocketCompatTest, HandshakeIsAlwaysTheFirstFrame)
{
    const std::string path = uniqueSocketPath("handshake_first");
    std::string error;
    Server server;
    ASSERT_TRUE(server.start(path, &error)) << error;

    std::atomic<bool> churning{true};
    std::thread churn([&] {
        while (churning) {
            (void)server.publish(answeringFixture("x"));
            server.unpublish("fixture");
        }
    });

    int wrongFirst = 0;
    for (int round = 0; round < 1000; ++round) {
        auto peer = connectUnix(path);
        if (decodeFrame(readFrame(peer.get())).type != PacketType::Handshake)
            ++wrongFirst;
    }
    churning = false;
    churn.join();
    server.stop();
    EXPECT_EQ(wrongFirst, 0);
}

#else

TEST(QtRemotePlainSocketCompatTest, WindowsUsesTheNewPlainTransportOnly)
{
    GTEST_SKIP() << "Windows binaries are rebuilt together; Qt 6.9/6.11 local-socket compatibility is out of scope";
}

#endif

} // namespace

#include "test_qt_remote_plain_socket_compat.moc"
