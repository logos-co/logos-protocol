#include "qtro_wire.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <nlohmann/json.hpp>
#include <type_traits>
#include <unordered_map>

namespace logos::qt_remote_plain {
namespace {

constexpr std::uint32_t kNullSize = 0xffffffffu;
constexpr std::size_t kMaxContainerEntries = 1u << 20;
constexpr std::size_t kMaxByteArray = 64u << 20;
constexpr std::string_view kProtocolVersion = "QtRO 2.0";

template <typename UInt>
void appendLittle(std::vector<std::uint8_t>& out, UInt value)
{
    static_assert(std::is_unsigned_v<UInt>);
    for (std::size_t i = 0; i < sizeof(UInt); ++i)
        out.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}

constexpr std::uint32_t kReplacement = 0xfffd;

// Invalid input becomes U+FFFD, as QString::fromUtf8 and toUtf8 do; Qt peers
// legitimately send strings with lone surrogates.
std::vector<std::uint16_t> utf8ToUtf16(std::string_view input)
{
    std::vector<std::uint16_t> out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size();) {
        const auto first = static_cast<std::uint8_t>(input[i]);
        std::uint32_t cp = 0;
        std::size_t count = 0;
        if (first < 0x80) {
            cp = first;
            count = 1;
        } else if ((first & 0xe0) == 0xc0) {
            cp = first & 0x1f;
            count = 2;
        } else if ((first & 0xf0) == 0xe0) {
            cp = first & 0x0f;
            count = 3;
        } else if ((first & 0xf8) == 0xf0) {
            cp = first & 0x07;
            count = 4;
        }
        bool valid = count != 0 && i + count <= input.size();
        for (std::size_t j = 1; valid && j < count; ++j) {
            const auto next = static_cast<std::uint8_t>(input[i + j]);
            valid = (next & 0xc0) == 0x80;
            cp = (cp << 6) | (next & 0x3f);
        }
        const std::uint32_t minimum[] = {0, 0, 0x80, 0x800, 0x10000};
        if (valid && (cp < minimum[count] || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)))
            valid = false;
        if (!valid) {
            out.push_back(static_cast<std::uint16_t>(kReplacement));
            ++i;
            continue;
        }
        if (cp <= 0xffff) {
            out.push_back(static_cast<std::uint16_t>(cp));
        } else {
            cp -= 0x10000;
            out.push_back(static_cast<std::uint16_t>(0xd800 + (cp >> 10)));
            out.push_back(static_cast<std::uint16_t>(0xdc00 + (cp & 0x3ff)));
        }
        i += count;
    }
    return out;
}

std::string utf16ToUtf8(const std::vector<std::uint16_t>& input, bool* lossy = nullptr)
{
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        std::uint32_t cp = input[i];
        if (cp >= 0xd800 && cp <= 0xdbff) {
            if (i + 1 < input.size() && input[i + 1] >= 0xdc00 && input[i + 1] <= 0xdfff)
                cp = 0x10000 + ((cp - 0xd800) << 10) + (input[++i] - 0xdc00);
            else
                cp = kReplacement;
        } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            cp = kReplacement;
        }
        if (cp == kReplacement && input[i] != kReplacement && lossy) *lossy = true;
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }
    return out;
}

nlohmann::json rpcToJson(const plain::RpcValue& value)
{
    if (value.isNull()) return nullptr;
    if (value.isBool()) return value.asBool();
    if (value.isInt()) return value.asInt();
    if (value.isUInt()) return value.asUInt();
    if (value.isDouble()) return value.asDouble();
    if (value.isString()) return value.asString();
    if (value.isBytes()) {
        throw CodecError("QJson values cannot contain raw bytes");
    }
    if (value.isList()) {
        auto out = nlohmann::json::array();
        for (const auto& child : value.asList().items)
            out.push_back(rpcToJson(child));
        return out;
    }
    auto out = nlohmann::json::object();
    for (const auto& [key, child] : value.asMap().entries)
        out[key] = rpcToJson(child);
    return out;
}

plain::RpcValue jsonToRpc(const nlohmann::json& value)
{
    if (value.is_null()) return {};
    if (value.is_boolean()) return plain::RpcValue{value.get<bool>()};
    if (value.is_number_unsigned()) return plain::RpcValue::makeInteger(value.get<std::uint64_t>());
    if (value.is_number_integer()) return plain::RpcValue{value.get<std::int64_t>()};
    if (value.is_number_float()) return plain::RpcValue{value.get<double>()};
    if (value.is_string()) return plain::RpcValue{value.get<std::string>()};
    if (value.is_array()) {
        plain::RpcList list;
        list.items.reserve(value.size());
        for (const auto& child : value)
            list.items.push_back(jsonToRpc(child));
        return plain::RpcValue{std::move(list)};
    }
    plain::RpcMap map;
    for (auto it = value.begin(); it != value.end(); ++it)
        map.emplace(it.key(), jsonToRpc(it.value()));
    return plain::RpcValue{std::move(map)};
}

std::vector<std::uint8_t> jsonBytes(const plain::RpcValue& value)
{
    const std::string text = rpcToJson(value).dump(
        -1, ' ', false, nlohmann::json::error_handler_t::replace);
    return {text.begin(), text.end()};
}

