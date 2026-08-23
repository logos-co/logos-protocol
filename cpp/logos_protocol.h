#ifndef LOGOS_PROTOCOL_H
#define LOGOS_PROTOCOL_H

/* ===========================================================================
 * logos_protocol.h — the public, language-neutral C ABI of logos-protocol.
 *
 * This is the ONE seam every Logos SDK builds on. The data model is
 * JSON-in-strings: method arguments are a JSON array, results are a JSON
 * value, event payloads are a JSON array — all UTF-8 `const char*`.
 *
 * Ownership:
 *   - Every `char*` RETURNED by this library is heap-allocated and owned by
 *     the caller; free it with lp_string_free() (safe on NULL).
 *   - Every `const char*` PASSED IN is borrowed for the duration of the call.
 *
 * Bytes encoding: binary data crossing this ABI is encoded inside JSON as
 *   {"_bytes": "<base64url>"}
 * (a single-key object). This is lossless for arbitrary bytes, including
 * embedded NUL. It matches the plain-wire encoding (json_mapping.cpp) and is
 * the canonical representation at this boundary.
 *
 * Error shape: structural failures report one canonical JSON object through
 * `out_error_json` / error callbacks:
 *   {"code": "<machine_code>", "message": "<human text>", "origin": "<module>"}
 *
 * Threading / event-loop contract:
 *   - Callbacks may arrive on an internal protocol thread — never assume
 *     they run on your own thread.
 *   - Handles are thread-safe per-handle: calls on one handle may be made
 *     from any thread; the library marshals to the handle's owner thread
 *     internally where required.
 *   - Qt-free transports (plain tcp/tcp_ssl, mock) are serviced by the
 *     library's own workers — no caller event loop is needed.
 *   - The Qt Remote Objects transport (the current default inside module
 *     processes) ADDITIONALLY requires a running Qt event loop in the
 *     process. Every Logos module process has one (logos_host runs it).
 *     Standalone non-Qt consumers must use the plain transport.
 *     A client on that transport is created on — and owned by — the Qt main
 *     thread no matter which thread calls lp_client_create(), because its
 *     node and socket are only serviced by that thread's loop. Calls from
 *     other threads marshal onto it and block until it answers.
 *   - lp_invoke() blocks the calling thread until the result arrives or the
 *     timeout elapses (timeout_ms <= 0 selects the default, currently 20s).
 *
 * Cancellation / lifetime:
 *   - After lp_client_destroy() / lp_unsubscribe() RETURNS, no further
 *     callbacks fire for that handle; pending async results are dropped.
 *     `user_data` may be freed only after that point, never before.
 *   - Both are safe to call from ANY thread, including a worker that happens
 *     to drop the last reference to a client. lp_client_destroy() defers the
 *     underlying teardown to the client's owner thread when called elsewhere,
 *     so the handle may outlive the call by an event-loop turn — the
 *     no-callbacks guarantee above holds regardless.
 *
 * Versioning: this library carries the logos-protocol semantic version —
 * the single number that governs Logos load/call compatibility. Two
 * participants interoperate iff they share the same MAJOR. MINOR is
 * additive/back-compatible; PATCH never affects compatibility.
 * =========================================================================== */

