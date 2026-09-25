#include "implementations/qt_remote_plain/qtro_wire.h"
#include "logos_types.h"
#include "module_proxy.h"

#include <gtest/gtest.h>

#include <QBuffer>
#include <QDataStream>
#include <QDate>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaMethod>
#include <QTime>
#include <QTimeZone>
#include <QUrl>
#include <QUuid>
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

TEST(QtRemotePlainCompatTest, EveryQJsonValueAlternativeMatchesQt692)
{
    RpcList array;
    array.items = {RpcValue{true}, RpcValue{42.5}, RpcValue{"x"}};
    RpcMap object;
    object.emplace("answer", RpcValue{std::int64_t{42}});

    const std::vector<std::pair<QJsonValue, Variant>> cases{
        {QJsonValue(QJsonValue::Undefined),
         Variant{MetaType::JsonValue, false, RpcValue{}, {}, true}},
        {QJsonValue(QJsonValue::Null),
         Variant{MetaType::JsonValue, false, RpcValue{}}},
        {QJsonValue(true),
         Variant{MetaType::JsonValue, false, RpcValue{true}}},
        {QJsonValue(42.5),
         Variant{MetaType::JsonValue, false, RpcValue{42.5}}},
        {QJsonValue(QStringLiteral("hello")),
         Variant{MetaType::JsonValue, false, RpcValue{"hello"}}},
        {QJsonValue(QJsonArray{true, 42.5, QStringLiteral("x")}),
         Variant{MetaType::JsonValue, false, RpcValue{std::move(array)}}},
        {QJsonValue(QJsonObject{{QStringLiteral("answer"), 42}}),
         Variant{MetaType::JsonValue, false, RpcValue{std::move(object)}}},
    };

    for (const auto& [qtValue, plainValue] : cases) {
        QByteArray qt;
        QDataStream stream(&qt, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_6_2);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream << QVariant::fromValue(qtValue);

        Writer writer;
        writer.variant(plainValue);
        EXPECT_EQ(writer.data(), stdBytes(qt));

        const auto bytes = stdBytes(qt);
        Reader reader(bytes);
        EXPECT_EQ(reader.variant(), plainValue);
        EXPECT_EQ(reader.remaining(), 0u);
    }
}