// Qt writes a null QJsonDocument as a null QByteArray and reads an empty
// buffer as a null document; `empty` is what that decodes to for the type.
plain::RpcValue parseJsonBytes(const std::optional<std::vector<std::uint8_t>>& bytes,
                               plain::RpcValue empty = {})
{
    if (!bytes || bytes->empty())
        return empty;
    const std::string text(bytes->begin(), bytes->end());
    const auto parsed = nlohmann::json::parse(text, nullptr, false);
    if (parsed.is_discarded())
        throw CodecError("invalid QJson payload");
    return jsonToRpc(parsed);
}

std::string utf8Of(std::uint32_t cp)
{
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = kReplacement;
    std::string out;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
    return out;
}

std::int64_t floorDiv(std::int64_t a, std::int64_t b)
{
    return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0);
}

// QDate::toString(Qt::ISODate): proleptic Gregorian, years 1..9999 only.
std::string isoDate(std::int64_t jd)
{
    if (jd < 1721426 || jd > 5373484) return {};
    const std::int64_t a = jd + 32044;
    const std::int64_t b = floorDiv(4 * a + 3, 146097);
    const std::int64_t c = a - floorDiv(146097 * b, 4);
    const std::int64_t d = floorDiv(4 * c + 3, 1461);
    const std::int64_t e = c - floorDiv(1461 * d, 4);
    const std::int64_t m = floorDiv(5 * e + 2, 153);
    const std::int64_t day = e - floorDiv(153 * m + 2, 5) + 1;
    const std::int64_t month = m + 3 - 12 * floorDiv(m, 10);
    const std::int64_t year = 100 * b + d - 4800 + floorDiv(m, 10);
    char text[16];
    std::snprintf(text, sizeof(text), "%04d-%02d-%02d", static_cast<int>(year),
                  static_cast<int>(month), static_cast<int>(day));
    return text;
}

// QTime::toString(Qt::ISODateWithMs); a null time is -1 milliseconds.
std::string isoTime(std::int32_t msecs)
{
    if (msecs < 0 || msecs >= 86400000) return {};
    char text[16];
    std::snprintf(text, sizeof(text), "%02d:%02d:%02d.%03d", msecs / 3600000,
                  msecs / 60000 % 60, msecs / 1000 % 60, msecs % 1000);
    return text;
}

std::string isoOffset(std::int32_t seconds)
{
    const char sign = seconds < 0 ? '-' : '+';
    const std::int32_t magnitude = seconds < 0 ? -seconds : seconds;
    char text[16];
    std::snprintf(text, sizeof(text), "%c%02d:%02d", sign, magnitude / 3600, magnitude / 60 % 60);
    return text;
}

std::vector<std::pair<std::vector<std::uint16_t>, const std::pair<std::string, plain::RpcValue>*>>
sortedMapEntries(const plain::RpcMap& map)
{
    std::vector<std::pair<std::vector<std::uint16_t>, const std::pair<std::string, plain::RpcValue>*>> entries;
    entries.reserve(map.entries.size());
    for (const auto& entry : map.entries)
        entries.emplace_back(utf8ToUtf16(entry.first), &entry);
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    return entries;
}

const plain::RpcValue& requiredMapValue(const plain::RpcMap& map, const char* key)
{
    const auto* value = map.find(key);
    if (!value)
        throw CodecError(std::string("LogosResult is missing '") + key + "'");
    return *value;
}

// Checked accessors: a Variant whose value does not match its type must fail
// the one frame being encoded, never escape as std::bad_variant_access.
[[noreturn]] void mismatch(const char* expected)
{
    throw CodecError(std::string("QVariant value is not a ") + expected);
}

bool boolOf(const plain::RpcValue& value)
{
    if (!value.isBool()) mismatch("bool");
    return value.asBool();
}

std::int64_t integerOf(const plain::RpcValue& value)
{
    if (value.isInt()) return value.asInt();
    if (value.isUInt()) return static_cast<std::int64_t>(value.asUInt());
    if (value.isBool()) return value.asBool() ? 1 : 0;
    mismatch("integer");
}

std::uint64_t unsignedOf(const plain::RpcValue& value)
{
    if (value.isUInt()) return value.asUInt();
    if (value.isInt()) return static_cast<std::uint64_t>(value.asInt());
    if (value.isBool()) return value.asBool() ? 1 : 0;
    mismatch("unsigned integer");
}

double realOf(const plain::RpcValue& value)
{
    if (value.isDouble()) return value.asDouble();
    if (value.isInt()) return static_cast<double>(value.asInt());
    if (value.isUInt()) return static_cast<double>(value.asUInt());
    mismatch("number");
}

const std::string& stringOf(const plain::RpcValue& value)
{
    if (!value.isString()) mismatch("string");
    return value.asString();
}

const plain::RpcList& listOf(const plain::RpcValue& value)
{
    if (!value.isList()) mismatch("list");
    return value.asList();
}

const plain::RpcMap& mapOf(const plain::RpcValue& value)
{
    if (!value.isMap()) mismatch("map");
    return value.asMap();
}

const std::vector<std::uint8_t>& bytesOf(const plain::RpcValue& value)
{
    if (!value.isBytes()) mismatch("byte array");
    return value.asBytes().data;
}

constexpr std::size_t kMaxNesting = 64;

Writer packetPrefix(PacketType type, std::string_view name)
{
    Writer writer;
    writer.u32(0);
    writer.u16(static_cast<std::uint16_t>(type));
    if (type != PacketType::ObjectList)
        writer.string(name);
    return writer;
}

std::vector<std::uint8_t> finishPacket(Writer writer)
{
    if (writer.data().size() < 4 || writer.data().size() - 4 > std::numeric_limits<std::uint32_t>::max())
        throw CodecError("QtRO frame is too large");
    writer.patchU32(0, static_cast<std::uint32_t>(writer.data().size() - 4));
    return writer.take();
}

} // namespace