// HOW TO GUARD A CONDITIONAL SURFACE, and it is not what it looks like.
//
// Every entry below is additive at a MINOR, so codegen guards the surface on
// the version it appeared at. The obvious spelling is wrong:
//
//     #if LOGOS_PROTOCOL_VERSION_MINOR >= 5          // WRONG
//
// because at 1.0.0 the MINOR resets to 0 and every such guard silently goes
// false. Nothing fails to build and nothing fails to load — the definitions and
// the calls disappear together — so the symptom is modules quietly losing
// teardown and grantability, with no diagnostic anywhere. Compare the pair:
//
//     #if defined(LOGOS_PROTOCOL_VERSION_MINOR) &&     // RIGHT
//         (LOGOS_PROTOCOL_VERSION_MAJOR > 0 ||
//          (LOGOS_PROTOCOL_VERSION_MAJOR == 0 &&
//           LOGOS_PROTOCOL_VERSION_MINOR >= 5))
//
// Emit the arithmetic expanded rather than behind a function-like macro: the
// generated sources are resolved by `unifdef` in the backends' ABI checks, and
// unifdef handles nested integer arithmetic but silently no-ops on what it
// cannot evaluate. logos-rust-sdk already gets this right by comparing the
// tuple (major, minor).
#define LOGOS_PROTOCOL_VERSION_MAJOR 0
// 0.6: the caller of a dispatch — logos_module_set_call_caller()
// (logos_module_impl.h), which carries WHO is calling into the module image for
// the duration of one dispatch, plus the host half that produces the document
// (logos::CallerScope / logos::currentInboundCallerJson, logos_caller_scope.h,
// resolved by ModuleProxy::authorize as a by-product of the authorization scan
// it was already performing).
//
// Additive at the ABI level and at the INTERFACE level, which are two separate
// claims and both matter here. At the ABI: a cdylib generated below 0.6 exports
// no such symbol and the glue generated alongside it emits no call. At the
// interface: the caller is NOT a declared parameter and never appears in a
// .lidl — it is an ambient accessor — so no module's signature changes, nothing
// opts in per method, and a module that never asks is unaffected.
//
// WHY A MINOR AND NOT A PATCH, since the surface is only reachable through a
// generated call: a new REQUIRED module-impl export is exactly the thing the
// two prior ABI breaks were. Both (grant_host_services at 0.3, the teardown
// pair at 0.5) shipped with caller and module in perfect agreement about the
// version and still failed at dlopen on Linux only, because version agreement
// says nothing about which SYMBOLS a backend's emitter writes. The MINOR is
// what the guards are keyed to — the generator guard that emits the call, the
// backend guard that emits the definition, and the exports.txt every backend
// diffs itself against — so a surface with no MINOR of its own has no way to be
// guarded and no way to be checked.
// 0.5: the module teardown pair — logos_module_about_to_unload() and
// logos_module_set_unload_done_callback() (logos_module_impl.h), which let a
// module finish work before it is torn down. Additive at the ABI level: a
// cdylib generated below 0.5 exports neither, and the glue generated alongside
// it emits no calls, so an older module keeps the teardown it always had.
//
// An earlier version of this note went further and called that arrangement
// safe. It is not, and the ABI has been broken twice on the strength of it —
// grant_host_services at 0.3 and this pair at 0.5. Being generated in the same
// build makes the glue and the module agree on the VERSION; it says nothing
// about which SYMBOLS a given language backend's emitter writes for that
// version, because each backend implements this ABI independently. Both
// breakages happened at perfect version agreement, and both were invisible on
// macOS and fatal on Linux. See the note above logos_module_about_to_unload in
// logos_module_impl.h, and nix/module-impl-abi.nix, which publishes this
// header's export list so every backend can check itself against it.
// 0.4: per-identity token stores — lp_token_isolate_identity() and the four
// functions around it (lp_token_identity_is_isolated, lp_token_get_for,
// lp_token_save_for, lp_token_reset_identity), plus lp_client_create resolving
// its store through the origin instead of the image singleton. Additive: with
// nothing isolated, forIdentity() returns the same object instance() does, so
// every pre-0.4 host and binding runs on exactly the store it ran on before —
// the new symbols are the only way to get any other behaviour.
// 0.3: the trust-root surface — lp_grant_host_services() plus the two functions
// it gates (lp_token_keys, lp_inform_module_token_to), and the module-impl
// export that carries the grant across the cdylib boundary
// (logos_module_grant_host_services, logos_module_impl.h). Additive: no
// existing symbol changes behaviour, and an image that is never granted sees
// exactly the pre-0.3 surface — the gates are closed by default — so a host
// that knows nothing about the grant keeps working unmodified.
// 0.2: per-module concurrent dispatch ("multi"). Additive/back-compatible — a
// multi module returns a deferred-completion sentinel from callMethod and pushes
// the result as a __logos_call_complete__ event (see logos_async_dispatch.h);
// the provider/host ABI is UNCHANGED, so same-MAJOR hosts (incl. 0.1 daemons)
// load and forward multi modules without modification. A pre-0.2 *consumer*
// would see the raw sentinel rather than awaiting it — graceful, not a crash.
// 0.7: an isolated identity's OWN credential — lp_token_adopt_credential(),
// and the behaviour change that makes it necessary: a private token store is
// created EMPTY instead of inheriting this image's "core"/"capability_module"
// tokens. That inheritance handed every isolated identity the HOST's credential,
// which authorized as the host at any callee (the caller document came back
// {"kind":"host"}) and satisfied ModuleProxy::informModuleToken's
// trusted-channel gate — a write into another module's token map, reachable
// with three public calls and no generated glue.
//
// ADDITIVE AT THE ABI, BREAKING FOR ISOLATED IDENTITIES, and the two halves have
// to be said separately. No symbol changes signature, no module-impl export is
// added, and a host that never calls lp_token_isolate_identity /
// TokenManager::isolateIdentity is bit-for-bit unaffected: forIdentity() still
// returns instance() pointer-identically for every un-isolated name. A host that
// DOES isolate and does not adopt is broken loudly and immediately — its first
// outbound call dies at ModuleProxy::authorize's empty-token check with
// "auth token not recognized" — which is the intended failure mode for a change
// that removes a credential nobody was entitled to. Ship protocol, then
// logos-plugin-qt, then the hosts, with matching flake.locks.
#define LOGOS_PROTOCOL_VERSION_MINOR 7
#define LOGOS_PROTOCOL_VERSION_PATCH 0
#define LOGOS_PROTOCOL_VERSION_STRING "0.7.0"

