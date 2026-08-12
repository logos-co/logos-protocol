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
  surface (`lp_grant_host_services` and the two functions it gates), and the
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