Variant Variant::fromRpc(plain::RpcValue input)
{
    Variant out;
    out.isNull = input.isNull();
    if (input.isNull()) out.type = MetaType::Invalid;
    else if (input.isBool()) out.type = MetaType::Bool;
    else if (input.isInt()) out.type = MetaType::LongLong;
    else if (input.isUInt()) out.type = MetaType::ULongLong;
    else if (input.isDouble()) out.type = MetaType::Double;
    else if (input.isString()) out.type = MetaType::String;
    else if (input.isBytes()) out.type = MetaType::ByteArray;
    else if (input.isList()) {
        out.type = MetaType::VariantList;
        out.nestedValues.reserve(input.asList().items.size());
        for (const auto& child : input.asList().items)
            out.nestedValues.push_back(fromRpc(child));
    }
    else if (input.isMap()) {
        out.type = MetaType::VariantMap;
        out.nestedKeys.reserve(input.asMap().entries.size());
        out.nestedValues.reserve(input.asMap().entries.size());
        for (const auto& entry : input.asMap().entries) {
            out.nestedKeys.push_back(entry.first);
            out.nestedValues.push_back(fromRpc(entry.second));
        }
    }
    out.value = std::move(input);
    return out;
}

Variant Variant::logosResult(bool success, Variant resultValue, Variant resultError)
{
    plain::RpcMap map;
    map.emplace("success", plain::RpcValue{success});
    map.emplace("value", resultValue.value);
    map.emplace("error", resultError.value);
    Variant out{MetaType::User, false, plain::RpcValue{std::move(map)}, "LogosResult"};
    out.nestedValues.push_back(std::move(resultValue));
    out.nestedValues.push_back(std::move(resultError));
    return out;
}

bool Variant::operator==(const Variant& other) const
{
    return type == other.type && isNull == other.isNull
        && value == other.value && customType == other.customType
        && jsonUndefined == other.jsonUndefined
        && nestedKeys == other.nestedKeys
        && nestedValues == other.nestedValues
        && raw == other.raw && opaque == other.opaque;
}

void Writer::u8(std::uint8_t value) { m_data.push_back(value); }
void Writer::u16(std::uint16_t value) { appendLittle(m_data, value); }
void Writer::u32(std::uint32_t value) { appendLittle(m_data, value); }
void Writer::u64(std::uint64_t value) { appendLittle(m_data, value); }
void Writer::i32(std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }
void Writer::i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }
void Writer::boolean(bool value) { u8(value ? 1 : 0); }

void Writer::real(double value)
{
    static_assert(sizeof(double) == sizeof(std::uint64_t));
    std::uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    u64(bits);
}

void Writer::bytes(const std::vector<std::uint8_t>& value, bool isNull)
{
    if (isNull) {
        u32(kNullSize);
        return;
    }
    if (value.size() > std::numeric_limits<std::uint32_t>::max())
        throw CodecError("QByteArray is too large");
    u32(static_cast<std::uint32_t>(value.size()));
    raw(value);
}

void Writer::byteString(std::string_view value, bool includeTerminator)
{
    std::vector<std::uint8_t> data(value.begin(), value.end());
    if (includeTerminator)
        data.push_back(0);
    bytes(data);
}

void Writer::string(std::string_view utf8, bool isNull)
{
    if (isNull) {
        u32(kNullSize);
        return;
    }
    const auto units = utf8ToUtf16(utf8);
    if (units.size() > std::numeric_limits<std::uint32_t>::max() / 2)
        throw CodecError("QString is too large");
    u32(static_cast<std::uint32_t>(units.size() * 2));
    for (const auto unit : units)
        u16(unit);
}

void Writer::stringList(const std::vector<std::string>& value)
{
    if (value.size() > std::numeric_limits<std::uint32_t>::max())
        throw CodecError("QStringList is too large");
    u32(static_cast<std::uint32_t>(value.size()));
    for (const auto& item : value)
        string(item);
}

void Writer::byteStringList(const std::vector<std::string>& value)
{
    if (value.size() > std::numeric_limits<std::uint32_t>::max())
        throw CodecError("QByteArrayList is too large");
    u32(static_cast<std::uint32_t>(value.size()));
    for (const auto& item : value)
        byteString(item);
}

