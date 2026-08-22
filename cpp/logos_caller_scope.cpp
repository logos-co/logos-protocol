#include "logos_caller_scope.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

namespace logos {

namespace {

// The per-thread slot. One std::string, holding the innermost open scope's
// document.
//
// A plain thread_local rather than a std::vector stack: CallerScope already
// keeps the enclosing document in its own frame, so the "stack" is the C++ call
// stack. That is both cheaper and harder to desynchronize — there is no
// container whose depth could disagree with the number of live scopes.
std::string& slot()
{
    thread_local std::string current;
    return current;
}

} // namespace

std::string callerUnknownJson()
{
    return R"({"kind":"unknown"})";
}

std::string callerHostAnchorJson()
{
    return R"({"kind":"host"})";
}

std::string callerModuleJson(const std::string& name)
{
    // A name with no producer is not a module arm. Resolving to Unknown here
    // rather than emitting {"kind":"module","name":""} keeps rule 4 of the wire
    // shape (a known arm missing a required field is Unknown) true at the
    // PRODUCER as well as at every reader.
    if (name.empty()) return callerUnknownJson();

    nlohmann::json j;
    j["kind"] = "module";
    j["name"] = name;
    // `replace` rather than the default throwing handler. A store key is a
    // module name and is UTF-8 in every path that exists today, but this runs
    // on the authorization path of every inbound call: a throw here would turn
    // a naming problem into a failed dispatch, which is a strictly worse
    // outcome than a name with a replacement character in it.
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string currentInboundCallerJson()
{
    return slot();
}

CallerScope::CallerScope(std::string callerJson)
    : m_previous(std::move(slot()))
{
    slot() = std::move(callerJson);
}

CallerScope::~CallerScope()
{
    slot() = std::move(m_previous);
}

} // namespace logos
