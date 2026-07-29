#include "qvariant_rpc_value.h"

#include "../../logos_types.h"

#include <QMetaType>

#include <cmath>
#include <limits>

namespace logos::plain {

namespace {

RpcValue fromJsonValue(const QJsonValue& v);
QJsonValue toJsonValue(const RpcValue& v);

RpcValue fromJsonValue(const QJsonValue& v)
{
    switch (v.type()) {
    case QJsonValue::Null:      return RpcValue{std::monostate{}};
    case QJsonValue::Bool:      return RpcValue{v.toBool()};
    case QJsonValue::Double: {
        double d = v.toDouble();
        double intPart = 0.0;
        // Strict `<` on the upper bound: double(int64max) rounds UP to exactly
        // 2^63, so `d <= double(int64max)` admitted d == 2^63, and int64_t(d) on
        // an out-of-range double is undefined behaviour — saturating to int64max
        // on arm64, INT64_MIN on x86-64. The lower bound needs no such care:
        // double(int64min) is exactly -2^63 and representable.
        if (std::modf(d, &intPart) == 0.0 &&
            d >= double(std::numeric_limits<int64_t>::min()) &&
            d < 9223372036854775808.0)   // 2^63, i.e. int64max + 1
            return RpcValue{int64_t(d)};
        return RpcValue{d};
    }
    case QJsonValue::String:    return RpcValue{v.toString().toStdString()};
    case QJsonValue::Array: {
        RpcList out;
        const auto arr = v.toArray();
        out.items.reserve(arr.size());
        for (const QJsonValue& e : arr) out.items.push_back(fromJsonValue(e));
        return RpcValue{std::move(out)};
    }
    case QJsonValue::Object: {
        RpcMap out;
        const auto obj = v.toObject();
        for (auto it = obj.begin(); it != obj.end(); ++it)
            out.emplace(it.key().toStdString(), fromJsonValue(it.value()));
        return RpcValue{std::move(out)};
    }
    default:
        return RpcValue{std::monostate{}};
    }
}

QJsonValue toJsonValue(const RpcValue& v)
{
    if (v.isNull())   return QJsonValue(QJsonValue::Null);
    if (v.isBool())   return QJsonValue(v.asBool());
    if (v.isInt())    return QJsonValue(static_cast<double>(v.asInt()));
    // QJsonValue has no unsigned primitive and its double cannot hold the band
    // above int64max exactly. This path only carries method-introspection
    // METADATA (parameter descriptors), never payload values, so the lossy cast
    // is acceptable here — but without this branch a uint64 would fall through
    // to Null, which is worse than imprecise.
    if (v.isUInt())   return QJsonValue(static_cast<double>(v.asUInt()));
    if (v.isDouble()) return QJsonValue(v.asDouble());
    if (v.isString()) return QJsonValue(QString::fromStdString(v.asString()));
    if (v.isBytes()) {
        // QJsonValue has no bytes primitive; encode as base64 string.
        const auto& b = v.asBytes().data;
        QByteArray ba(reinterpret_cast<const char*>(b.data()),
                      static_cast<int>(b.size()));
        return QJsonValue(QString::fromLatin1(ba.toBase64(QByteArray::Base64UrlEncoding)));
    }
    if (v.isList()) {
        QJsonArray arr;
        for (const auto& e : v.asList().items) arr.append(toJsonValue(e));
        return arr;
    }
    if (v.isMap()) {
        QJsonObject obj;
        for (const auto& kv : v.asMap().entries)
            obj.insert(QString::fromStdString(kv.first), toJsonValue(kv.second));
        return obj;
    }
    return QJsonValue(QJsonValue::Null);
}

} // anonymous namespace

RpcValue qvariantToRpcValue(const QVariant& v)
{
    if (!v.isValid()) return RpcValue{std::monostate{}};

    // LogosResult is a user-defined struct registered via qRegisterMetaType;
    // its metatype id is assigned at runtime so we can't put it in the
    // switch on QMetaType::Type below. Check it first — if we let it fall
    // through to the default, we'd stringify it via QVariant::toString()
    // (returning "" because LogosResult has no QString converter) or,
    // earlier, lose it as std::monostate{} and the receiver would see null.
    //
    // Wire shape: {"success": bool, "value": <any>, "error": <any>}.
    // That matches the struct's fields and recursively reuses the RpcValue
    // conversion for `value` and `error`, which themselves are QVariants
    // carrying primitives / QVariantMap / QVariantList / etc.
    //
    // Look up the metatype id per call (not cached in a `static`): the
    // first `qvariantToRpcValue` call might land before any `LogosAPI`
    // has called `qRegisterMetaType<LogosResult>`, and we don't want to
    // permanently cache `UnknownType` in that case. The lookup is a
    // hash probe — trivially cheap compared to the actual RPC work.
    {
        const int logosResultId = QMetaType::fromName("LogosResult").id();
        if (logosResultId != QMetaType::UnknownType && v.userType() == logosResultId) {
            const LogosResult r = v.value<LogosResult>();
            RpcMap m;
            m.emplace("success", RpcValue{r.success});
            m.emplace("value",   qvariantToRpcValue(r.value));
            m.emplace("error",   qvariantToRpcValue(r.error));
            return RpcValue{std::move(m)};
        }
    }

    // Fast path for the common scalar types.
    switch (static_cast<QMetaType::Type>(v.userType())) {
    case QMetaType::Bool:     return RpcValue{v.toBool()};
    case QMetaType::Int:
    case QMetaType::Long:
    case QMetaType::LongLong:
    case QMetaType::Short:
    case QMetaType::Char:
    case QMetaType::SChar:
        return RpcValue{int64_t(v.toLongLong())};
    case QMetaType::UInt:
    case QMetaType::ULong:
    case QMetaType::ULongLong:
    case QMetaType::UShort:
    case QMetaType::UChar:
        // makeInteger, not int64_t(): a LIDL `uint` above int64max used to wrap
        // to -1 here, silently and in every direction. Values that fit int64_t
        // still take the int64_t alternative, so nothing else changes.
        return RpcValue::makeInteger(v.toULongLong());
    case QMetaType::Float:
    case QMetaType::Double:
        return RpcValue{v.toDouble()};
    case QMetaType::QString:
        return RpcValue{v.toString().toStdString()};
    case QMetaType::QByteArray: {
        QByteArray ba = v.toByteArray();
        RpcBytes b;
        b.data.assign(reinterpret_cast<const uint8_t*>(ba.data()),
                      reinterpret_cast<const uint8_t*>(ba.data()) + ba.size());
        return RpcValue{std::move(b)};
    }
    case QMetaType::QVariantList: {
        RpcList list;
        const QVariantList src = v.toList();
        list.items.reserve(src.size());
        for (const QVariant& e : src) list.items.push_back(qvariantToRpcValue(e));
        return RpcValue{std::move(list)};
    }
    case QMetaType::QVariantMap: {
        RpcMap map;
        const QVariantMap src = v.toMap();
        for (auto it = src.begin(); it != src.end(); ++it)
            map.emplace(it.key().toStdString(), qvariantToRpcValue(it.value()));
        return RpcValue{std::move(map)};
    }
    case QMetaType::QJsonValue:
        return fromJsonValue(v.toJsonValue());
    case QMetaType::QJsonArray: {
        RpcList list;
        const QJsonArray arr = v.toJsonArray();
        list.items.reserve(arr.size());
        for (const QJsonValue& e : arr) list.items.push_back(fromJsonValue(e));
        return RpcValue{std::move(list)};
    }
    case QMetaType::QJsonObject: {
        RpcMap map;
        const QJsonObject obj = v.toJsonObject();
        for (auto it = obj.begin(); it != obj.end(); ++it)
            map.emplace(it.key().toStdString(), fromJsonValue(it.value()));
        return RpcValue{std::move(map)};
    }
    default:
        // Best-effort fallback: stringify.
        if (v.canConvert<QString>()) return RpcValue{v.toString().toStdString()};
        return RpcValue{std::monostate{}};
    }
}

QVariant rpcValueToQVariant(const RpcValue& v)
{
    if (v.isNull())   return QVariant();
    if (v.isBool())   return QVariant(v.asBool());
    if (v.isInt())    return QVariant(static_cast<qlonglong>(v.asInt()));
    if (v.isUInt())   return QVariant(static_cast<qulonglong>(v.asUInt()));
    if (v.isDouble()) return QVariant(v.asDouble());
    if (v.isString()) return QVariant(QString::fromStdString(v.asString()));
    if (v.isBytes()) {
        const auto& b = v.asBytes().data;
        return QVariant(QByteArray(reinterpret_cast<const char*>(b.data()),
                                   static_cast<int>(b.size())));
    }
    if (v.isList()) return QVariant(rpcListToQVariantList(v.asList().items));
    if (v.isMap()) {
        QVariantMap map;
        for (const auto& kv : v.asMap().entries)
            map.insert(QString::fromStdString(kv.first), rpcValueToQVariant(kv.second));
        return QVariant(std::move(map));
    }
    return QVariant();
}

std::vector<RpcValue> qvariantListToRpcList(const QVariantList& list)
{
    std::vector<RpcValue> out;
    out.reserve(list.size());
    for (const QVariant& e : list) out.push_back(qvariantToRpcValue(e));
    return out;
}

QVariantList rpcListToQVariantList(const std::vector<RpcValue>& list)
{
    QVariantList out;
    out.reserve(list.size());
    for (const auto& e : list) out.append(rpcValueToQVariant(e));
    return out;
}

QJsonArray methodsToJsonArray(const std::vector<MethodMetadata>& methods)
{
    QJsonArray out;
    for (const auto& m : methods) {
        QJsonObject o;
        o["name"]        = QString::fromStdString(m.name);
        o["signature"]   = QString::fromStdString(m.signature);
        o["returnType"]  = QString::fromStdString(m.returnType);
        o["isInvokable"] = m.isInvokable;
        QJsonArray params;
        for (const auto& p : m.parameters.items) params.append(toJsonValue(p));
        o["parameters"] = std::move(params);
        out.append(o);
    }
    return out;
}

std::vector<MethodMetadata> methodsFromJsonArray(const QJsonArray& arr)
{
    std::vector<MethodMetadata> out;
    out.reserve(arr.size());
    for (const QJsonValue& v : arr) {
        if (!v.isObject()) continue;
        const auto o = v.toObject();
        MethodMetadata m;
        m.name        = o.value("name").toString().toStdString();
        m.signature   = o.value("signature").toString().toStdString();
        m.returnType  = o.value("returnType").toString().toStdString();
        m.isInvokable = o.value("isInvokable").toBool(true);
        if (o.contains("parameters") && o.value("parameters").isArray()) {
            for (const QJsonValue& p : o.value("parameters").toArray())
                m.parameters.items.push_back(fromJsonValue(p));
        }
        out.push_back(std::move(m));
    }
    return out;
}

} // namespace logos::plain