void Writer::variant(const Variant& input)
{
    u32(static_cast<std::uint32_t>(input.type));
    u8(input.isNull ? 1 : 0);
    if (input.type == MetaType::User)
        byteString(input.customType, true);
    if (input.opaque || !input.raw.empty()) {
        raw(input.raw);
        return;
    }

    switch (input.type) {
    case MetaType::Invalid:
        return;
    case MetaType::Bool:
        boolean(boolOf(input.value));
        return;
    case MetaType::Int:
        i32(static_cast<std::int32_t>(integerOf(input.value)));
        return;
    case MetaType::UInt:
        u32(static_cast<std::uint32_t>(unsignedOf(input.value)));
        return;
    case MetaType::LongLong:
    case MetaType::Long:
        i64(integerOf(input.value));
        return;
    case MetaType::ULongLong:
    case MetaType::ULong:
        u64(unsignedOf(input.value));
        return;
    case MetaType::Double:
    case MetaType::Float: // DoublePrecision streams write floats as doubles
        real(realOf(input.value));
        return;
    case MetaType::Short:
        u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(integerOf(input.value))));
        return;
    case MetaType::UShort:
        u16(static_cast<std::uint16_t>(unsignedOf(input.value)));
        return;
    case MetaType::Char:
    case MetaType::SChar:
        u8(static_cast<std::uint8_t>(static_cast<std::int8_t>(integerOf(input.value))));
        return;
    case MetaType::UChar:
        u8(static_cast<std::uint8_t>(unsignedOf(input.value)));
        return;
    case MetaType::Character:
    case MetaType::Char16: {
        const auto units = utf8ToUtf16(stringOf(input.value));
        u16(units.empty() ? 0 : units.front());
        return;
    }
    case MetaType::Char32: {
        const auto units = utf8ToUtf16(stringOf(input.value));
        std::uint32_t cp = units.empty() ? 0 : units.front();
        if (cp >= 0xd800 && cp <= 0xdbff && units.size() > 1)
            cp = 0x10000 + ((cp - 0xd800) << 10) + (units[1] - 0xdc00);
        u32(cp);
        return;
    }
    case MetaType::String:
        string(stringOf(input.value));
        return;
    case MetaType::Url:
        byteString(stringOf(input.value));
        return;
    case MetaType::ByteArray:
        bytes(bytesOf(input.value));
        return;
    case MetaType::StringList: {
        const auto& items = listOf(input.value).items;
        std::vector<std::string> strings;
        strings.reserve(items.size());
        for (const auto& value : items)
            strings.push_back(stringOf(value));
        stringList(strings);
        return;
    }
    case MetaType::ByteArrayList: {
        const auto& items = listOf(input.value).items;
        if (items.size() > std::numeric_limits<std::uint32_t>::max())
            throw CodecError("QByteArrayList is too large");
        u32(static_cast<std::uint32_t>(items.size()));
        for (const auto& value : items)
            bytes(bytesOf(value));
        return;
    }
    case MetaType::VariantList: {
        const auto& list = listOf(input.value).items;
        if (list.size() > std::numeric_limits<std::uint32_t>::max())
            throw CodecError("QVariantList is too large");
        u32(static_cast<std::uint32_t>(list.size()));
        const bool typed = input.nestedValues.size() == list.size();
        for (std::size_t i = 0; i < list.size(); ++i)
            variant(typed ? input.nestedValues[i] : Variant::fromRpc(list[i]));
        return;
    }
    case MetaType::VariantMap:
    case MetaType::VariantHash: {
        const auto entries = sortedMapEntries(mapOf(input.value));
        if (entries.size() > std::numeric_limits<std::uint32_t>::max())
            throw CodecError("QVariantMap is too large");
        u32(static_cast<std::uint32_t>(entries.size()));
        const bool typed = input.nestedKeys.size() == input.nestedValues.size()
            && input.nestedKeys.size() == entries.size();
        std::unordered_map<std::string, const Variant*> typedValues;
        if (typed) {
            typedValues.reserve(input.nestedKeys.size());
            for (std::size_t i = 0; i < input.nestedKeys.size(); ++i)
                typedValues.emplace(input.nestedKeys[i], &input.nestedValues[i]);
        }
        for (const auto& [unused, entry] : entries) {
            (void)unused;
            string(entry->first);
            const Variant* child = nullptr;
            if (typed) {
                const auto it = typedValues.find(entry->first);
                if (it != typedValues.end()) child = it->second;
            }
            variant(child ? *child : Variant::fromRpc(entry->second));
        }
        return;
    }
    case MetaType::JsonValue: {
        if (input.jsonUndefined) {
            u8(0x80); // QJsonValue::Undefined
        } else if (input.value.isNull()) {
            u8(0); // QJsonValue::Null
        } else if (input.value.isBool()) {
            u8(1);
            boolean(input.value.asBool());
        } else if (input.value.isDouble() || input.value.isIntegral()) {
            u8(2);
            real(realOf(input.value));
        } else if (input.value.isString()) {
            u8(3);
            string(input.value.asString());
        } else if (input.value.isList()) {
            u8(4);
            bytes(jsonBytes(input.value));
        } else {
            u8(5);
            bytes(jsonBytes(input.value));
        }
        return;
    }
    case MetaType::JsonObject:
    case MetaType::JsonArray:
    case MetaType::JsonDocument:
        bytes(jsonBytes(input.value));
        return;
    case MetaType::User: {
        if (input.customType != "LogosResult")
            throw CodecError("unsupported QVariant custom type: " + input.customType);
        const auto& map = mapOf(input.value);
        boolean(boolOf(requiredMapValue(map, "success")));
        const bool typed = input.nestedValues.size() == 2;
        variant(typed ? input.nestedValues[0]
                      : Variant::fromRpc(requiredMapValue(map, "value")));
        variant(typed ? input.nestedValues[1]
                      : Variant::fromRpc(requiredMapValue(map, "error")));
        return;
    }
    case MetaType::Date:
    case MetaType::Time:
    case MetaType::DateTime:
    case MetaType::Uuid:
        throw CodecError("QVariant date/time and QUuid values need their serialized form");
    }
    throw CodecError("unsupported QVariant metatype id "
                     + std::to_string(static_cast<std::uint32_t>(input.type)));
}