/* ---------------------------------------------------------------------------
 * Export marking.
 *
 * These lp_* functions are the stable C ABI the JS and Rust SDKs bind to, so
 * they must appear in the export table of the shared build (liblogos_protocol
 * .dll / .so). On Windows that is not automatic: CMake builds shared libraries
 * with symbol export disabled unless symbols are marked explicitly or
 * WINDOWS_EXPORT_ALL_SYMBOLS is set -- the cross-built DLL was measured with
 * ZERO exports before this macro existed, so every FFI consumer would have
 * failed to bind at load time.
 *
 * Marked explicitly rather than via WINDOWS_EXPORT_ALL_SYMBOLS so the ABI
 * surface is the one we declare, not whatever happens to have external
 * linkage. Mirrors logos_module_impl.h's LOGOS_MODULE_IMPL_EXPORT.
 * ------------------------------------------------------------------------- */
#if defined(_WIN32)
#if defined(LOGOS_PROTOCOL_BUILDING_SHARED)
#define LP_API __declspec(dllexport)
#else
/* Consumers get plain declarations: dllimport would force them to link the
 * import library even when they use the static archive. */
#define LP_API
#endif
#else
#define LP_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Return codes (negative = failure). Functions returning int use these.
 * ------------------------------------------------------------------------- */
#define LP_OK 0
#define LP_ERR_INVALID_ARG (-1)
#define LP_ERR_UNSUPPORTED (-2) /* provider surface: exercised in a later phase */
#define LP_ERR_INTERNAL (-3)
#define LP_ERR_UNAVAILABLE (-4) /* target module/object could not be acquired */

/* ---------------------------------------------------------------------------
 * Version
 * ------------------------------------------------------------------------- */

/** Version string "MAJOR.MINOR.PATCH" of the linked logos-protocol.
 *  Returns a static string — do NOT free. */
LP_API const char* lp_protocol_version(void);

/** MAJOR component of the linked logos-protocol version. Equal majors are
 *  compatible; unequal majors are not. */
LP_API int lp_protocol_abi_major(void);

/* ---------------------------------------------------------------------------
 * Memory
 * ------------------------------------------------------------------------- */

/** Free a string returned by this library. Safe to call with NULL. */
LP_API void lp_string_free(char* s);

/* ---------------------------------------------------------------------------
 * Process-global mode / transport defaults
 * ------------------------------------------------------------------------- */

/** Set the process-wide communication mode: "remote" (IPC, default),
 *  "local" (in-process registry) or "mock" (in-memory, for tests).
 *  Returns LP_OK or LP_ERR_INVALID_ARG. */
LP_API int lp_set_mode(const char* mode);

/** Current mode as "remote" | "local" | "mock". Static string — do not free. */
LP_API const char* lp_get_mode(void);

/** Set the process-global default transport from a JSON object, e.g.
 *    {"protocol":"local"}
 *    {"protocol":"tcp","host":"127.0.0.1","port":6001,"codec":"json"}
 *    {"protocol":"tcp_ssl","host":"...","port":6443,"codec":"cbor",
 *     "ca_file":"...","cert_file":"...","key_file":"...","verify_peer":true}
 *  Returns LP_OK or LP_ERR_INVALID_ARG on parse failure. */
