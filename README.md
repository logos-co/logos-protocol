# logos-protocol

The Logos protocol layer: transports, token exchange, and the
**language-neutral `lp_*` C ABI** (`cpp/logos_protocol.h`) that every Logos
SDK builds on.

Extracted from `logos-cpp-sdk` so that non-C++ SDKs (Rust, …) consume the
same transports, capability/token flow and wire behavior through one stable,
versioned boundary instead of re-wrapping the C++/Qt SDK.

## What lives here

- **Public C ABI** — `cpp/logos_protocol.h`: consumer surface
  (`lp_client_*`, `lp_invoke[_async]`, `lp_subscribe`, tokens,
  `lp_get_methods`), provider groundwork (`lp_provider_*`), the trust-root
  surface (`lp_grant_host_services` and the two functions it gates),
  per-identity token stores (`lp_token_isolate_identity`, `lp_token_get_for`,
  `lp_token_save_for`, `lp_token_reset_identity`,
  `lp_token_identity_is_isolated`), and the
  **protocol version** (`LOGOS_PROTOCOL_VERSION_*`, `lp_protocol_version()`,
  `lp_protocol_abi_major()`). JSON-in-strings data model; bytes cross the
  boundary as `{"_bytes":"<base64url>"}` (lossless, NUL-safe).
- **Transports** — plain TCP / TCP+TLS (Boost.Asio + OpenSSL + nlohmann,
  Qt-free), `qt_local`, in-memory mock, and Qt Remote Objects
  (`qt_remote` — the only Qt-bearing transport).
- **Consumer core** — `LogosAPIClient` / `LogosAPIConsumer` including the
  automatic `capability_module.requestModule` token-fetch flow (behind the
  protocol boundary: every language gets it for free).
- **Provider-side plumbing** — `ModuleProxy` (auth gate the transports
  publish) and the abstract `LogosProviderObject` interface
  (`logos_provider_interface.h`).
- **Token manager**, transport/registry factories, mode config
  (remote/local/mock), and the canonical QVariant↔JSON conversion used at
  the QRO boundary.

### Per-identity token stores

`TokenManager::instance()` is the **image's** store. In a host that loads
several modules *in one image* it is also an ambient ring: the host writes
`name -> that module's root auth token` for every module it loads, and a client
presents a cached token before it ever mints one — so any module in that image
can reach any other with authority it was never granted, and no `requestModule`
appears in the log. Per-module *origin strings* do not change that, because
origin was never consulted on the path taken.

`TokenManager::forIdentity(origin)` makes origin **select the store** instead of
merely labelling the caller, and `isolateIdentity(origin)` is how a host opts a
name in (`lp_token_isolate_identity` and friends from C). Both are additive and
inert by default: until a name is isolated, `forIdentity()` returns the *same
object* `instance()` returns, so a host that knows nothing about this is
unchanged. A private store is seeded with the trust-root bootstrap (`core`,
`capability_module`) so first-call `requestModule` still works, and with nothing
else.

This is a second axis, not a replacement for the per-**image** split: a module
cdylib links its own copy of this library and therefore has its own
`instance()`, which stays correct as-is.

`logos-cpp-sdk` layers the typed C++ developer API (`LogosAPI`, module
context, code generator, provider base classes) on top of this repo.

## Versioning

This repo carries the **logos-protocol semver** — the single number that
governs Logos load/call compatibility. Two participants (modules, hosts,
SDKs in any language) interoperate **iff they share the same MAJOR**. MINOR
is additive/back-compatible; PATCH never affects compatibility. SDKs must
re-expose the version of the protocol they linked (never mint their own).

## Building

```bash
# Via workspace
ws build logos-protocol

# Standalone
nix build

# Tests
nix build .#tests
```

## Layering invariant

`logos-protocol` depends only on Qt / Boost / OpenSSL / nlohmann_json — it
must NEVER depend on logos-cpp-sdk, logos-qt-sdk, logos-rust-sdk, liblogos
or logos-lidl. Everything points inward.