void Writer::classDefinition(const ClassDefinition& value)
{
    string(value.typeName);
    u32(0); // enums declared on the class
    u32(0); // external Qt enums
    u32(0); // gadgets / external enum metaobjects
    u32(static_cast<std::uint32_t>(value.signalDefinitions.size()));
    for (const auto& signal : value.signalDefinitions) {
        byteString(signal.signature);
        byteStringList(signal.parameterNames);
    }
    u32(static_cast<std::uint32_t>(value.methodDefinitions.size()));
    for (const auto& method : value.methodDefinitions) {
        byteString(method.signature);
        byteString(method.returnType);
        byteStringList(method.parameterNames);
    }
    u32(static_cast<std::uint32_t>(value.propertyDefinitions.size()));
    for (const auto& property : value.propertyDefinitions) {
        byteString(property.name, true);
        byteString(property.typeName, true);
        byteString(property.notifySignal);
    }
}

void Writer::raw(const std::vector<std::uint8_t>& value)
{
    m_data.insert(m_data.end(), value.begin(), value.end());
}

void Writer::patchU32(std::size_t offset, std::uint32_t value)
{
    if (offset + 4 > m_data.size())
        throw CodecError("patch offset is outside buffer");
    for (std::size_t i = 0; i < 4; ++i)
        m_data[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

Reader::Reader(const std::vector<std::uint8_t>& data) : Reader(data.data(), data.size()) {}
Reader::Reader(const std::uint8_t* data, std::size_t size) : m_data(data), m_size(size) {}

void Reader::require(std::size_t count) const
{
    if (count > remaining())
        throw CodecError("truncated QtRO packet");
}

std::uint8_t Reader::u8()
{
    require(1);
    return m_data[m_pos++];
}

std::uint16_t Reader::u16()
{
    require(2);
    const auto value = static_cast<std::uint16_t>(m_data[m_pos])
        | (static_cast<std::uint16_t>(m_data[m_pos + 1]) << 8);
    m_pos += 2;
    return value;
}

std::uint32_t Reader::u32()
{
    require(4);
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i)
        value |= static_cast<std::uint32_t>(m_data[m_pos + i]) << (i * 8);
    m_pos += 4;
    return value;
}

std::uint64_t Reader::u64()
{
    require(8);
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i)
        value |= static_cast<std::uint64_t>(m_data[m_pos + i]) << (i * 8);
    m_pos += 8;
    return value;
}

std::int32_t Reader::i32() { return static_cast<std::int32_t>(u32()); }
std::int64_t Reader::i64() { return static_cast<std::int64_t>(u64()); }

bool Reader::boolean()
{
    const auto value = u8();
    if (value > 1)
        throw CodecError("invalid QDataStream bool");
    return value != 0;
}

double Reader::real()
{
    const std::uint64_t bits = u64();
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::optional<std::vector<std::uint8_t>> Reader::bytes()
{
    const auto size = u32();
    if (size == kNullSize)
        return std::nullopt;
    if (size > kMaxByteArray)
        throw CodecError("QByteArray exceeds transport limit");
    require(size);
    std::vector<std::uint8_t> value(m_data + m_pos, m_data + m_pos + size);
    m_pos += size;
    return value;
}

std::optional<std::string> Reader::string()
{
    return string(nullptr);
}

std::optional<std::string> Reader::string(bool* lossy)
{
    const auto byteCount = u32();
    if (byteCount == kNullSize)
        return std::nullopt;
    if ((byteCount & 1u) != 0 || byteCount > kMaxByteArray)
        throw CodecError("invalid QString byte count");
    require(byteCount);
    std::vector<std::uint16_t> units;
    units.reserve(byteCount / 2);
    for (std::uint32_t i = 0; i < byteCount / 2; ++i)
        units.push_back(u16());
    return utf16ToUtf8(units, lossy);
}

std::vector<std::string> Reader::stringList()
{
    const auto count = u32();
    if (count > kMaxContainerEntries)
        throw CodecError("QStringList exceeds transport limit");
    std::vector<std::string> result;
    result.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto value = string();
        result.push_back(value.value_or(std::string{}));
    }
    return result;
}

std::vector<std::string> Reader::byteStringList()
{
    const auto count = u32();
    if (count > kMaxContainerEntries)
        throw CodecError("QByteArrayList exceeds transport limit");
    std::vector<std::string> result;
    result.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto value = bytes().value_or(std::vector<std::uint8_t>{});
        result.emplace_back(value.begin(), value.end());
    }
    return result;
}

Variant Reader::variantHeader()
{
    Variant result;
    result.type = static_cast<MetaType>(u32());
    result.isNull = u8() != 0;
    if (result.type == MetaType::User) {
        auto name = bytes();
        if (!name || name->empty() || name->back() != 0)
            throw CodecError("invalid QVariant custom type name");
        result.customType.assign(name->begin(), name->end() - 1);
    }
    return result;
}

