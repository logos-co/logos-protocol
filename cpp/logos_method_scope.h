#ifndef LOGOS_METHOD_SCOPE_H
#define LOGOS_METHOD_SCOPE_H

// The scope a pushed token carries (informScopedModuleToken): exactly {"methods":[...]},
// non-empty, unique and capped. Anything else is refused, so a later key fails closed.

#include <nlohmann/json.hpp>

#include <cstddef>
#include <set>
#include <string>
#include <utility>

namespace logos {

constexpr std::size_t kMaxMethodScopeBytes = 64 * 1024;
constexpr std::size_t kMaxMethodScopeEntries = 1024;
constexpr std::size_t kMaxMethodNameBytes = 256;

inline bool parseMethodScope(const std::string& text, std::set<std::string>* methods = nullptr)
{
    if (text.empty() || text.size() > kMaxMethodScopeBytes) return false;
    const nlohmann::json scope = nlohmann::json::parse(text, nullptr, false);
    if (!scope.is_object() || scope.size() != 1) return false;
    const auto list = scope.find("methods");
    if (list == scope.end() || !list->is_array() || list->empty()
        || list->size() > kMaxMethodScopeEntries)
        return false;
    std::set<std::string> parsed;
    for (const auto& entry : *list) {
        if (!entry.is_string()) return false;
        std::string name = entry.get<std::string>();
        if (name.empty() || name.size() > kMaxMethodNameBytes || name == "*") return false;
        for (const unsigned char c : name)
            if (c < 0x20 || c == 0x7f) return false;
        if (!parsed.insert(std::move(name)).second) return false;
    }
    if (methods) *methods = std::move(parsed);
    return true;
}

// Answered whatever a caller's scope: identity and introspection, with no arguments.
inline bool isMethodScopeExempt(const std::string& method, bool noArguments)
{
    return noArguments
        && (method == "name" || method == "version" || method == "lidl"
            || method == "getPluginMethods" || method == "getPluginEvents"
            || method == "getPluginInterface");
}

} // namespace logos

#endif
