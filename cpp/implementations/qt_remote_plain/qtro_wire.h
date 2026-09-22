#ifndef LOGOS_QT_REMOTE_PLAIN_QTRO_WIRE_H
#define LOGOS_QT_REMOTE_PLAIN_QTRO_WIRE_H

#include "../plain/rpc_value.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace logos::qt_remote_plain {

// Qt Remote Objects 6.9.2 uses QDataStream::Qt_6_2 in little-endian mode.
// These are the stable QMetaType ids which can cross the Logos module surface.
enum class MetaType : std::uint32_t {
    Invalid = 0,
    Bool = 1,
    Int = 2,
    UInt = 3,
    LongLong = 4,
    ULongLong = 5,
    Double = 6,
    VariantMap = 8,
    VariantList = 9,
    String = 10,
    StringList = 11,
    ByteArray = 12,
    JsonValue = 45,
    JsonObject = 46,
    JsonArray = 47,
    JsonDocument = 48,
    User = 65536,
};

enum class PacketType : std::uint16_t {
    Invalid = 0,
    Handshake = 1,
    Init = 2,
    InitDynamic = 3,
    AddObject = 4,
    RemoveObject = 5,
    Invoke = 6,
    InvokeReply = 7,
    PropertyChange = 8,
    ObjectList = 9,
    Ping = 10,
    Pong = 11,
};

class CodecError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A QVariant on the QtRO wire. `value` is the language-neutral payload and the
// explicit type is retained so a decoded value can be reproduced byte-for-byte.
// Newly-created semantic values use Variant::fromRpc()'s canonical mapping.
struct Variant {
    MetaType type = MetaType::Invalid;
    bool isNull = true;
    plain::RpcValue value;
    std::string customType;
    // QJsonValue::Undefined and QJsonValue::Null both have no RpcValue
    // payload, but Qt gives them distinct wire tags (0x80 and 0). Preserve the
    // distinction so a decoded value can be emitted byte-for-byte.
    bool jsonUndefined = false;
    // QVariant containers carry a complete QVariant for every child. RpcValue
    // deliberately has no Qt metatype information, so keep the recursive wire
    // variants alongside the language-neutral value. Maps use matching entries
    // in nestedKeys/nestedValues; lists use nestedValues only. LogosResult keeps
    // its value and error children in nestedValues.
    std::vector<std::string> nestedKeys;
    std::vector<Variant> nestedValues;

    static Variant fromRpc(plain::RpcValue value);
    static Variant logosResult(bool success, Variant value, Variant error);

    bool operator==(const Variant& other) const;
};

struct ObjectInfo {
    std::string name;
    std::optional<std::string> typeName;
    std::vector<std::uint8_t> signature;

    bool operator==(const ObjectInfo& other) const
    {
        return name == other.name && typeName == other.typeName
            && signature == other.signature;
    }
};

struct SignalDefinition {
    std::string signature;
    std::vector<std::string> parameterNames;

    bool operator==(const SignalDefinition& other) const
    {
        return signature == other.signature && parameterNames == other.parameterNames;
    }
};

struct MethodDefinition {
    std::string signature;
    std::string returnType;
    std::vector<std::string> parameterNames;

    bool operator==(const MethodDefinition& other) const
    {
        return signature == other.signature && returnType == other.returnType
            && parameterNames == other.parameterNames;
    }
};

struct PropertyDefinition {
    std::string name;
    std::string typeName;
    std::string notifySignal;

    bool operator==(const PropertyDefinition& other) const
    {
        return name == other.name && typeName == other.typeName
            && notifySignal == other.notifySignal;
    }
};

// The Logos QtRO objects use no enums, gadgets or properties. The wire still
// carries the corresponding zero counts; keeping them implicit here prevents a
// general Qt meta-object clone from becoming part of the plain transport.
struct ClassDefinition {
    std::string typeName;
    std::vector<SignalDefinition> signalDefinitions;
    std::vector<MethodDefinition> methodDefinitions;
    std::vector<PropertyDefinition> propertyDefinitions;

    bool operator==(const ClassDefinition& other) const
    {
        return typeName == other.typeName
            && signalDefinitions == other.signalDefinitions
            && methodDefinitions == other.methodDefinitions
            && propertyDefinitions == other.propertyDefinitions;
    }
};

struct Frame {
    PacketType type = PacketType::Invalid;
    std::string name;
    std::vector<std::uint8_t> payload;
};

class Writer {
public:
    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void i32(std::int32_t value);
    void i64(std::int64_t value);
    void boolean(bool value);
    void real(double value);
    void bytes(const std::vector<std::uint8_t>& value, bool isNull = false);
    void byteString(std::string_view value, bool includeTerminator = false);
    void string(std::string_view utf8, bool isNull = false);
    void stringList(const std::vector<std::string>& value);
    void byteStringList(const std::vector<std::string>& value);
    void variant(const Variant& value);
    void classDefinition(const ClassDefinition& value);
    void raw(const std::vector<std::uint8_t>& value);
    void patchU32(std::size_t offset, std::uint32_t value);

    const std::vector<std::uint8_t>& data() const { return m_data; }
    std::vector<std::uint8_t> take() { return std::move(m_data); }

private:
    std::vector<std::uint8_t> m_data;
};

class Reader {
public:
    explicit Reader(const std::vector<std::uint8_t>& data);
    Reader(const std::uint8_t* data, std::size_t size);

    std::uint8_t u8();
    std::uint16_t u16();
    std::uint32_t u32();
    std::uint64_t u64();
    std::int32_t i32();
    std::int64_t i64();
    bool boolean();
    double real();
    std::optional<std::vector<std::uint8_t>> bytes();
    std::optional<std::string> string();
    std::vector<std::string> stringList();
    std::vector<std::string> byteStringList();
    Variant variant();
    ClassDefinition classDefinition();
    std::vector<std::uint8_t> remainingBytes();

    std::size_t remaining() const { return m_size - m_pos; }
    std::size_t position() const { return m_pos; }

private:
    void require(std::size_t count) const;

    const std::uint8_t* m_data;
    std::size_t m_size;
    std::size_t m_pos = 0;
};

std::vector<std::uint8_t> handshakePacket();
std::vector<std::uint8_t> objectListPacket(const std::vector<ObjectInfo>& objects);
std::vector<std::uint8_t> addObjectPacket(std::string_view name, bool dynamic = true);
std::vector<std::uint8_t> removeObjectPacket(std::string_view name);
std::vector<std::uint8_t> initDynamicPacket(
    std::string_view name,
    const ClassDefinition& definition,
    const std::vector<Variant>& properties = {});
std::vector<std::uint8_t> invokePacket(
    std::string_view name,
    std::int32_t call,
    std::int32_t index,
    const std::vector<Variant>& args,
    std::int32_t serialId = -1,
    std::int32_t propertyIndex = -1);
std::vector<std::uint8_t> invokeReplyPacket(
    std::string_view name,
    std::int32_t serialId,
    const Variant& value);
std::vector<std::uint8_t> pingPacket(PacketType type, std::string_view name);

Frame decodeFrame(const std::vector<std::uint8_t>& bytes);
std::optional<std::size_t> completeFrameSize(const std::vector<std::uint8_t>& buffered);

// Fixed dynamic definitions emitted by QRemoteObjectHost::enableRemoting for
// the two QObject surfaces used by current Logos modules.
ClassDefinition moduleProxyDefinition();
ClassDefinition moduleHandshakeProxyDefinition();

} // namespace logos::qt_remote_plain

#endif
