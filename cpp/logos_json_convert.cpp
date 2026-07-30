#include "logos_json_convert.h"

#include "logos_codec.h"
#include "logos_types.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QMetaType>

namespace logos {

namespace {

// Canonical C-ABI bytes encoding: {"_bytes": "<base64url, unpadded>"} — a
// single-key object, lossless for arbitrary bytes incl. embedded NUL.
// Matches the plain wire's encoding (implementations/plain/json_mapping.cpp);
// Qt's Base64UrlEncoding|OmitTrailingEquals produces the identical alphabet
// and padding-free form.
// Delegates to the canonical codec so the Qt consumer path cannot drift from the
// wire or from the providers: same alphabet, same padding rule, same single-key
// shape. Only the QByteArray <-> std::vector<uint8_t> hop lives here.
nlohmann::json byteArrayToTaggedJson(const QByteArray& bytes)
{
    const auto* p = reinterpret_cast<const uint8_t*>(bytes.constData());
    return logos::bytesToJson(std::vector<uint8_t>(p, p + bytes.size()));
}

QByteArray taggedJsonToByteArray(const nlohmann::json& j)
{
    const std::vector<uint8_t> bytes = logos::bytesFromJson(j);
    return QByteArray(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<qsizetype>(bytes.size()));
}

} // namespace

nlohmann::json qvariantToNlohmann(const QVariant& v)
{
    if (v.userType() == QMetaType::QByteArray)
        return byteArrayToTaggedJson(v.toByteArray());

    const int logosResultId = QMetaType::fromName("LogosResult").id();
    if (logosResultId != QMetaType::UnknownType && v.userType() == logosResultId) {
        const LogosResult lr = v.value<LogosResult>();
        nlohmann::json obj;
        obj["success"] = lr.success;
        // Recurse so the result value keeps its exact shape: bytes -> tagged
        // form, nested integers stay integers, maps/lists preserved (the old
        // QJsonValue::fromVariant path degraded numerics to double and mangled
        // bytes).
        obj["value"] = qvariantToNlohmann(lr.value);
        QJsonValue errJson = QJsonValue::fromVariant(lr.error);
        obj["error"] = errJson.isString() ? nlohmann::json(errJson.toString().toStdString())
                                           : nullptr;
        return obj;
    }

    // Integers stay integers: QJsonValue::fromVariant degrades every numeric
    // to double, which a strict consumer on the other side of the C ABI
    // (e.g. a generated dispatch reading an int param) must not see as 5.0.
    switch (v.userType()) {
    case QMetaType::Int:       return v.toInt();
    case QMetaType::UInt:      return v.toUInt();
    case QMetaType::LongLong:  return static_cast<int64_t>(v.toLongLong());
    case QMetaType::ULongLong: return static_cast<uint64_t>(v.toULongLong());
    default: break;
    }

    // Containers: recurse element-by-element so NESTED integers keep their type
    // too. The QJsonValue::fromVariant fallback below degrades every numeric to
    // double at every depth, so a `[int]` param (a QVariantList of ints) would
    // arrive as [1.0, 2.0, ...] and a strict `.get<std::vector<int64_t>>()` on
    // the C-ABI side throws -> the dispatch decodes an empty vector. The scalar
    // switch above only covers a top-level number; these cover the list/map/
    // string-list wrappers that carry them.
    if (v.userType() == QMetaType::QStringList) {
        nlohmann::json arr = nlohmann::json::array();
        for (const QString& s : v.toStringList())
            arr.push_back(s.toStdString());
        return arr;
    }
    if (v.userType() == QMetaType::QVariantList) {
        nlohmann::json arr = nlohmann::json::array();
        for (const QVariant& e : v.toList())
            arr.push_back(qvariantToNlohmann(e));
        return arr;
    }
    if (v.userType() == QMetaType::QVariantMap) {
        nlohmann::json obj = nlohmann::json::object();
        const QVariantMap m = v.toMap();
        for (auto it = m.constBegin(); it != m.constEnd(); ++it)
            obj[it.key().toStdString()] = qvariantToNlohmann(it.value());
        return obj;
    }

    // Fallbacks for QVariants that are natively Qt JSON types (e.g. a module
    // handed back a QJsonObject/QJsonArray directly). These run AFTER the
    // container recursion above ON PURPOSE: a QVariantList/QVariantMap also
    // reports canConvert<QJsonArray/Object>(), and routing it through QJson here
    // would flatten a nested QByteArray to a plain string (QJson has no byte
    // type) and degrade nested numerics to double — the exact losses the
    // recursion prevents. So containers must be handled first; only genuine
    // QJson-typed variants reach this point.
    if (v.canConvert<QJsonObject>()) {
        QJsonDocument doc(v.toJsonObject());
        try { return nlohmann::json::parse(doc.toJson(QJsonDocument::Compact).toStdString()); }
        catch (...) {}
    }
    if (v.canConvert<QJsonArray>()) {
        QJsonDocument doc(qvariant_cast<QJsonArray>(v));
        try { return nlohmann::json::parse(doc.toJson(QJsonDocument::Compact).toStdString()); }
        catch (...) {}
    }

    QJsonValue jv = QJsonValue::fromVariant(v);
    if (jv.isString())  return jv.toString().toStdString();
    if (jv.isBool())    return jv.toBool();
    if (jv.isDouble())  return jv.toDouble();
    if (jv.isObject() || jv.isArray()) {
        QJsonDocument doc = jv.isObject() ? QJsonDocument(jv.toObject())
                                           : QJsonDocument(jv.toArray());
        try { return nlohmann::json::parse(doc.toJson(QJsonDocument::Compact).toStdString()); }
        catch (...) {}
    }
    return nullptr;
}

QVariant nlohmannToQVariant(const nlohmann::json& j)
{
    if (j.is_null())
        return QVariant();
    if (j.is_boolean())
        return QVariant(j.get<bool>());
    if (j.is_number_unsigned())
        return QVariant(static_cast<qulonglong>(j.get<uint64_t>()));
    if (j.is_number_integer())
        return QVariant(static_cast<qlonglong>(j.get<int64_t>()));
    if (j.is_number_float())
        return QVariant(j.get<double>());
    if (j.is_string())
        return QVariant(QString::fromStdString(j.get<std::string>()));
    if (isTaggedBytes(j))
        return QVariant(taggedJsonToByteArray(j));
    if (j.is_object()) {
        QVariantMap map;
        for (auto it = j.begin(); it != j.end(); ++it)
            map.insert(QString::fromStdString(it.key()), nlohmannToQVariant(it.value()));
        return QVariant(map);
    }
    if (j.is_array()) {
        QVariantList list;
        list.reserve(static_cast<int>(j.size()));
        for (const auto& elem : j)
            list.append(nlohmannToQVariant(elem));
        return QVariant(list);
    }
    return QVariant();
}

LogosResult jsonToLogosResult(const nlohmann::json& j)
{
    LogosResult r;
    r.success = false;
    if (!j.is_object()) return r;
    if (j.contains("success") && j["success"].is_boolean())
        r.success = j["success"].get<bool>();
    // Both payload fields recurse through the canonical decoder, so a `value`
    // carrying bytes / nested integers / containers comes back with the same
    // shape qvariantToNlohmann sent, and a null `error` stays an invalid
    // QVariant rather than becoming an empty QString.
    if (j.contains("value")) r.value = nlohmannToQVariant(j["value"]);
    if (j.contains("error")) r.error = nlohmannToQVariant(j["error"]);
    return r;
}

QVariantList nlohmannArgsToQVariantList(const nlohmann::json& args)
{
    QVariantList result;
    if (!args.is_array()) return result;
    for (const auto& arg : args) {
        if (arg.is_string())
            result.append(QString::fromStdString(arg.get<std::string>()));
        else if (arg.is_boolean())
            result.append(arg.get<bool>());
        else if (arg.is_number_unsigned())
            result.append(static_cast<qulonglong>(arg.get<uint64_t>()));
        else if (arg.is_number_integer())
            result.append(static_cast<qlonglong>(arg.get<int64_t>()));
        else if (arg.is_number_float())
            result.append(arg.get<double>());
        else if (arg.is_null())
            result.append(QVariant());
        else if (isTaggedBytes(arg))
            result.append(QVariant(taggedJsonToByteArray(arg)));
        else if (arg.is_object() || arg.is_array()) {
            result.append(nlohmannToQVariant(arg));
        } else {
            result.append(QVariant());
        }
    }
    return result;
}

QJsonArray methodsToJsonArray(const std::vector<LogosMethodMetadata>& methods)
{
    QJsonArray out;
    for (const auto& m : methods) {
        QJsonObject o;
        o["name"]        = QString::fromStdString(m.name);
        o["signature"]   = QString::fromStdString(m.signature);
        o["returnType"]  = QString::fromStdString(m.returnType);
        o["isInvokable"] = m.isInvokable;
        if (m.parameters.is_array()) {
            QJsonDocument paramDoc = QJsonDocument::fromJson(
                QByteArray::fromStdString(m.parameters.dump()));
            o["parameters"] = paramDoc.array();
        } else {
            o["parameters"] = QJsonArray();
        }
        out.append(o);
    }
    return out;
}

} // namespace logos
