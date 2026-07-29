#ifndef LOGOS_PLAIN_RPC_VALUE_H
#define LOGOS_PLAIN_RPC_VALUE_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace logos::plain {

// -----------------------------------------------------------------------------
// RpcValue — plain C++ variant carried by the wire RPC layer.
//
// Covers the shapes we actually need (null / bool / int / double / string /
// bytes / list / map). No Qt types. The JSON/CBOR codec converts to/from
// `nlohmann::json`; Qt-side callers convert to/from `QVariant` at the Qt
// boundary (see plain_logos_object.cpp, plain_transport_host.cpp).
//
// Uses recursive std::variant via wrapper structs so list/map can hold
// RpcValue children without forward-declaration headaches.
// -----------------------------------------------------------------------------

struct RpcValue;

struct RpcList {
    std::vector<RpcValue> items;
    bool operator==(const RpcList& other) const { return items == other.items; }
    bool operator!=(const RpcList& other) const { return !(*this == other); }
};

// std::map<string, RpcValue> would require RpcValue to be complete at this
// point, which is impossible (RpcValue contains RpcMap as a variant alt).
// Use a vector of pairs instead — also gives us deterministic encoding
// order for free, which matters when we move to CBOR.
//
// Method bodies that dereference RpcValue are defined out-of-line below,
// once RpcValue is complete.
struct RpcMap {
    std::vector<std::pair<std::string, RpcValue>> entries;

    void emplace(std::string key, RpcValue val);
    const RpcValue* find(const std::string& key) const;
    const RpcValue& at(const std::string& key) const;

    bool operator==(const RpcMap& other) const;
    bool operator!=(const RpcMap& other) const { return !(*this == other); }
};

struct RpcBytes {
    std::vector<uint8_t> data;
    bool operator==(const RpcBytes& other) const { return data == other.data; }
    bool operator!=(const RpcBytes& other) const { return !(*this == other); }
};

struct RpcValue {
    using Variant = std::variant<
        std::monostate,   // null
        bool,
        int64_t,
        uint64_t,         // ONLY for values above int64max — see makeInteger()
        double,
        std::string,
        RpcBytes,
        RpcList,
        RpcMap
    >;

    Variant value;

    RpcValue() = default;
    RpcValue(std::monostate) : value(std::monostate{}) {}
    RpcValue(bool b) : value(b) {}
    RpcValue(int i) : value(static_cast<int64_t>(i)) {}
    RpcValue(int64_t i) : value(i) {}
    RpcValue(uint64_t u) : value(u) {}
    RpcValue(double d) : value(d) {}
    RpcValue(const char* s) : value(std::string(s)) {}
    RpcValue(std::string s) : value(std::move(s)) {}
    RpcValue(RpcBytes b) : value(std::move(b)) {}
    RpcValue(RpcList l) : value(std::move(l)) {}
    RpcValue(RpcMap m) : value(std::move(m)) {}

    // Canonical way to build an integer from an unsigned source.
    //
    // The uint64_t alternative exists for exactly one reason: to carry values
    // int64_t cannot. It is NOT used for every non-negative integer, and that is
    // deliberate — std::variant equality compares the alternative index first,
    // so representing 42 as uint64_t would make RpcValue{42} != decode("42") and
    // silently change the metatype of every non-negative integer already
    // crossing this wire, to fix nothing. Values that fit int64_t keep their
    // existing representation; only the band above int64max is new.
    static RpcValue makeInteger(uint64_t u) {
        if (u <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            return RpcValue{static_cast<int64_t>(u)};
        return RpcValue{u};
    }

    bool isNull()   const { return std::holds_alternative<std::monostate>(value); }
    bool isBool()   const { return std::holds_alternative<bool>(value); }
    bool isInt()    const { return std::holds_alternative<int64_t>(value); }
    bool isUInt()   const { return std::holds_alternative<uint64_t>(value); }
    bool isDouble() const { return std::holds_alternative<double>(value); }
    bool isString() const { return std::holds_alternative<std::string>(value); }
    bool isBytes()  const { return std::holds_alternative<RpcBytes>(value); }
    bool isList()   const { return std::holds_alternative<RpcList>(value); }
    bool isMap()    const { return std::holds_alternative<RpcMap>(value); }

    // True for either integer alternative — use this when you care about "is a
    // whole number" rather than about signedness, so a uint64 above int64max is
    // not mistaken for a non-integer.
    bool isIntegral() const { return isInt() || isUInt(); }

    bool                asBool()   const { return std::get<bool>(value); }
    int64_t             asInt()    const { return std::get<int64_t>(value); }
    uint64_t            asUInt()   const { return std::get<uint64_t>(value); }
    double              asDouble() const { return std::get<double>(value); }
    const std::string&  asString() const { return std::get<std::string>(value); }
    const RpcBytes&     asBytes()  const { return std::get<RpcBytes>(value); }
    const RpcList&      asList()   const { return std::get<RpcList>(value); }
    const RpcMap&       asMap()    const { return std::get<RpcMap>(value); }

    bool operator==(const RpcValue& other) const { return value == other.value; }
    bool operator!=(const RpcValue& other) const { return !(*this == other); }
};

// ── RpcMap out-of-line methods (need complete RpcValue) ────────────────────

inline void RpcMap::emplace(std::string key, RpcValue val) {
    entries.emplace_back(std::move(key), std::move(val));
}

inline const RpcValue* RpcMap::find(const std::string& key) const {
    for (const auto& kv : entries) if (kv.first == key) return &kv.second;
    return nullptr;
}

inline const RpcValue& RpcMap::at(const std::string& key) const {
    const RpcValue* v = find(key);
    if (!v) throw std::out_of_range("RpcMap::at: key not found: " + key);
    return *v;
}

inline bool RpcMap::operator==(const RpcMap& other) const {
    return entries == other.entries;
}

} // namespace logos::plain

#endif // LOGOS_PLAIN_RPC_VALUE_H
