#ifndef LOGOS_CODEC_H
#define LOGOS_CODEC_H

// ---------------------------------------------------------------------------
// logos_codec.h — THE canonical JSON representation of LIDL values.
//
// One implementation, here. Before this header the same encoding lived in six
// places (the Qt conversion, the plain wire's json_mapping, the lp helper in
// logos-cpp-sdk, a copy emitted into every generated cdylib module, the Rust
// SDK and the Python client) and they disagreed on which inputs they accepted.
// Anything that needs to move a LIDL value in or out of JSON uses these.
//
// LEAF TYPES
//   tstr     std::string                      -> json string
//   bstr     std::vector<uint8_t>             -> {"_bytes": "<base64url, unpadded>"}
//   int      any signed integral              -> json integer
//   uint     any unsigned integral            -> json integer
//   float64  float / double                   -> json number
//   bool     bool                             -> json bool
//   any      nlohmann::json (LogosMap/List)   -> verbatim; recursion STOPS here
//
// COMPOSITION — generic, no whitelist
//   [T]        std::vector<T>                              for any supported T
//   {tstr: T}  std::map / std::unordered_map<std::string,T> for any supported T
//   ...at any depth. Bytes stay tagged wherever they occur, so [bstr], [[bstr]],
//   {tstr: [bstr]} and bytes nested inside a map all encode canonically without
//   anything having to enumerate the combination.
//
// WHY BYTES ARE TAGGED: JSON has no bytes primitive and a JSON string must be
// valid UTF-8, so raw binary (embedded NULs, anything >= 0x80) cannot ride a
// plain string. The single-key object is the wire form every language agrees on.
// Note the inherent ambiguity: a genuine one-key map named "_bytes" whose value
// is a string is indistinguishable from bytes. Don't name a map key "_bytes".
//
// DECODE STRICTNESS: shape mismatches throw CodecError, which callers surface as
// a structured error (the generated dispatch turns it into
// {"code":"dispatch_failed",...}) rather than silently substituting a default —
// silent defaults are how a mangled value reaches business logic. `bstr` alone
// keeps a documented lenient form (see bytesFromJsonLenient) because the Qt
// consumer path and the logoscore CLI's argument auto-typing both produce plain
// strings and number arrays for byte parameters.
// ---------------------------------------------------------------------------

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace logos {

// Thrown by fromJson<T>() when the value does not match the expected LIDL type.
// `what()` carries the path to the offending element ("[2].payload") so a
// failure inside a nested container is actionable.
class CodecError : public std::runtime_error {
public:
    explicit CodecError(const std::string& message)
        : std::runtime_error(message) {}
};

namespace detail {

inline std::string joinPath(const std::string& path, const std::string& step)
{
    return path.empty() ? step : path + step;
}

[[noreturn]] inline void typeError(const std::string& path, const char* expected,
                                   const nlohmann::json& got)
{
    throw CodecError("expected " + std::string(expected)
                     + (path.empty() ? std::string(" at value") : " at " + path)
                     + ", got " + std::string(got.type_name()));
}

} // namespace detail

// ── base64url, unpadded ────────────────────────────────────────────────────

inline std::string b64UrlEncode(const std::vector<uint8_t>& bytes)
{
    static const char* alpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 3 <= bytes.size()) {
        const uint32_t n = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8)
                         | uint32_t(bytes[i + 2]);
        out += alpha[(n >> 18) & 0x3f];
        out += alpha[(n >> 12) & 0x3f];
        out += alpha[(n >> 6) & 0x3f];
        out += alpha[n & 0x3f];
        i += 3;
    }
    if (i < bytes.size()) {
        uint32_t n = uint32_t(bytes[i]) << 16;
        if (i + 1 < bytes.size()) n |= uint32_t(bytes[i + 1]) << 8;
        out += alpha[(n >> 18) & 0x3f];
        out += alpha[(n >> 12) & 0x3f];
        if (i + 1 < bytes.size()) out += alpha[(n >> 6) & 0x3f];
    }
    return out;
}

