#ifndef LOGOS_JSON_CONVERT_H
#define LOGOS_JSON_CONVERT_H

#include <QVariant>
#include <QVariantList>
#include <QJsonArray>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "logos_types.h"   // LogosResult

struct LogosMethodMetadata {
    std::string name;
    std::string signature;
    std::string returnType;
    bool        isInvokable = true;
    nlohmann::json parameters = nlohmann::json::array();
};

namespace logos {

nlohmann::json qvariantToNlohmann(const QVariant& v);
QVariant nlohmannToQVariant(const nlohmann::json& j);

// The inverse of the LogosResult branch inside qvariantToNlohmann.
//
// Without it the pair is asymmetric: qvariantToNlohmann OWNS
// LogosResult -> {success,value,error}, but the return direction only had
// nlohmannToQVariant, which turns that object into a plain QVariantMap — and
// `qvariant_cast<LogosResult>` of a QVariantMap yields a default-constructed
// (success=false) result, silently. Any consumer that receives a `result` over
// the canonical JSON wire needs this, so it lives beside its inverse rather
// than being re-derived per code generator.
//
// `error` is decoded through nlohmannToQVariant so a JSON null stays an INVALID
// QVariant — matching what the Qt transport delivers for "no error" and what
// the encoder above emits. A std::string-typed intermediate cannot represent
// that state, which is why this is the JSON-level inverse and not a
// StdLogosResult hop.
LogosResult jsonToLogosResult(const nlohmann::json& j);

QVariantList nlohmannArgsToQVariantList(const nlohmann::json& args);
QJsonArray methodsToJsonArray(const std::vector<LogosMethodMetadata>& methods);

} // namespace logos

#endif // LOGOS_JSON_CONVERT_H