LP_API int lp_set_default_transport(const char* transport_json);

/* ---------------------------------------------------------------------------
 * Consumer: clients, invoke, subscribe
 * ------------------------------------------------------------------------- */

typedef struct lp_client lp_client;
typedef struct lp_subscription lp_subscription;

/** Result callback for lp_invoke_async.
 *  ok != 0 → `json` is the result JSON value; ok == 0 → `json` is the
 *  canonical error object. `json` is only valid for the duration of the
 *  callback — copy it if you need it longer. */
typedef void (*lp_result_cb)(int ok, const char* json, void* user_data);

/** Event callback for lp_subscribe. `data_json` is a JSON array (the event
 *  payload), valid only for the duration of the callback. */
typedef void (*lp_event_cb)(const char* event_name, const char* data_json,
                            void* user_data);

/**
 * Create a client for calling `target_module` on behalf of `origin_module`.
 *
 * `target_transport_json` / `capability_transport_json`: JSON object as for
 * lp_set_default_transport(), or NULL to use the process default. The
 * capability transport is used by the automatic `requestModule` token-fetch
 * flow (this library dials `capability_module` transparently the first time
 * a target requires a token — every language gets that flow for free).
 *
 * Owner thread: for a Qt-affine transport (Qt Remote Objects / local mode) the
 * client is constructed on the Qt main thread — blocking this call until that
 * thread runs it — because its node and socket are only serviced there. Any
 * thread may call this. For the Qt-free transports (tcp / tcp_ssl / mock) the
 * calling thread becomes the owner thread, as before.
 *
 * Returns NULL on invalid arguments.
 */
LP_API lp_client* lp_client_create(const char* target_module,
                            const char* origin_module,
                            const char* target_transport_json,
                            const char* capability_transport_json);

/** Destroy a client. After this returns, no further callbacks fire for the
 *  client or its subscriptions. */
LP_API void lp_client_destroy(lp_client* client);

/**
 * Call `method` on the client's target module, blocking until the result
 * arrives or the timeout elapses.
 *
 * `args_json`: JSON array of arguments (NULL means "[]").
 * `timeout_ms <= 0` selects the default timeout.
 *
 * On LP_OK: *out_result_json (if non-NULL) receives the result JSON value
 * (may be "null" — today's protocol does not distinguish "no result" from
 * a failed call at this level; that matches existing behavior).
 * On failure: *out_error_json (if non-NULL) receives the canonical error
 * object. Both out-strings are owned by the caller (lp_string_free).
 */
LP_API int lp_invoke(lp_client* client,
              const char* method,
              const char* args_json,
              int timeout_ms,
              char** out_result_json,
              char** out_error_json);

/**
 * Asynchronous variant of lp_invoke. Returns LP_OK if the call was
 * dispatched; `cb` then fires exactly once with the result (from the
 * client's owner thread). Safe to call from any thread.
 *
 * `cb` carries the same outcome the sync twin splits across its return code
 * and out-params: ok != 0 → `json` is the result JSON value; ok == 0 → `json`
 * is the canonical error object lp_invoke would have written to
 * out_error_json. A LP_OK return therefore means "dispatched", never
 * "succeeded" — the outcome is only known in the callback.
 *
 * WHAT ok == 0 COVERS, precisely, because "the same outcome as the sync twin"
 * is a statement about PARITY and not about completeness. Reported: failure to
 * acquire the target ("object_unavailable"), a call that exceeds its deadline,
 * a rejected auth token, and MODULE_NOT_LOADED from a host that is up. Both
 * twins report all four; neither did before.
 *
 * NOT reported, and it is not an oversight: an unknown method name. Every
 * provider flavour answers one with a bare null, byte-identical to a method
 * that legitimately returns null, so the distinction does not exist on the
 * wire to be reported. Closing it needs a provider-contract change across the
 * SDKs, not a transport change here. A provider's own rejection of well-formed
 * arguments ("dispatch_failed") is likewise NOT folded in by either twin — it
 * arrives as a result, and the generated wrappers fold it.
 *
 * Argument/handle validation still fails synchronously with
 * LP_ERR_INVALID_ARG and `cb` is NOT called in that case.
 */
