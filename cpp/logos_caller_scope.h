#ifndef LOGOS_CALLER_SCOPE_H
#define LOGOS_CALLER_SCOPE_H

#include <string>

/* ===========================================================================
 * logos_caller_scope.h — WHO is calling the dispatch running on this thread.
 *
 * The host half of the ambient caller accessor. ModuleProxy has just decided
 * that an inbound call is authorized; deciding that is exactly the moment it
 * also learns WHICH issued token matched, and therefore who is on the other
 * end. This file is where that answer is parked for the duration of the
 * dispatch, and nothing more.
 *
 * NOT the accessor a module author calls. That is logos::currentCaller() in
 * the C++ SDK (and logos_rust_sdk::current_caller() in Rust), which reads a
 * thread-local in the MODULE's image after the generated glue pushed this
 * document across the module-impl C ABI. The two are deliberately separate
 * stores in separate images — see WHY THIS IS NOT ONE THREAD-LOCAL below.
 *
 * Qt-free on purpose: std::string in, std::string out. The eventual no-Qt host
 * has the same job to do at the same seam, and the JSON document is the whole
 * interface between the two halves.
 *
 * ── the wire shape ──────────────────────────────────────────────────────────
 *
 * NORMATIVE HOME IS logos_module_impl.h, above logos_module_set_call_caller —
 * the one file every language backend already reads. What follows is a pointer,
 * not a second specification. In summary: a JSON object with a mandatory
 * "kind", one of "unknown" | "host" | "module" | "derived" | "operator".
 *
 * ── WHY THIS IS NOT ONE THREAD-LOCAL ────────────────────────────────────────
 *
 * The obvious implementation — open a thread_local here and have the module's
 * handler read it — does not work, and fails silently rather than loudly on the
 * two platforms where it fails. ModuleProxy::callRemoteMethod runs in the HOST
 * image; the handler runs in the module cdylib, which links its own copy of
 * this library. Measured on shipped binaries in this workspace (logos_host and
 * modules_state_plugin.dylib): both DEFINE ModuleProxy::callRemoteMethod and
 * TokenManager::instance(), each has its own image-local function-local static,
 * and neither carries a single undefined reference to the other's. The Mach-O
 * headers say NOUNDEFS + TWOLEVEL, i.e. every intra-image call was bound to
 * that image's own copy at static-link time. PE has no interposition at all
 * (see logos_shared_api.h). Only ELF's flat namespace would collapse the two,
 * which is precisely the platform asymmetry that let two ABI breaks through.
 *
 * So the value has to be PUSHED across the module-impl C ABI, and the pull that
 * feeds that push has to be a dynamic, by-name call into the host image. That
 * is what LogosAPI::currentCallerJson() (logos-plugin-qt) exists for: it is
 * reached through QMetaObject::invokeMethod, whose metaObject() is virtual and
 * therefore selects the vtable the HOST's constructor installed. The same
 * cross-image-safe channel initLogos, aboutToUnload and the authToken /
 * hostServices / modulePath properties already use.
 *
 * ── WHERE A CALLER IS AVAILABLE, AND WHERE IT IS UNKNOWN ────────────────────
 *
 * ModuleProxy::callRemoteMethod is the only entry into a module handler, so a
 * caller exists exactly where that function is reached THROUGH THE META-OBJECT
 * — never where it is reached by a direct C++ call, because a direct call binds
 * to the calling image's own copy and opens the scope in the wrong image's
 * thread-local.
 *
 *   qt_remote (QtRO local socket)  YES. The replica invokes "callRemoteMethod"
 *                                  by name (remote_transport.cpp:155).
 *   plain tcp / tcp_ssl            YES for the identity. An operator-issued
 *                                  named token authorizes through
 *                                  TokenValidator, which returns bool only, so
 *                                  those land on Unknown until it is widened.
 *                                  This is the main plain-transport case.
 *   qt_local (in-process)          UNKNOWN. local_transport.cpp:94 and :111
 *                                  call m_proxy->callRemoteMethod DIRECTLY.
 *                                  Routing it through the meta-object would fix
 *                                  it and also close a pre-existing cross-image
 *                                  hazard; LogosMode::Local has no in-tree
 *                                  producer today, so it is deliberately left.
 *   ui-host QtRO bridge            UNKNOWN. The UI plugin's backend object is
 *                                  published directly, with no ModuleProxy and
 *                                  no token on the channel — nothing to
 *                                  resolve. A property of that bridge, not a
 *                                  gap here.
 *   legacy Q_INVOKABLE Qt plugins  UNKNOWN. The handler body runs in the plugin
 *                                  image with no generated glue and therefore
 *                                  no push. Live, not hypothetical:
 *                                  capability_module is one.
 *   mock transport                 UNKNOWN. MockTransport::callMethod goes to
 *                                  MockStore and bypasses ModuleProxy
 *                                  entirely, so NO MOCK-MODE TEST CAN EXERCISE
 *                                  THIS. Tests must be real QtRO or plain.
 *   background threads, timers,    UNKNOWN, correctly — no dispatch is in
 *   onContextReady, event emission progress.
 *   getPlugin{Methods,Events,      n/a. Answered by the proxy before any
 *   Interface}, the name()/        handler is reached.
 *   version() fallback
 * =========================================================================== */

