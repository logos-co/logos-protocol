#include "json_mapping.h"

#include "../../logos_codec.h"

#include <type_traits>

namespace logos::plain {

using json = nlohmann::json;

// ── RpcValue ↔ json ─────────────────────────────────────────────────────────
//
// Mapping:
//   null   ↔ json null
//   bool   ↔ json boolean
//   int64  ↔ json integer
//   double ↔ json number (non-integer)
//   string ↔ json string
//   bytes  ↔ {"_bytes": base64url}
//   list   ↔ json array
//   map    ↔ json object (we disambiguate bytes via the "_bytes" key)
//
// JSON has no bytes primitive, so `bytes` round-trip via a tagged object.
// CBOR has native byte strings; when we want a "real" CBOR byte
// representation we can upgrade CborCodec to bypass this hack and use
// `json::binary_t` — for now identical behaviour keeps the code paths
// uniform and tested.

namespace {

json valueToJson(const RpcValue& v);
RpcValue jsonToValue(const json& j);

json valueToJson(const RpcValue& v)
{
    if (v.isNull())   return nullptr;
    if (v.isBool())   return v.asBool();
    if (v.isInt())    return v.asInt();
    if (v.isUInt())   return v.asUInt();   // > int64max: nlohmann keeps it as number_unsigned
    if (v.isDouble()) return v.asDouble();
    if (v.isString()) return v.asString();
    if (v.isBytes())  return logos::bytesToJson(v.asBytes().data);
    if (v.isList()) {
        json arr = json::array();
        for (const auto& item : v.asList().items) arr.push_back(valueToJson(item));
        return arr;
    }
    if (v.isMap()) {
        json obj = json::object();
        for (const auto& [k, val] : v.asMap().entries) obj[k] = valueToJson(val);
        return obj;
    }
    return nullptr;
}

RpcValue jsonToValue(const json& j)
{
    if (j.is_null())    return RpcValue{std::monostate{}};
    if (j.is_boolean()) return RpcValue{j.get<bool>()};
    // is_number_unsigned() is checked FIRST and routed through makeInteger:
    // .get<int64_t>() on a value above int64max wraps silently (2^64-1 -> -1)
    // with no exception, so a correct peer's uint64 used to arrive as -1.
    if (j.is_number_unsigned())
        return RpcValue::makeInteger(j.get<uint64_t>());
    if (j.is_number_integer())
        return RpcValue{j.get<int64_t>()};
    if (j.is_number_float()) return RpcValue{j.get<double>()};
    if (j.is_string())       return RpcValue{j.get<std::string>()};
    if (j.is_array()) {
        RpcList list;
        list.items.reserve(j.size());
        for (const auto& e : j) list.items.push_back(jsonToValue(e));
        return RpcValue{std::move(list)};
    }
    if (j.is_object()) {
        // Disambiguate bytes.
        if (logos::isTaggedBytes(j)) {
            std::vector<uint8_t> bytes;
            if (!logos::b64UrlDecodeChecked(j["_bytes"].get<std::string>(), bytes))
                throw CodecError("invalid base64url input");
            return RpcValue{RpcBytes{std::move(bytes)}};
        }
        RpcMap map;
        for (auto it = j.begin(); it != j.end(); ++it)
            map.emplace(it.key(), jsonToValue(it.value()));
        return RpcValue{std::move(map)};
    }
    // CBOR round-trips through nlohmann::json::binary as binary_t. Convert
    // to our bytes representation so CborCodec and JsonCodec end up with
    // the same logical RpcValue shape.
    if (j.is_binary()) {
        const auto& b = j.get_binary();
        return RpcValue{RpcBytes{std::vector<uint8_t>(b.begin(), b.end())}};
    }
    return RpcValue{std::monostate{}};
}

// ── Message struct ↔ json helpers ──────────────────────────────────────────

json methodToJson(const MethodMetadata& m)
{
    json o = json::object();
    o["name"] = m.name;
    o["signature"] = m.signature;
    o["returnType"] = m.returnType;
    o["isInvokable"] = m.isInvokable;
    json pa = json::array();
    for (const auto& p : m.parameters.items) pa.push_back(valueToJson(p));
    o["parameters"] = std::move(pa);
    return o;
}

MethodMetadata methodFromJson(const json& j)
{
    MethodMetadata m;
    m.name        = j.value("name", std::string{});
    m.signature   = j.value("signature", std::string{});
    m.returnType  = j.value("returnType", std::string{});
    m.isInvokable = j.value("isInvokable", true);
    if (j.contains("parameters") && j["parameters"].is_array()) {
        for (const auto& p : j["parameters"]) m.parameters.items.push_back(jsonToValue(p));
    }
    return m;
}

json argsToJson(const std::vector<RpcValue>& args)
{
    json a = json::array();
    for (const auto& v : args) a.push_back(valueToJson(v));
    return a;
}

std::vector<RpcValue> argsFromJson(const json& j)
{
    std::vector<RpcValue> out;
    if (j.is_array()) {
        out.reserve(j.size());
        for (const auto& e : j) out.push_back(jsonToValue(e));
    }
    return out;
}

} // anonymous namespace

// ── Public entry points ────────────────────────────────────────────────────

json messageToJson(const AnyMessage& msg)
{
    return std::visit([](const auto& m) -> json {
        using T = std::decay_t<decltype(m)>;
        json o = json::object();
        if constexpr (std::is_same_v<T, CallMessage>) {
            o["id"] = m.id;
            o["authToken"] = m.authToken;
            o["object"] = m.object;
            o["method"] = m.method;
            o["args"] = argsToJson(m.args);
        } else if constexpr (std::is_same_v<T, ResultMessage>) {
            o["id"] = m.id;
            o["ok"] = m.ok;
            if (m.ok) {
                o["value"] = valueToJson(m.value);
            } else {
                o["err"] = m.err;
                o["errCode"] = m.errCode;
            }
        } else if constexpr (std::is_same_v<T, SubscribeMessage>) {
            o["object"] = m.object;
            o["event"]  = m.eventName;
        } else if constexpr (std::is_same_v<T, UnsubscribeMessage>) {
            o["object"] = m.object;
            o["event"]  = m.eventName;
        } else if constexpr (std::is_same_v<T, EventMessage>) {
            o["object"] = m.object;
            o["event"]  = m.eventName;
            o["data"]   = argsToJson(m.data);
        } else if constexpr (std::is_same_v<T, TokenMessage>) {
            o["authToken"]  = m.authToken;
            o["moduleName"] = m.moduleName;
            o["token"]      = m.token;
        } else if constexpr (std::is_same_v<T, MethodsMessage>) {
            o["id"]        = m.id;
            o["authToken"] = m.authToken;
            o["object"]    = m.object;
        } else if constexpr (std::is_same_v<T, MethodsResultMessage>) {
            o["id"] = m.id;
            o["ok"] = m.ok;
            if (m.ok) {
                json ma = json::array();
                for (const auto& md : m.methods) ma.push_back(methodToJson(md));
                o["methods"] = std::move(ma);
            } else {
                o["err"] = m.err;
            }
        }
        return o;
    }, msg);
}

AnyMessage jsonToMessage(MessageType tag, const json& j)
{
    if (!j.is_object()) throw CodecError("expected top-level object");
    switch (tag) {
    case MessageType::Call: {
        CallMessage m;
        m.id        = j.value("id", uint64_t{0});
        m.authToken = j.value("authToken", std::string{});
        m.object    = j.value("object", std::string{});
        m.method    = j.value("method", std::string{});
        if (j.contains("args")) m.args = argsFromJson(j["args"]);
        return m;
    }
    case MessageType::Result: {
        ResultMessage m;
        m.id = j.value("id", uint64_t{0});
        m.ok = j.value("ok", false);
        if (m.ok) {
            if (j.contains("value")) m.value = jsonToValue(j["value"]);
        } else {
            m.err     = j.value("err", std::string{});
            m.errCode = j.value("errCode", std::string{});
        }
        return m;
    }
    case MessageType::Subscribe: {
        SubscribeMessage m;
        m.object    = j.value("object", std::string{});
        m.eventName = j.value("event",  std::string{});
        return m;
    }
    case MessageType::Unsubscribe: {
        UnsubscribeMessage m;
        m.object    = j.value("object", std::string{});
        m.eventName = j.value("event",  std::string{});
        return m;
    }
    case MessageType::Event: {
        EventMessage m;
        m.object    = j.value("object", std::string{});
        m.eventName = j.value("event",  std::string{});
        if (j.contains("data")) m.data = argsFromJson(j["data"]);
        return m;
    }
    case MessageType::Token: {
        TokenMessage m;
        m.authToken  = j.value("authToken",  std::string{});
        m.moduleName = j.value("moduleName", std::string{});
        m.token      = j.value("token",      std::string{});
        return m;
    }
    case MessageType::Methods: {
        MethodsMessage m;
        m.id        = j.value("id",        uint64_t{0});
        m.authToken = j.value("authToken", std::string{});
        m.object    = j.value("object",    std::string{});
        return m;
    }
    case MessageType::MethodsResult: {
        MethodsResultMessage m;
        m.id = j.value("id", uint64_t{0});
        m.ok = j.value("ok", false);
        if (m.ok && j.contains("methods") && j["methods"].is_array()) {
            for (const auto& md : j["methods"]) m.methods.push_back(methodFromJson(md));
        } else if (!m.ok) {
            m.err = j.value("err", std::string{});
        }
        return m;
    }
    }
    throw CodecError("unknown message tag");
}

} // namespace logos::plain