LP_API int lp_invoke_async(lp_client* client,
                    const char* method,
                    const char* args_json,
                    int timeout_ms,
                    lp_result_cb cb,
                    void* user_data);

/**
 * Subscribe to `event_name` emitted by the client's target module.
 * `cb` fires once per event with the payload as a JSON array.
 *
 * The target module does NOT have to be reachable yet. This is the normal
 * case, not an edge case: a module subscribes to its dependency during init(),
 * and a ui_qml backend during onContextReady(), both of which run while the
 * dependency's host has been spawned but has not called listen(). The
 * subscription is held and armed when the module appears — including a
 * mid-session package install — so a NULL return means the ARGUMENTS were
 * refused, never "not there yet".
 *
 * What it does not promise: arming is not retroactive and no transport buffers,
 * so an event the module emits in the window before the subscription arms
 * reaches nobody. A module that fires a one-shot "ready" event synchronously
 * inside its own init() can still be missed; if that event matters, expose a
 * method the subscriber can call after subscribing.
 *
 * Returns NULL only for a null/empty client, event name or callback.
 */
LP_API lp_subscription* lp_subscribe(lp_client* client,
                              const char* event_name,
                              lp_event_cb cb,
                              void* user_data);

/** Cancel a subscription. After this returns the callback will not fire again
 *  (already-running invocations are allowed to finish first) — that part is
 *  synchronous and unconditional.
 *
 *  The client also stops TRACKING it, so a subscription cancelled while still
 *  waiting for its module leaves the retry machinery instead of being warned
 *  about forever. That half is EVENTUAL, not immediate: it is posted to the
 *  client's owner thread and takes effect on a later turn of that thread's
 *  event loop. Doing it synchronously would mean blocking on the owner thread
 *  while holding a lock that thread's delivery callback also takes — a
 *  deadlock, and an outright hang once that event loop has stopped, which is
 *  exactly when a language binding's subscription handle is dropped.
 *
 *  Consequence for callers: lp_pending_subscriptions() may still list a
 *  just-cancelled subscription until the owner thread runs. If the client is
 *  destroyed first the cancellation simply never runs, which is correct — the
 *  registry died with it. */
LP_API void lp_unsubscribe(lp_subscription* sub);

/** Diagnostics: a JSON array of "<module>::<event>" for every subscription on
 *  this client that has been accepted but has not armed yet — i.e. is waiting
 *  for its module to appear. `[]` when everything is live.
 *
 *  Exists because the Qt consumer has had this visibility all along and the C
 *  ABI had none, which is precisely why a subscription that silently never
 *  armed was undetectable from Rust, Nim or a universal C++ module. Caller
 *  frees via lp_string_free; NULL only for a null client. */
LP_API char* lp_pending_subscriptions(lp_client* client);

/** Introspect the target module's methods/events as a JSON array (the
 *  same shape `lm` prints). Caller frees via lp_string_free. NULL on
 *  failure. */
LP_API char* lp_get_methods(lp_client* client);

/* ---------------------------------------------------------------------------
 * Tokens
 * ------------------------------------------------------------------------- */

/** Get the stored token for `module_name`. Returns NULL when absent;
 *  caller frees via lp_string_free.
 *
 *  Reads this IMAGE's store — the one lp_client_create uses for every origin
 *  that has not been isolated. For an isolated origin, see lp_token_get_for. */
LP_API char* lp_token_get(const char* module_name);

/** Store a token for `module_name` in this image's store. */
LP_API int lp_token_save(const char* module_name, const char* token);

/* --- per-identity token stores ---------------------------------------------
 *
 * A host that loads several modules IN ONE IMAGE gives all of them the same
 * token store, and that store holds every loaded module's own auth token. Since
 * a client presents a cached token before it ever mints one, each module in such
 * a host can reach every other one with authority it was never granted, and no
 * `requestModule` is logged. These four functions let a host give a named
 * identity a store of its own, so origin SELECTS the tokens a caller can present
 * instead of merely labelling it.
 *
 * Additive and inert by default: with nothing isolated, every function above
 * and every lp_client_create behaves exactly as before, on the same store.
 * ------------------------------------------------------------------------- */