// Decodes unpadded base64url. Deliberately tolerant: '=' padding and stray
// characters (whitespace from a hand-written spec, a padded encoder on the
// other side) are skipped rather than aborting the whole value — one of the
// four behaviours the old copies disagreed on.
inline std::vector<uint8_t> b64UrlDecode(const std::string& in)
{
    auto idx = [](char ch) -> int {
        if (ch >= 'A' && ch <= 'Z') return ch - 'A';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9') return ch - '0' + 52;
        if (ch == '-') return 62;
        if (ch == '_') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve(in.size() * 3 / 4);
    uint32_t buf = 0;
    int bits = 0;
    for (char ch : in) {
        const int v = idx(ch);
        if (v < 0) continue;
        buf = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xff));
        }
    }
    return out;
}

// Strict variant: returns false on any character outside the alphabet (padding
// aside) or an impossible length, leaving `out` empty. The plain wire validates
// its frames with this — a corrupt frame must be rejected, not silently decoded
// to fewer bytes. Consumer-facing decodes use the tolerant one above.
inline bool b64UrlDecodeChecked(const std::string& in, std::vector<uint8_t>& out)
{
    auto idx = [](char ch) -> int {
        if (ch >= 'A' && ch <= 'Z') return ch - 'A';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9') return ch - '0' + 52;
        if (ch == '-') return 62;
        if (ch == '_') return 63;
        return -1;
    };
    out.clear();
    std::string body = in;
    while (!body.empty() && body.back() == '=') body.pop_back();
    if (body.size() % 4 == 1) return false;

    uint32_t buf = 0;
    int bits = 0;
    for (char ch : body) {
        const int v = idx(ch);
        if (v < 0) {
            out.clear();
            return false;
        }
        buf = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xff));
        }
    }
    return true;
}

// ── tagged bytes ───────────────────────────────────────────────────────────

inline nlohmann::json bytesToJson(const std::vector<uint8_t>& bytes)
{
    return nlohmann::json{{"_bytes", b64UrlEncode(bytes)}};
}

// The canonical form: EXACTLY one key, "_bytes", holding a string. The size
// check matters — without it a map that merely contains a "_bytes" entry is
// read as bytes, which is how the lp helper and the plain wire disagreed.
inline bool isTaggedBytes(const nlohmann::json& j)
{
    return j.is_object() && j.size() == 1 && j.contains("_bytes")
        && j["_bytes"].is_string();
}

inline std::vector<uint8_t> bytesFromJson(const nlohmann::json& j)
{
    if (!isTaggedBytes(j))
        detail::typeError("", "tagged bytes {\"_bytes\": \"...\"}", j);
    return b64UrlDecode(j["_bytes"].get<std::string>());
}

// Canonical form, plus the shapes other layers legitimately produce for a byte
// parameter:
//   string        -> its raw bytes (a Qt consumer passing a QString, a CLI arg)
//   number        -> its decimal text as bytes (QVariant(int)->QByteArray parity)
//   array of ints -> those byte values
// Providers decode arguments with this; everything else uses the canonical form.
inline std::vector<uint8_t> bytesFromJsonLenient(const nlohmann::json& j,
                                                const std::string& path = "")
{
    if (isTaggedBytes(j))
        return b64UrlDecode(j["_bytes"].get<std::string>());
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        return std::vector<uint8_t>(s.begin(), s.end());
    }
    if (j.is_number()) {
        const std::string s = j.dump();
        return std::vector<uint8_t>(s.begin(), s.end());
    }
    if (j.is_array()) {
        std::vector<uint8_t> out;
        out.reserve(j.size());
        for (const auto& e : j)
            if (e.is_number_integer() || e.is_number_unsigned())
                out.push_back(static_cast<uint8_t>(e.get<int64_t>() & 0xff));
        return out;
    }
    detail::typeError(path, "bytes", j);
}

// ── the generic value codec ────────────────────────────────────────────────
//
// Codec<T> is a trait rather than a function template because C++ has no
// partial specialisation of function templates, and the composition rule
// (vector<T>, map<string,T>) is inherently partial. An unsupported T leaves
// Codec<T> incomplete, so the failure is a compile error at the call site
// naming the type — never a silent fallback.

