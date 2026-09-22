#include "implementations/qt_remote_plain/qtro_wire.h"
#include "module_proxy.h"

#include <gtest/gtest.h>

#include <QBuffer>
#include <QDataStream>
#include <QMetaMethod>
#include <QVariant>

#include <functional>

using namespace logos::plain;
using namespace logos::qt_remote_plain;

namespace {

QByteArray qtPacket(quint16 id, const QString* name,
                    const std::function<void(QDataStream&)>& body)
{
    QByteArray result;
    QDataStream stream(&result, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_2);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << quint32(0) << id;
    if (name) stream << *name;
    body(stream);
    stream.device()->seek(0);
    stream << quint32(result.size() - int(sizeof(quint32)));
    return result;
}

std::vector<std::uint8_t> stdBytes(const QByteArray& value)
{
    const auto* begin = reinterpret_cast<const std::uint8_t*>(value.constData());
    return {begin, begin + value.size()};
}

QByteArrayList names(std::initializer_list<const char*> values)
{
    QByteArrayList result;
    for (const auto* value : values) result.append(QByteArray(value));
    return result;
}

TEST(QtRemotePlainCompatTest, ControlPacketsMatchQt692QDataStream)
{
    const QString protocol = QStringLiteral("QtRO 2.0");
    EXPECT_EQ(handshakePacket(),
              stdBytes(qtPacket(1, &protocol, [](QDataStream&) {})));

    const QString object = QStringLiteral("chat_module");
    EXPECT_EQ(addObjectPacket("chat_module", true),
              stdBytes(qtPacket(4, &object, [](QDataStream& stream) { stream << true; })));
    EXPECT_EQ(removeObjectPacket("chat_module"),
              stdBytes(qtPacket(5, &object, [](QDataStream&) {})));
}

TEST(QtRemotePlainCompatTest, InvokePacketMatchesQt692ForCurrentValueProfile)
{
    const QString object = QStringLiteral("chat_module");
    QVariantMap nested;
    nested.insert(QStringLiteral("answer"), QVariant::fromValue<qlonglong>(42));
    nested.insert(QStringLiteral("bytes"), QByteArray::fromHex("007fff"));
    const QVariantList qtArgs{
        QString::fromUtf8("Grüße 🌍"),
        QVariant::fromValue<qlonglong>(-9),
        QVariant::fromValue<qulonglong>(std::numeric_limits<quint64>::max()),
        true,
        nested,
    };

    RpcMap map;
    map.emplace("answer", RpcValue{std::int64_t{42}});
    map.emplace("bytes", RpcValue{RpcBytes{{0x00, 0x7f, 0xff}}});
    std::vector<Variant> args{
        Variant::fromRpc(RpcValue{"Grüße 🌍"}),
        Variant::fromRpc(RpcValue{std::int64_t{-9}}),
        Variant::fromRpc(RpcValue{std::numeric_limits<std::uint64_t>::max()}),
        Variant::fromRpc(RpcValue{true}),
        Variant::fromRpc(RpcValue{std::move(map)}),
    };

    const auto expected = qtPacket(6, &object, [&](QDataStream& stream) {
        stream << qint32(0) << qint32(3) << quint32(qtArgs.size());
        for (const auto& arg : qtArgs) stream << arg;
        stream << qint32(17) << qint32(-1);
    });
    EXPECT_EQ(invokePacket("chat_module", 0, 3, args, 17, -1), stdBytes(expected));
}

TEST(QtRemotePlainCompatTest, FixedModuleProxyDefinitionMatchesItsMetaObject)
{
    const auto fixed = moduleProxyDefinition();
    const QMetaObject& meta = ModuleProxy::staticMetaObject;

    std::vector<SignalDefinition> actualSignals;
    std::vector<MethodDefinition> actualMethods;
    for (int i = meta.methodOffset(); i < meta.methodCount(); ++i) {
        const QMetaMethod method = meta.method(i);
        std::vector<std::string> parameterNames;
        for (const auto& name : method.parameterNames())
            parameterNames.push_back(name.toStdString());
        if (method.methodType() == QMetaMethod::Signal) {
            actualSignals.push_back({method.methodSignature().toStdString(), std::move(parameterNames)});
        } else if (method.methodType() == QMetaMethod::Method
                   || method.methodType() == QMetaMethod::Slot) {
            actualMethods.push_back({method.methodSignature().toStdString(),
                                     method.typeName() ? method.typeName() : "",
                                     std::move(parameterNames)});
        }
    }

    EXPECT_EQ(fixed.signalDefinitions, actualSignals);
    EXPECT_EQ(fixed.methodDefinitions, actualMethods);
}

TEST(QtRemotePlainCompatTest, DynamicDefinitionBytesMatchQt692QDataStream)
{
    const auto definition = moduleHandshakeProxyDefinition();
    Writer plain;
    plain.classDefinition(definition);

    QByteArray qt;
    QDataStream stream(&qt, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_2);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << QString::fromStdString(definition.typeName);
    stream << quint32(0) << quint32(0) << quint32(0);
    stream << quint32(0);
    stream << quint32(1);
    stream << QByteArray("informModuleToken(QString,QString,QString)")
           << QByteArray("bool")
           << names({"authToken", "moduleName", "token"});
    stream << quint32(0);

    EXPECT_EQ(plain.data(), stdBytes(qt));
}

} // namespace