/** Give `identity` a private token store. The store is created EMPTY — it does
 *  NOT inherit this image's "core" / "capability_module" tokens, which are the
 *  HOST's credential and would let the identity authorize as the host. The host
 *  must give the identity its OWN credential with lp_token_adopt_credential
 *  before it can call anything; until then every call it makes is refused.
 *
 *
 *  Idempotent. Returns LP_ERR_UNSUPPORTED — changing nothing — if a client for
 *  this identity was already created against the shared store; isolating then
 *  would leave some callers on the shared store and some on the private one.
 *  Call this BEFORE creating any client for the identity, and treat the refusal
 *  as fatal for that identity rather than continuing. */
LP_API int lp_token_isolate_identity(const char* identity);

/** 1 if `identity` has a private store, 0 if it shares this image's store,
 *  LP_ERR_INVALID_ARG for NULL. */
LP_API int lp_token_identity_is_isolated(const char* identity);

/** Get the token `identity` would present to `module_name`. NULL when absent;
 *  caller frees via lp_string_free. Reads this image's shared store for an
 *  identity that has not been isolated — and, like every use of an identity's
 *  store, that counts as vending the shared store: isolate FIRST, then read. */
LP_API char* lp_token_get_for(const char* identity, const char* module_name);

/** Store a token in `identity`'s store — how a host seeds an isolated identity
 *  with the tokens it is actually entitled to. For the identity's OWN
 *  credential, use lp_token_adopt_credential instead: it owns the bootstrap key
 *  set, so a binding never has to spell "core"/"capability_module" itself.
 *  Writes this image's shared store
 *  for an identity that has not been isolated, which is almost certainly not
 *  what the caller meant: the token becomes visible to every other non-isolated
 *  caller, and lp_token_isolate_identity then refuses the name rather than
 *  stranding this write outside the private store. Isolate first. */
LP_API int lp_token_save_for(const char* identity, const char* module_name,
                             const char* token);

/** Clear an isolated identity's store — the unload hook, so a reloaded module
 *  does not present tokens minted for its previous incarnation. The identity's
 *  CREDENTIAL goes with it: a reload re-mints and re-registers, which
 *  invalidates the old credential at the target, so the caller must follow this
 *  with lp_token_adopt_credential for the new one. Returns LP_ERR_UNSUPPORTED
 *  for a non-isolated identity, whose store is shared and must not be cleared
 *  out from under everyone else. */
LP_API int lp_token_reset_identity(const char* identity);

/** Install `credential` as `identity`'s OWN credential in its private store:
 *  its value under every bootstrap key ("core", "capability_module"). This is
 *  what makes an isolated identity able to speak at all — it is the token
 *  presented to `capability_module.requestModule`, and the token
 *  capability_module pushes back with.
 *
 *  The host mints `credential`, registers it with capability_module
 *  (lp_inform_module_token / informModuleToken over the trusted channel) and
 *  only THEN calls this. Register-before-adopt, so at no instant does the
 *  identity hold a credential capability_module has not yet accepted.
 *
 *  Returns LP_ERR_INVALID_ARG for a NULL/empty argument, and LP_ERR_UNSUPPORTED
 *  — writing nothing — when `identity` is not isolated (the store would be the
 *  shared one, handing the credential to every un-isolated caller) or when
 *  `credential` is this image's own host anchor (adopting the host's credential
 *  as your own is the elevation this whole surface exists to prevent). */
LP_API int lp_token_adopt_credential(const char* identity, const char* credential);

/** The module names this image's token store holds, as a JSON array. Caller
 *  frees via lp_string_free.
 *
 *  Requires the "token_registry" host service (lp_grant_host_services): an
 *  ungranted image gets NULL. NULL is therefore "refused", never "empty" — a
 *  granted call with nothing stored returns "[]". Order is unspecified.
 *
 *  This is the known-caller gate a trust-root module needs: it answers "have I
 *  ever been handed a token for this module?" without exposing any token
 *  value. */
LP_API char* lp_token_keys(void);

/** Deliver a module token to the client's target (the consumer-side
 *  `informModuleToken`). Returns LP_OK when the target accepted it.
 *
 *  Note the fixed destination: this reaches `capability_module`, whatever the
 *  client's target is. It is the CONSUMER half of the exchange — "core, here is
 *  a token" — not a way to push a token at an arbitrary module. For that, see
 *  lp_inform_module_token_to below. */
