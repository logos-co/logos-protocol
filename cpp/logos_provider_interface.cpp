#include "logos_provider_interface.h"
#include "logos_json_convert.h"
#include <QDebug>

// ---------------------------------------------------------------------------
// LogosProviderObject — universal virtual defaults
// ---------------------------------------------------------------------------

nlohmann::json LogosProviderObject::callMethodStd(const std::string& /*methodName*/,
                                                   const nlohmann::json& /*args*/)
{
    return nullptr;
}

std::vector<LogosMethodMetadata> LogosProviderObject::getMethodsStd()
{
    return {};
}

void LogosProviderObject::setEventListenerStd(UniversalEventCallback /*callback*/)
{
}

// ---------------------------------------------------------------------------
// LogosProviderObject — bridge helpers (Qt-free providers delegate here)
// ---------------------------------------------------------------------------

QVariant LogosProviderObject::callMethodStdBridge(const QString& methodName, const QVariantList& args)
{
    nlohmann::json jArgs = nlohmann::json::array();
    for (const QVariant& a : args)
        jArgs.push_back(logos::qvariantToNlohmann(a));

    nlohmann::json result = callMethodStd(methodName.toStdString(), jArgs);
    return logos::nlohmannToQVariant(result);
}

QJsonArray LogosProviderObject::getMethodsStdBridge()
{
    return logos::methodsToJsonArray(getMethodsStd());
}

void LogosProviderObject::setEventListenerStdBridge(EventCallback callback)
{
    setEventListenerStd([callback](const std::string& eventName, const std::string& data) {
        if (!callback) return;
        QVariantList qData;
        // Parse with nlohmann and convert with the SAME helper the method path
        // uses (callMethodStdBridge above), so a value does not depend on
        // whether it left the module as a return or as an event.
        //
        // This used QJsonDocument::fromJson + QJsonValue::toVariant. Qt 6 backs
        // QJsonValue with QCborValue, so integers up to int64 survived — but a
        // uint64 above int64max has no integral representation there and fell
        // back to double: 18446744073709551615 arrived as 1.8446744073709552e+19,
        // exact on the method path and rounded one hop later. The Qt parser also
        // has no notion of the canonical {"_bytes": ...} tag, so byte payloads
        // arrived as a QVariantMap and only survived because that map round-trips
        // to a consumer that decodes the tag itself.
        //
        // parse(..., nullptr, false) is the non-throwing form: malformed input
        // yields a discarded value and takes the raw-string fallback below,
        // which is the behaviour QJsonDocument gave for unparseable data.
        const nlohmann::json payload = nlohmann::json::parse(data, nullptr, false);
        if (!payload.is_discarded() && payload.is_array()) {
            qData = logos::nlohmannArgsToQVariantList(payload);
        } else {
            qData.append(QString::fromStdString(data));
        }
        callback(QString::fromStdString(eventName), qData);
    });
}