namespace logos {

/* ---------------------------------------------------------------------------
 * The documents. Constructed here rather than spelled at each call site so
 * there is exactly one producer of each arm, and so the escaping of a caller
 * name is done once by a real JSON writer.
 * ------------------------------------------------------------------------- */

/* {"kind":"unknown"} — the FAIL-CLOSED value, and an in-band one.
 *
 * Every "we could not name the caller" outcome resolves to this document. It is
 * never spelled by absence: a reader that treats an empty string as unknown
 * would treat a truncated or dropped document the same way as a deliberate
 * refusal to name, and those are not the same fact. */
std::string callerUnknownJson();

/* {"kind":"host"} — the call presented one of the host bootstrap anchors.
 *
 * CARRIES NO NAME, and must not gain one. "core" and "capability_module" hold
 * the same token VALUE under two keys by construction (TokenManager::
 * adoptCredential writes ONE credential under every bootstrapKeys() key), so a
 * name on this arm would be a coin flip presented as a fact.
 *
 * AND IT IS THE HOST'S OWN CREDENTIAL, not any identity's. An isolated identity
 * carries its own credential under those keys, which the callee's proxy finds
 * in its caller-keyed inbound record and answers with the module arm below.
 * Until logos-protocol 0.7 a private store was born holding a COPY of the
 * host's, so an in-process view resolved HERE — the host arm — which is the
 * elevation adoptCredentialFor exists to close. */
std::string callerHostAnchorJson();

/* {"kind":"module","name":"<name>"} — a named module, token-bound.
 *
 * "token-bound" is the strongest honest word and is chosen over "verified" or
 * "authenticated" deliberately: the name comes from the key under which THIS
 * module recorded the token the caller just presented. capability_module, which
 * mints those tokens, checks only that the asserted name EXISTS as a key.
 *
 * No `instance` field is produced today; see logos_module_impl.h for why the
 * field is nonetheless part of the shape from day one. */
std::string callerModuleJson(const std::string& name);

/* ---------------------------------------------------------------------------
 * The per-thread stack.
 * ------------------------------------------------------------------------- */

/**
 * @brief The caller of the dispatch currently running ON THIS THREAD.
 *
 * Returns the document the innermost open CallerScope was constructed with, or
 * an EMPTY string when no dispatch is in flight on this thread.
 *
 * Empty is a third state, distinct from {"kind":"unknown"}: "no dispatch here"
 * rather than "a dispatch whose caller could not be named". Callers that must
 * hand a value onward convert empty to callerUnknownJson() at that boundary —
 * LogosAPI::currentCallerJson() does exactly that — so the distinction never
 * escapes this process's host image.
 *
 * A background thread, a timer callback and onContextReady all read empty here,
 * correctly, because there is no caller.
 */
std::string currentInboundCallerJson();

/**
 * @brief Names the caller for the duration of one dispatch, on one thread.
 *
 * SAVES AND RESTORES the previous value rather than clearing on exit, and that
 * is load-bearing rather than defensive. A handler that makes an outbound call
 * spins a nested QEventLoop, and QtRO can deliver a SECOND inbound call on the
 * same thread inside it. A scope that cleared on exit would leave the outer
 * dispatch reading "no caller" from the point the inner one returned — the
 * outer handler's caller would evaporate mid-frame, with nothing to see.
 *
 * Not copyable, not movable: the lifetime is the C++ scope, full stop.
 */
class CallerScope {
public:
    explicit CallerScope(std::string callerJson);
    ~CallerScope();

    CallerScope(const CallerScope&) = delete;
    CallerScope& operator=(const CallerScope&) = delete;
    CallerScope(CallerScope&&) = delete;
    CallerScope& operator=(CallerScope&&) = delete;

private:
    std::string m_previous;
};

} // namespace logos

#endif // LOGOS_CALLER_SCOPE_H
