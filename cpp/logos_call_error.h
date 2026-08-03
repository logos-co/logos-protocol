#ifndef LOGOS_CALL_ERROR_H
#define LOGOS_CALL_ERROR_H

// The canonical cross-module call error — the C++ face of the protocol's
// {code, message, origin} error JSON (see makeErrorJson / lp_invoke's
// out_error_json). Deliberately Qt-free: it crosses into Qt-free module
// code (generated typed wrappers expose it as an optional out-parameter,
// e.g. `calc.add(a, b, &err)`), so only std types appear here.

#include <string>

namespace logos {

// Error codes are lowercase snake_case strings, mirroring the C ABI's JSON
// contract rather than an enum so the set can grow (transport-level codes,
// provider dispatch errors) without an ABI break.
//
// Currently produced:
//   "object_unavailable" — the target module/object is not there: it could not
//                          be acquired, or the transport answered that it is
//                          not published. One code for one condition, whether
//                          it is detected at acquire time (QtRO, which resolves
//                          a replica up front) or at call time (the plain wire,
//                          whose requestObject hands back a handle for any name
//                          over an open connection and only learns the truth
//                          from the reply).
//   "timeout"            — the caller's deadline elapsed with no reply.
//   "transport_error"    — the connection failed or was torn down mid-call.
//   "call_failed"        — the peer could not dispatch the call at all (as
//                          distinct from a provider that ran and REJECTED it,
//                          which answers the "dispatch_failed" envelope as its
//                          result value — see logos-cpp-sdk#129).
//   "unauthorized"       — the provider rejected our token and the one
//                          permitted re-exchange also failed.
struct CallError {
    std::string code;     // empty = no error
    std::string message;
    std::string origin;   // module the error originated from / was detected for

    bool ok() const { return code.empty(); }
    void clear() { code.clear(); message.clear(); origin.clear(); }
};

// ---------------------------------------------------------------------------
// Canonical constructors.
//
// Every transport that can detect one of these produces it HERE rather than
// spelling the code out at the failure site, so a caller decoding a
// {code, message, origin} object gets the same vocabulary no matter which wire
// the call went over — and so lp_invoke and lp_invoke_async, which both render
// this struct with the same makeErrorJson(), stay indistinguishable.
// ---------------------------------------------------------------------------

inline CallError callErrorTimeout(const std::string& origin,
                                  const std::string& method, int timeoutMs)
{
    return {"timeout",
            "call to '" + origin + "." + method + "' timed out after "
                + std::to_string(timeoutMs) + "ms",
            origin};
}

inline CallError callErrorObjectUnavailable(const std::string& origin,
                                            const std::string& detail)
{
    return {"object_unavailable", detail, origin};
}

inline CallError callErrorTransport(const std::string& origin,
                                    const std::string& detail)
{
    return {"transport_error", detail, origin};
}

inline CallError callErrorCallFailed(const std::string& origin,
                                     const std::string& detail)
{
    return {"call_failed", detail, origin};
}

// Map a plain-wire ResultMessage failure (errCode + err) onto the vocabulary
// above. The wire's codes are the transport's own spelling; this is the single
// place they are translated, so a new wire code degrades to "call_failed"
// instead of silently becoming an empty CallError (which reads as SUCCESS).
inline CallError callErrorFromWire(const std::string& origin,
                                   const std::string& wireCode,
                                   const std::string& message)
{
    const std::string detail = message.empty() ? wireCode : message;
    if (wireCode == "MODULE_NOT_LOADED")
        return callErrorObjectUnavailable(origin, detail);
    if (wireCode == "TRANSPORT_CLOSED" || wireCode == "TRANSPORT_ERROR")
        return callErrorTransport(origin, detail);
    return callErrorCallFailed(origin, detail);
}

} // namespace logos

#endif // LOGOS_CALL_ERROR_H