Variant Reader::variant()
{
    if (m_depth >= kMaxNesting)
        throw CodecError("QVariant nesting exceeds transport limit");
    ++m_depth;
    struct Leave {
        std::size_t& depth;
        ~Leave() { --depth; }
    } leave{m_depth};

    Variant result = variantHeader();
    const std::size_t dataStart = m_pos;
    // Keep the serialized form where the RpcValue alone cannot reproduce it.
    bool keepRaw = false;
    const auto text = [&](bool& inexact) {
        bool lossy = false;
        auto value = string(&lossy);
        inexact = !value || lossy;
        return value.value_or(std::string{});
    };

    switch (result.type) {
    case MetaType::Invalid:
        result.value = plain::RpcValue{};
        break;
    case MetaType::Bool:
        result.value = plain::RpcValue{boolean()};
        break;
    case MetaType::Int:
        result.value = plain::RpcValue{static_cast<std::int64_t>(i32())};
        break;
    case MetaType::UInt:
        result.value = plain::RpcValue::makeInteger(u32());
        break;
    case MetaType::LongLong:
    case MetaType::Long: // QMetaType::save writes long as qlonglong
        result.value = plain::RpcValue{i64()};
        break;
    case MetaType::ULongLong:
    case MetaType::ULong:
        result.value = plain::RpcValue::makeInteger(u64());
        break;
    case MetaType::Double:
    case MetaType::Float: // DoublePrecision streams write floats as doubles
        result.value = plain::RpcValue{real()};
        break;
    case MetaType::Short:
        result.value = plain::RpcValue{static_cast<std::int64_t>(static_cast<std::int16_t>(u16()))};
        break;
    case MetaType::UShort:
        result.value = plain::RpcValue{static_cast<std::int64_t>(u16())};
        break;
    case MetaType::Char:
    case MetaType::SChar:
        result.value = plain::RpcValue{static_cast<std::int64_t>(static_cast<std::int8_t>(u8()))};
        break;
    case MetaType::UChar:
        result.value = plain::RpcValue{static_cast<std::int64_t>(u8())};
        break;
    case MetaType::Character:
    case MetaType::Char16:
        result.value = plain::RpcValue{utf16ToUtf8({u16()}, nullptr)};
        keepRaw = true;
        break;
    case MetaType::Char32:
        result.value = plain::RpcValue{utf8Of(u32())};
        keepRaw = true;
        break;
    case MetaType::String: {
        bool inexact = false;
        result.value = plain::RpcValue{text(inexact)};
        keepRaw = inexact;
        break;
    }
    case MetaType::ByteArray: {
        const auto value = bytes();
        plain::RpcBytes data;
        if (value) data.data = *value;
        result.value = plain::RpcValue{std::move(data)};
        keepRaw = !value;
        break;
    }
    case MetaType::Url: { // QUrl::toEncoded(), null for an invalid URL
        const auto encoded = bytes();
        result.value = plain::RpcValue{encoded ? std::string(encoded->begin(), encoded->end())
                                               : std::string{}};
        keepRaw = true;
        break;
    }
    case MetaType::Uuid: { // little endian fields: data1, data2, data3, data4[8]
        const auto data1 = u32();
        const auto data2 = u16();
        const auto data3 = u16();
        std::uint8_t data4[8];
        for (auto& byte : data4) byte = u8();
        char textValue[40];
        std::snprintf(textValue, sizeof(textValue),
                      "{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
                      data1, data2, data3, data4[0], data4[1], data4[2], data4[3],
                      data4[4], data4[5], data4[6], data4[7]);
        result.value = plain::RpcValue{std::string(textValue)};
        keepRaw = true;
        break;
    }
    case MetaType::Date:
        result.value = plain::RpcValue{isoDate(i64())};
        keepRaw = true;
        break;
    case MetaType::Time:
        result.value = plain::RpcValue{isoTime(i32())};
        keepRaw = true;
        break;
    case MetaType::DateTime: {
        const std::string date = isoDate(i64());
        const std::string time = isoTime(i32());
        const auto spec = static_cast<std::int8_t>(u8());
        std::string suffix;
        if (spec == 1) { // Qt::UTC
            suffix = "Z";
        } else if (spec == 2) { // Qt::OffsetFromUTC
            suffix = isoOffset(i32());
        } else if (spec == 3) { // Qt::TimeZone, then a QTimeZone
            bool ignored = false;
            const std::string zone = text(ignored);
            if (zone == "OffsetFromUtc") {
                (void)text(ignored);
                suffix = isoOffset(i32());
                (void)text(ignored);
                (void)text(ignored);
                (void)i32();
                (void)text(ignored);
            } else if (zone == "AheadOfUtcBy") {
                suffix = isoOffset(i32());
            } else if (zone == "QTimeZone::UTC") {
                suffix = "Z";
            } else if (zone != "QTimeZone::LocalTime" && zone != "-No Time Zone Specified!") {
                suffix = "[" + zone + "]";
            }
        }
        result.value = plain::RpcValue{date.empty() || time.empty()
                                           ? std::string{} : date + "T" + time + suffix};
        keepRaw = true;
        break;
    }
    case MetaType::StringList: {
        const auto count = u32();
        if (count > kMaxContainerEntries)
            throw CodecError("QStringList exceeds transport limit");
        plain::RpcList list;
        list.items.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            bool inexact = false;
            list.items.emplace_back(text(inexact));
            keepRaw = keepRaw || inexact;
        }
        result.value = plain::RpcValue{std::move(list)};
        break;
    }
    case MetaType::ByteArrayList: {
        const auto count = u32();
        if (count > kMaxContainerEntries)
            throw CodecError("QByteArrayList exceeds transport limit");
        plain::RpcList list;
        list.items.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto value = bytes();
            plain::RpcBytes data;
            if (value) data.data = *value;
            list.items.emplace_back(std::move(data));
            keepRaw = keepRaw || !value;
        }
        result.value = plain::RpcValue{std::move(list)};
        break;
    }
    case MetaType::VariantList: {
        const auto count = u32();
        if (count > kMaxContainerEntries)
            throw CodecError("QVariantList exceeds transport limit");
        plain::RpcList list;
        list.items.reserve(count);
        result.nestedValues.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            Variant child = variant();
            list.items.push_back(child.value);
            result.nestedValues.push_back(std::move(child));
        }
        result.value = plain::RpcValue{std::move(list)};
        break;
    }
    case MetaType::VariantMap:
    case MetaType::VariantHash: {
        const auto count = u32();
        if (count > kMaxContainerEntries)
            throw CodecError("QVariantMap exceeds transport limit");
        plain::RpcMap map;
        map.entries.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            // Function-argument evaluation order is unspecified in C++17.
            // Decode the key before the value explicitly so GCC/MinGW cannot
            // consume the QVariant bytes while the reader still points at the
            // QString key.
            auto key = string().value_or(std::string{});
            Variant child = variant();
            map.emplace(key, child.value);
            result.nestedKeys.push_back(std::move(key));
            result.nestedValues.push_back(std::move(child));
        }
        result.value = plain::RpcValue{std::move(map)};
        break;
    }
    case MetaType::JsonValue: {
        switch (u8()) {
        case 0: result.value = plain::RpcValue{}; break;
        case 1: result.value = plain::RpcValue{boolean()}; break;
        case 2: result.value = plain::RpcValue{real()}; break;
        case 3: result.value = plain::RpcValue{string().value_or(std::string{})}; break;
        case 4: result.value = parseJsonBytes(bytes(), plain::RpcValue{plain::RpcList{}}); break;
        case 5: result.value = parseJsonBytes(bytes(), plain::RpcValue{plain::RpcMap{}}); break;
        case 0x80:
            result.value = plain::RpcValue{};
            result.jsonUndefined = true;
            break;
        default: throw CodecError("invalid QJsonValue type");
        }
        break;
    }
    case MetaType::JsonObject:
        result.value = parseJsonBytes(bytes(), plain::RpcValue{plain::RpcMap{}});
        break;
    case MetaType::JsonArray:
        result.value = parseJsonBytes(bytes(), plain::RpcValue{plain::RpcList{}});
        break;
    case MetaType::JsonDocument: {
        const auto payload = bytes();
        result.value = parseJsonBytes(payload);
        keepRaw = !payload || payload->empty();
        break;
    }
    case MetaType::User: {
        if (result.customType != "LogosResult")
            throw CodecError("unsupported QVariant custom type: " + result.customType);
        plain::RpcMap map;
        map.emplace("success", plain::RpcValue{boolean()});
        Variant value = variant();
        Variant error = variant();
        map.emplace("value", value.value);
        map.emplace("error", error.value);
        result.nestedValues.push_back(std::move(value));
        result.nestedValues.push_back(std::move(error));
        result.value = plain::RpcValue{std::move(map)};
        break;
    }
    default:
        throw CodecError("unsupported QVariant metatype id "
                         + std::to_string(static_cast<std::uint32_t>(result.type)));
    }
    if (keepRaw)
        result.raw.assign(m_data + dataStart, m_data + m_pos);
    return result;
}