TEST(QtRemotePlainCompatTest, QJsonValuesConsumeCompletePacketsAndNestedContainers)
{
    RpcList array;
    array.items = {RpcValue{true}, RpcValue{42.5}, RpcValue{"x"}};
    RpcMap objectValue;
    objectValue.emplace("answer", RpcValue{std::int64_t{42}});
    const std::vector<std::pair<QJsonValue, Variant>> cases{
        {QJsonValue(QJsonValue::Undefined),
         Variant{MetaType::JsonValue, false, RpcValue{}, {}, true}},
        {QJsonValue(QJsonValue::Null),
         Variant{MetaType::JsonValue, false, RpcValue{}}},
        {QJsonValue(true), Variant{MetaType::JsonValue, false, RpcValue{true}}},
        {QJsonValue(42.5), Variant{MetaType::JsonValue, false, RpcValue{42.5}}},
        {QJsonValue(QStringLiteral("hello")),
         Variant{MetaType::JsonValue, false, RpcValue{"hello"}}},
        {QJsonValue(QJsonArray{true, 42.5, QStringLiteral("x")}),
         Variant{MetaType::JsonValue, false, RpcValue{array}}},
        {QJsonValue(QJsonObject{{QStringLiteral("answer"), 42}}),
         Variant{MetaType::JsonValue, false, RpcValue{objectValue}}},
    };
    const QString object = QStringLiteral("fixture");

    QVariantList qtList;
    QVariantMap qtMap;
    RpcList expectedList;
    RpcMap expectedMap;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& [qtValue, plainValue] = cases[i];
        const QVariant qtVariant = QVariant::fromValue(qtValue);

        const auto expectedReply = qtPacket(7, &object, [&](QDataStream& stream) {
            stream << qint32(17) << qtVariant;
        });
        EXPECT_EQ(invokeReplyPacket("fixture", 17, plainValue), stdBytes(expectedReply));

        const auto expectedEvent = qtPacket(6, &object, [&](QDataStream& stream) {
            stream << qint32(0) << qint32(0) << quint32(1) << qtVariant
                   << qint32(-1) << qint32(-1);
        });
        EXPECT_EQ(invokePacket("fixture", 0, 0, {plainValue}), stdBytes(expectedEvent));

        qtList.push_back(qtVariant);
        qtMap.insert(QString::number(i), qtVariant);
        expectedList.items.push_back(plainValue.value);
        expectedMap.emplace(std::to_string(i), plainValue.value);
    }

    QByteArray encodedList;
    QDataStream listStream(&encodedList, QIODevice::WriteOnly);
    listStream.setVersion(QDataStream::Qt_6_2);
    listStream.setByteOrder(QDataStream::LittleEndian);
    listStream << QVariant{qtList};
    const auto listBytes = stdBytes(encodedList);
    Reader listReader(listBytes);
    const Variant decodedList = listReader.variant();
    EXPECT_EQ(decodedList.type, MetaType::VariantList);
    EXPECT_EQ(decodedList.value, RpcValue{std::move(expectedList)});
    ASSERT_EQ(decodedList.nestedValues.size(), cases.size());
    for (std::size_t i = 0; i < cases.size(); ++i)
        EXPECT_EQ(decodedList.nestedValues[i], cases[i].second);
    EXPECT_EQ(listReader.remaining(), 0u);

    QByteArray encodedMap;
    QDataStream mapStream(&encodedMap, QIODevice::WriteOnly);
    mapStream.setVersion(QDataStream::Qt_6_2);
    mapStream.setByteOrder(QDataStream::LittleEndian);
    mapStream << QVariant{qtMap};
    const auto mapBytes = stdBytes(encodedMap);
    Reader mapReader(mapBytes);
    const Variant decodedMap = mapReader.variant();
    EXPECT_EQ(decodedMap.type, MetaType::VariantMap);
    EXPECT_EQ(decodedMap.value, RpcValue{std::move(expectedMap)});
    ASSERT_EQ(decodedMap.nestedValues.size(), cases.size());
    EXPECT_EQ(decodedMap.nestedKeys.size(), decodedMap.nestedValues.size());
    EXPECT_EQ(mapReader.remaining(), 0u);

    QByteArray encodedResult;
    QDataStream resultStream(&encodedResult, QIODevice::WriteOnly);
    resultStream.setVersion(QDataStream::Qt_6_2);
    resultStream.setByteOrder(QDataStream::LittleEndian);
    resultStream << quint32(static_cast<std::uint32_t>(MetaType::User)) << quint8(0)
                 << QByteArray("LogosResult\0", 12) << true
                 << QVariant::fromValue(QJsonValue(QStringLiteral("nested")))
                 << QVariant::fromValue(QJsonValue(QJsonValue::Null));
    const auto resultBytes = stdBytes(encodedResult);
    Reader resultReader(resultBytes);
    const Variant decodedResult = resultReader.variant();
    EXPECT_EQ(decodedResult.type, MetaType::User);
    EXPECT_EQ(decodedResult.customType, "LogosResult");
    EXPECT_EQ(decodedResult.value.asMap().find("value")->asString(), "nested");
    EXPECT_TRUE(decodedResult.value.asMap().find("error")->isNull());
    EXPECT_EQ(resultReader.remaining(), 0u);
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
    stream << quint32(2);
    stream << QByteArray("informModuleToken(QString,QString,QString)")
           << QByteArray("bool")
           << names({"authToken", "moduleName", "token"});
    stream << QByteArray("revokeModuleToken(QString,QString,QString)")
           << QByteArray("bool")
           << names({"authToken", "moduleName", "tokenDigest"});
    stream << quint32(0);

    EXPECT_EQ(plain.data(), stdBytes(qt));
}

std::vector<std::uint8_t> qtVariantBytes(const QVariant& value)
{
    QByteArray result;
    QDataStream stream(&result, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_2);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << value;
    return stdBytes(result);
}

QVariant qtVariantFrom(const std::vector<std::uint8_t>& bytes)
{
    QByteArray data(reinterpret_cast<const char*>(bytes.data()), qsizetype(bytes.size()));
    QDataStream stream(data);
    stream.setVersion(QDataStream::Qt_6_2);
    stream.setByteOrder(QDataStream::LittleEndian);
    QVariant value;
    stream >> value;
    return stream.status() == QDataStream::Ok ? value : QVariant();
}

RpcList byteItems(std::initializer_list<std::vector<std::uint8_t>> items)
{
    RpcList list;
    for (const auto& item : items) list.items.emplace_back(RpcBytes{item});
    return list;
}