namespace detail {

template <class T, class Enable = void> struct Codec;

// bstr — a FULL specialisation, so it wins over the generic vector<T> below.
template <> struct Codec<std::vector<uint8_t>, void> {
    static nlohmann::json to(const std::vector<uint8_t>& v) { return bytesToJson(v); }
    static std::vector<uint8_t> from(const nlohmann::json& j, const std::string& path)
    {
        // The path is threaded through so a bad bstr nested in a container
        // reports "[0].payload" rather than a bare "value" — the generated
        // cdylib codec used to carry that detail in its own copy, and this is
        // what lets that copy be deleted without losing the diagnostic.
        return bytesFromJsonLenient(j, path);
    }
};

template <> struct Codec<bool, void> {
    static nlohmann::json to(bool v) { return v; }
    static bool from(const nlohmann::json& j, const std::string& path)
    {
        if (!j.is_boolean()) typeError(path, "bool", j);
        return j.get<bool>();
    }
};

template <> struct Codec<std::string, void> {
    static nlohmann::json to(const std::string& v) { return v; }
    static std::string from(const nlohmann::json& j, const std::string& path)
    {
        if (!j.is_string()) typeError(path, "string", j);
        return j.get<std::string>();
    }
};

// `any` stops the recursion: the value passes through untouched, so a LogosMap
// or LogosList keeps whatever the peer sent (including tagged bytes nested
// inside it, which the author decodes with bytesFromJson).
template <> struct Codec<nlohmann::json, void> {
    static nlohmann::json to(const nlohmann::json& v) { return v; }
    static nlohmann::json from(const nlohmann::json& j, const std::string&) { return j; }
};

// int / uint — every C++ integral spelling, bool excluded (specialised above).
//
// Signedness is part of the type, so it is checked. `is_number_integer()` is
// true for a negative, and `.get<uint64_t>()` on -1 wraps to 18446744073709551615
// with no exception — a silent sign flip on a nominal value. Range is checked for
// the same reason: `.get<T>()` on a narrower T truncates rather than throwing.
//
// This rejects rather than coerces, matching the rest of the codec: a value the
// declared type cannot represent must not reach business logic wearing a
// different one.
//
// A WHOLE-VALUED float is not such a value. JSON does not distinguish 3 from
// 3.0, and every encoder that sees a whole double may emit either — which is
// exactly the reasoning Codec<double> already gives for accepting an integral
// number. The two directions have to agree, so 3.0 decodes as 3 while 3.7 is
// still refused. A CLI that types its arguments by parsing (logoscore's does)
// produces 3.0 for `3.0`, so refusing it breaks callers over a spelling.
template <class T>
struct Codec<T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>> {
    static nlohmann::json to(T v) { return v; }
    static T from(const nlohmann::json& j, const std::string& path)
    {
        if (j.is_number_float()) {
            const double d = j.get<double>();
            double intPart = 0.0;
            if (std::modf(d, &intPart) != 0.0)
                typeError(path, "integer", j);
            // Strict bounds: double(int64max) rounds UP to 2^63, so `<=` would
            // admit a value the cast cannot represent (undefined behaviour).
            if constexpr (std::is_unsigned_v<T>) {
                if (d < 0.0 || d >= 18446744073709551616.0)   // 2^64
                    typeError(path, "unsigned integer in range", j);
            } else {
                if (d < -9223372036854775808.0 || d >= 9223372036854775808.0)  // ±2^63
                    typeError(path, "signed integer in range", j);
            }
            const auto whole = static_cast<long double>(d);
            if (whole < static_cast<long double>(std::numeric_limits<T>::min()) ||
                whole > static_cast<long double>(std::numeric_limits<T>::max()))
                typeError(path, "integer in range", j);
            return static_cast<T>(d);
        }
        if (!j.is_number_integer() && !j.is_number_unsigned())
            typeError(path, "integer", j);

        if constexpr (std::is_unsigned_v<T>) {
            // A negative literal parses as number_integer, never number_unsigned.
            if (j.is_number_integer() && !j.is_number_unsigned() && j.get<int64_t>() < 0)
                typeError(path, "unsigned integer", j);
            const uint64_t u = j.get<uint64_t>();
            if (u > static_cast<uint64_t>(std::numeric_limits<T>::max()))
                typeError(path, "unsigned integer in range", j);
            return static_cast<T>(u);
        } else {
            if (j.is_number_unsigned()) {
                const uint64_t u = j.get<uint64_t>();
                if (u > static_cast<uint64_t>(std::numeric_limits<T>::max()))
                    typeError(path, "signed integer in range", j);
                return static_cast<T>(u);
            }
            const int64_t i = j.get<int64_t>();
            if (i < static_cast<int64_t>(std::numeric_limits<T>::min()) ||
                i > static_cast<int64_t>(std::numeric_limits<T>::max()))
                typeError(path, "signed integer in range", j);
            return static_cast<T>(i);
        }
    }
};

// float64 — an integral JSON number is accepted (2 and 2.0 are the same value
// to JSON, and every encoder that sees a whole double may emit either).
template <class T>
struct Codec<T, std::enable_if_t<std::is_floating_point_v<T>>> {
    static nlohmann::json to(T v) { return v; }
    static T from(const nlohmann::json& j, const std::string& path)
    {
        if (!j.is_number()) typeError(path, "number", j);
        return j.get<T>();
    }
};

// [T] — generic over the element type, so nesting composes.
template <class T> struct Codec<std::vector<T>, void> {
    static nlohmann::json to(const std::vector<T>& v)
    {
        nlohmann::json out = nlohmann::json::array();
        for (const T& e : v) out.push_back(Codec<T>::to(e));
        return out;
    }
    static std::vector<T> from(const nlohmann::json& j, const std::string& path)
    {
        if (!j.is_array()) typeError(path, "array", j);
        std::vector<T> out;
        out.reserve(j.size());
        for (size_t i = 0; i < j.size(); ++i)
            out.push_back(Codec<T>::from(j[i], joinPath(path, "[" + std::to_string(i) + "]")));
        return out;
    }
};

// {tstr: T} — both standard associative containers, same rule.
template <class M> struct MapCodec {
    using T = typename M::mapped_type;
    static nlohmann::json to(const M& v)
    {
        nlohmann::json out = nlohmann::json::object();
        for (const auto& kv : v) out[kv.first] = Codec<T>::to(kv.second);
        return out;
    }
    static M from(const nlohmann::json& j, const std::string& path)
    {
        if (!j.is_object()) typeError(path, "object", j);
        M out;
        for (auto it = j.begin(); it != j.end(); ++it)
            out.emplace(it.key(), Codec<T>::from(it.value(), joinPath(path, "." + it.key())));
        return out;
    }
};

template <class T> struct Codec<std::map<std::string, T>, void>
    : MapCodec<std::map<std::string, T>> {};
template <class T> struct Codec<std::unordered_map<std::string, T>, void>
    : MapCodec<std::unordered_map<std::string, T>> {};

} // namespace detail