Variant Reader::variantUntil(std::size_t end)
{
    if (end > m_size || end < m_pos)
        throw CodecError("QVariant bounds are outside the packet");
    const std::size_t start = m_pos;
    try {
        Variant value = variant();
        if (m_pos != end)
            throw CodecError("QVariant does not end where the packet does");
        return value;
    } catch (const CodecError&) {
        m_pos = start;
        Variant value = variantHeader();
        value.opaque = true;
        value.raw.assign(m_data + m_pos, m_data + end);
        m_pos = end;
        return value;
    }
}

ClassDefinition Reader::classDefinition()
{
    ClassDefinition result;
    result.typeName = string().value_or(std::string{});
    if (u32() != 0 || u32() != 0 || u32() != 0)
        throw CodecError("QtRO enums and gadgets are outside the Logos plain profile");

    const auto signalCount = u32();
    if (signalCount > kMaxContainerEntries)
        throw CodecError("signal definition count exceeds transport limit");
    result.signalDefinitions.reserve(signalCount);
    for (std::uint32_t i = 0; i < signalCount; ++i) {
        auto signature = bytes().value_or(std::vector<std::uint8_t>{});
        result.signalDefinitions.push_back({std::string(signature.begin(), signature.end()), byteStringList()});
    }

    const auto methodCount = u32();
    if (methodCount > kMaxContainerEntries)
        throw CodecError("method definition count exceeds transport limit");
    result.methodDefinitions.reserve(methodCount);
    for (std::uint32_t i = 0; i < methodCount; ++i) {
        auto signature = bytes().value_or(std::vector<std::uint8_t>{});
        auto returnType = bytes().value_or(std::vector<std::uint8_t>{});
        result.methodDefinitions.push_back({std::string(signature.begin(), signature.end()),
                                             std::string(returnType.begin(), returnType.end()),
                                             byteStringList()});
    }

    const auto propertyCount = u32();
    if (propertyCount > kMaxContainerEntries)
        throw CodecError("property definition count exceeds transport limit");
    result.propertyDefinitions.reserve(propertyCount);
    for (std::uint32_t i = 0; i < propertyCount; ++i) {
        auto name = bytes().value_or(std::vector<std::uint8_t>{});
        auto type = bytes().value_or(std::vector<std::uint8_t>{});
        auto signal = bytes().value_or(std::vector<std::uint8_t>{});
        if (!name.empty() && name.back() == 0) name.pop_back();
        if (!type.empty() && type.back() == 0) type.pop_back();
        result.propertyDefinitions.push_back({std::string(name.begin(), name.end()),
                                               std::string(type.begin(), type.end()),
                                               std::string(signal.begin(), signal.end())});
    }
    return result;
}