// Detector: each of these types dropped the whole connection on the previous
// head ("unsupported QVariant metatype id").
TEST(QtRemotePlainCompatTest, QtBuiltinValuesDecodeAndReencodeByteForByte)
{
    const QDate date(2026, 9, 23);
    const QTime time(10, 37, 50, 123);
    const QTimeZone berlin("Europe/Berlin");
    struct Case {
        QVariant qt;
        RpcValue expected;
    };
    std::vector<Case> cases{
        {QVariant::fromValue<float>(1.5f), RpcValue{1.5}},
        {QVariant::fromValue<short>(-3), RpcValue{std::int64_t{-3}}},
        {QVariant::fromValue<ushort>(65535), RpcValue{std::int64_t{65535}}},
        {QVariant::fromValue<char>('A'), RpcValue{std::int64_t{65}}},
        {QVariant::fromValue<signed char>(-5), RpcValue{std::int64_t{-5}}},
        {QVariant::fromValue<uchar>(200), RpcValue{std::int64_t{200}}},
        {QVariant::fromValue<long>(-7), RpcValue{std::int64_t{-7}}},
        {QVariant::fromValue<ulong>(7), RpcValue{std::int64_t{7}}},
        {QVariant(QChar(0x00e9)), RpcValue{"\xc3\xa9"}},
        {QVariant(QUrl(QStringLiteral("https://example.org/a b?q=1"))),
         RpcValue{"https://example.org/a%20b?q=1"}},
        {QVariant(QUuid(QStringLiteral("{12345678-9abc-def0-1234-56789abcdef0}"))),
         RpcValue{"{12345678-9abc-def0-1234-56789abcdef0}"}},
        {QVariant(date), RpcValue{"2026-09-23"}},
        {QVariant(time), RpcValue{"10:37:50.123"}},
        {QVariant(QDateTime(date, time, QTimeZone::UTC)),
         RpcValue{"2026-09-23T10:37:50.123Z"}},
        {QVariant(QDateTime(date, time, QTimeZone::fromSecondsAheadOfUtc(-3 * 3600))),
         RpcValue{"2026-09-23T10:37:50.123-03:00"}},
        {QVariant::fromValue(QByteArrayList{QByteArray("a"), QByteArray()}),
         RpcValue{byteItems({{'a'}, {}})}},
        {QVariant(QString()), RpcValue{""}},
        {QVariant(QByteArray()), RpcValue{RpcBytes{}}},
        {QVariant(QStringList{QStringLiteral("a"), QString()}),
         RpcValue{RpcList{{RpcValue{"a"}, RpcValue{""}}}}},
        {QVariant(QString(QChar(0xd83d))), RpcValue{"\xef\xbf\xbd"}},
        {QVariant::fromValue(QJsonDocument()), RpcValue{}},
    };
    if (berlin.isValid())
        cases.push_back({QVariant(QDateTime(date, time, berlin)),
                         RpcValue{"2026-09-23T10:37:50.123[Europe/Berlin]"}});

    for (const auto& value : cases) {
        SCOPED_TRACE(value.qt.typeName());
        const auto bytes = qtVariantBytes(value.qt);
        Reader reader(bytes);
        Variant decoded;
        ASSERT_NO_THROW(decoded = reader.variant());
        EXPECT_EQ(reader.remaining(), 0u);
        EXPECT_EQ(decoded.value, value.expected);
        Writer writer;
        ASSERT_NO_THROW(writer.variant(decoded));
        EXPECT_EQ(writer.data(), bytes);
    }
}

TEST(QtRemotePlainCompatTest, QVariantHashDecodesAndQtReadsItBack)
{
    QVariantHash hash;
    hash.insert(QStringLiteral("n"), 7);
    hash.insert(QStringLiteral("s"), QStringLiteral("x"));
    const auto bytes = qtVariantBytes(hash);
    Reader reader(bytes);
    const Variant decoded = reader.variant();
    ASSERT_TRUE(decoded.value.isMap());
    EXPECT_EQ(*decoded.value.asMap().find("n"), RpcValue{std::int64_t{7}});
    EXPECT_EQ(*decoded.value.asMap().find("s"), RpcValue{"x"});

    Writer writer;
    writer.variant(decoded);
    EXPECT_EQ(qtVariantFrom(writer.data()), QVariant(hash));
}

TEST(QtRemotePlainCompatTest, AQtCustomTypeCrossesOpaqueAndQtReadsItBack)
{
    const QVariant ints = QVariant::fromValue(QList<int>{1, 2, 3});
    const auto bytes = qtVariantBytes(ints);
    Reader strict(bytes);
    EXPECT_THROW((void)strict.variant(), CodecError);

    Reader reader(bytes);
    const Variant opaque = reader.variantUntil(bytes.size());
    EXPECT_TRUE(opaque.opaque);
    EXPECT_EQ(opaque.customType, "QList<int>");
    Writer writer;
    writer.variant(opaque);
    EXPECT_EQ(writer.data(), bytes);
    EXPECT_EQ(qtVariantFrom(writer.data()), ints);
}

} // namespace