// Encode a LIDL value of static type T into its canonical JSON form.
template <class T>
nlohmann::json toJson(const T& value)
{
    return detail::Codec<T>::to(value);
}

// Decode canonical JSON into a LIDL value of static type T.
// Throws CodecError on a shape mismatch, naming the path.
template <class T>
T fromJson(const nlohmann::json& j)
{
    return detail::Codec<T>::from(j, std::string());
}

// Same, with a path prefix so a failure inside a nested value reports where it
// came from ("arg2[0].payload" rather than "[0].payload").
template <class T>
T fromJson(const nlohmann::json& j, const std::string& path)
{
    return detail::Codec<T>::from(j, path);
}

// A JSON value that decodes itself into whatever the callee's parameter type is.
//
// Generated dispatch code has the author's signature available only as an
// overload-resolution target, not as text it can name — and naming it is a trap:
// spelling `[uint]` as std::vector<uint64_t> (the LIDL mapping) does not bind to
// an author's `const std::vector<uint32_t>&`, because distinct vector
// instantiations do not convert. Passing this proxy instead makes the compiler
// instantiate the conversion with the EXACT parameter type, so the author's own
// spelling is what gets decoded — any integer width, any supported nesting.
//
// An unsupported target type leaves Codec<T> incomplete, so the failure is a
// compile error naming the type at the call site, never a silent fallback.
class JsonArg {
public:
    JsonArg(const nlohmann::json& value, std::string path)
        : m_value(value), m_path(std::move(path)) {}

    template <class T>
    operator T() const   // NOLINT(google-explicit-constructor) — that is the point
    {
        return detail::Codec<std::decay_t<T>>::from(m_value, m_path);
    }

private:
    const nlohmann::json& m_value;
    std::string m_path;
};

} // namespace logos

#endif // LOGOS_CODEC_H