std::vector<std::uint8_t> Reader::remainingBytes()
{
    std::vector<std::uint8_t> result(m_data + m_pos, m_data + m_size);
    m_pos = m_size;
    return result;
}

std::vector<std::uint8_t> handshakePacket()
{
    return finishPacket(packetPrefix(PacketType::Handshake, kProtocolVersion));
}

std::vector<std::uint8_t> objectListPacket(const std::vector<ObjectInfo>& objects)
{
    auto writer = packetPrefix(PacketType::ObjectList, {});
    writer.u32(static_cast<std::uint32_t>(objects.size()));
    for (const auto& object : objects) {
        writer.string(object.name);
        writer.string(object.typeName.value_or(std::string{}), !object.typeName.has_value());
        writer.bytes(object.signature);
    }
    return finishPacket(std::move(writer));
}

std::vector<std::uint8_t> addObjectPacket(std::string_view name, bool dynamic)
{
    auto writer = packetPrefix(PacketType::AddObject, name);
    writer.boolean(dynamic);
    return finishPacket(std::move(writer));
}

std::vector<std::uint8_t> removeObjectPacket(std::string_view name)
{
    return finishPacket(packetPrefix(PacketType::RemoveObject, name));
}

std::vector<std::uint8_t> initPacket(
    std::string_view name,
    const std::vector<Variant>& properties)
{
    auto writer = packetPrefix(PacketType::Init, name);
    writer.u32(static_cast<std::uint32_t>(properties.size()));
    for (const auto& property : properties)
        writer.variant(property);
    return finishPacket(std::move(writer));
}

std::vector<std::uint8_t> initDynamicPacket(
    std::string_view name,
    const ClassDefinition& definition,
    const std::vector<Variant>& properties)
{
    auto writer = packetPrefix(PacketType::InitDynamic, name);
    writer.classDefinition(definition);
    writer.u32(static_cast<std::uint32_t>(properties.size()));
    for (const auto& property : properties)
        writer.variant(property);
    return finishPacket(std::move(writer));
}

std::vector<std::uint8_t> invokePacket(
    std::string_view name,
    std::int32_t call,
    std::int32_t index,
    const std::vector<Variant>& args,
    std::int32_t serialId,
    std::int32_t propertyIndex)
{
    auto writer = packetPrefix(PacketType::Invoke, name);
    writer.i32(call);
    writer.i32(index);
    writer.u32(static_cast<std::uint32_t>(args.size()));
    for (const auto& arg : args)
        writer.variant(arg);
    writer.i32(serialId);
    writer.i32(propertyIndex);
    return finishPacket(std::move(writer));
}

std::vector<std::uint8_t> invokeReplyPacket(
    std::string_view name,
    std::int32_t serialId,
    const Variant& value)
{
    auto writer = packetPrefix(PacketType::InvokeReply, name);
    writer.i32(serialId);
    writer.variant(value);
    return finishPacket(std::move(writer));
}

std::vector<std::uint8_t> pingPacket(PacketType type, std::string_view name)
{
    if (type != PacketType::Ping && type != PacketType::Pong)
        throw CodecError("pingPacket requires Ping or Pong");
    return finishPacket(packetPrefix(type, name));
}

Frame decodeFrame(const std::vector<std::uint8_t>& bytes)
{
    Reader reader(bytes);
    const auto payloadSize = reader.u32();
    if (payloadSize != reader.remaining())
        throw CodecError("QtRO frame size does not match packet bytes");
    Frame frame;
    frame.type = static_cast<PacketType>(reader.u16());
    if (frame.type != PacketType::ObjectList)
        frame.name = reader.string().value_or(std::string{});
    frame.payload = reader.remainingBytes();
    return frame;
}

std::optional<std::size_t> completeFrameSize(const std::vector<std::uint8_t>& buffered)
{
    if (buffered.size() < 4)
        return std::nullopt;
    const std::uint32_t payload = static_cast<std::uint32_t>(buffered[0])
        | (static_cast<std::uint32_t>(buffered[1]) << 8)
        | (static_cast<std::uint32_t>(buffered[2]) << 16)
        | (static_cast<std::uint32_t>(buffered[3]) << 24);
    if (payload > kMaxByteArray)
        throw CodecError("QtRO frame exceeds transport limit");
    const std::size_t total = static_cast<std::size_t>(payload) + 4;
    return buffered.size() >= total ? std::optional<std::size_t>{total} : std::nullopt;
}

ClassDefinition moduleProxyDefinition()
{
    return {
        "ModuleProxy",
        {{"eventResponse(QString,QVariantList)", {"eventName", "data"}}},
        {
            {"callRemoteMethod(QString,QString,QVariantList)", "QVariant", {"authToken", "methodName", "args"}},
            {"callRemoteMethod(QString,QString)", "QVariant", {"authToken", "methodName"}},
            {"callRemoteMethod(QString,QString,QVariantList,QString)", "QVariant", {"authToken", "methodName", "args", "transportProtocol"}},
            {"informModuleToken(QString,QString,QString)", "bool", {"authToken", "moduleName", "token"}},
            {"getPluginMethods()", "QJsonArray", {}},
            {"getPluginEvents()", "QJsonArray", {}},
            {"getPluginInterface()", "QJsonArray", {}},
        },
        {},
    };
}

ClassDefinition moduleHandshakeProxyDefinition()
{
    return {
        "ModuleHandshakeProxy",
        {},
        {{"informModuleToken(QString,QString,QString)", "bool", {"authToken", "moduleName", "token"}}},
        {},
    };
}

} // namespace logos::qt_remote_plain