LP_API int lp_inform_module_token(lp_client* client,
                           const char* auth_token,
                           const char* module_name,
                           const char* token);

/** Deliver the token for `module_name` TO `origin_module` — the provider half
 *  of the exchange, the direction capability_module pushes.
 *
 *  Prefers `origin_module`'s handshake surface so a target still running its
 *  initializer is reachable, and falls back to its business object for modules
 *  built before that surface existed. `timeout_ms <= 0` selects the default
 *  (20s), which bounds the fallback acquire and the call together.
 *
 *  Requires the "token_delivery" host service (lp_grant_host_services): an
 *  ungranted image gets LP_ERR_UNSUPPORTED. Returns LP_ERR_INTERNAL when the
 *  target refused or could not be reached. */
LP_API int lp_inform_module_token_to(lp_client* client,
                              const char* auth_token,
                              const char* origin_module,
                              const char* module_name,
                              const char* token,
                              int timeout_ms);

/* ---------------------------------------------------------------------------
 * Host services — the privileged surface a trust-root module is granted
 *
 * Two functions above are closed by default and opened only by an explicit
 * grant: lp_token_keys ("token_registry") and lp_inform_module_token_to
 * ("token_delivery"). They are what a capability/trust-root module needs and
 * what an ordinary module must not have.
 *
 * The grant is per-IMAGE, and that is the whole point rather than an
 * implementation detail. A host binary and a module cdylib each link their own
 * copy of this library, so each has its own process-global state — a grant
 * recorded in the host is invisible to a cdylib calling lp_token_keys. The
 * grant therefore has to cross the module-impl C ABI exactly as the auth token
 * already does (logos_module_grant_host_services in logos_module_impl.h), and
 * a gate "simplified" into the host would silently never fire.
 * ------------------------------------------------------------------------- */

/** Grant this image the named host services. `services_json` is a JSON array
 *  drawn from the closed set {"token_registry", "token_delivery"}.
 *
 *  REPLACES the current grant rather than adding to it. NULL, the empty string
 *  or `[]` clears it, closing both gates again — clearing is the fail-closed
 *  direction, so the lenient input handling costs nothing.
 *
 *  Returns LP_ERR_INVALID_ARG for malformed JSON, a non-array, a non-string
 *  element, or ANY unrecognised service name; the existing grant is left
 *  untouched in that case. An unknown name is a caller that believes it holds a
 *  privilege it does not, which must not be absorbed silently. */
LP_API int lp_grant_host_services(const char* services_json);

/* ---------------------------------------------------------------------------
 * Provider (GROUNDWORK — defined and compiled in this version, fully
 * exercised when module authoring lands on the common cdylib module-impl
 * C ABI. Until then lp_provider_register/emit return LP_ERR_UNSUPPORTED.)
 * ------------------------------------------------------------------------- */

typedef struct lp_provider lp_provider;

/** Dispatch a method call. Return a heap string (result JSON value) that the
 *  library frees with lp_string_free; return NULL to signal failure. */
typedef char* (*lp_dispatch_cb)(const char* method, const char* args_json,
                                void* user_data);

/** Return the module's method/event metadata as a JSON array (heap string,
 *  freed by the library via lp_string_free). */
typedef char* (*lp_getmethods_cb)(void* user_data);

/** Accept a token delivered by another module. Return LP_OK to accept. */
typedef int (*lp_token_cb)(const char* module_name, const char* token,
                           void* user_data);

LP_API lp_provider* lp_provider_create(const char* module_name,
                                const char* transport_set_json);
LP_API void lp_provider_destroy(lp_provider* provider);
LP_API int lp_provider_register(lp_provider* provider,
                         lp_dispatch_cb dispatch,
                         lp_getmethods_cb get_methods,
                         lp_token_cb on_token,
                         void* user_data);
LP_API int lp_provider_emit_event(lp_provider* provider,
                           const char* event_name,
                           const char* data_json);
LP_API int lp_provider_save_token(lp_provider* provider,
                           const char* module_name,
                           const char* token);

#ifdef __cplusplus
}
#endif

#endif /* LOGOS_PROTOCOL_H */
