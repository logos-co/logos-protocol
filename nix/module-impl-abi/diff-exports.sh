#!/usr/bin/env bash
#
#   diff-exports.sh <declared.txt> <defined.txt> <backend-label> [emitter-hint]
#
# Assert that a language backend DEFINES every module-impl export logos-protocol
# DECLARES, and explain the failure well enough to act on without context.
#
# Why every backend owes this, and why it is not obvious:
#
#   logos-protocol only DECLARES the module-impl C ABI. Each language backend
#   generates its own definitions, so "the header has it" and "my modules export
#   it" are independent facts. The gap has shipped twice — grant_host_services
#   at protocol 0.3, and the teardown pair at 0.5 — both times with the caller
#   and the module in perfect agreement about the protocol VERSION. Version
#   agreement is necessary and not sufficient: it says nothing about which
#   symbols a given backend's emitter happens to write.
#
#   The gap is invisible until runtime and then only on some platforms. An
#   undefined symbol in an ELF shared object is legal at link time and resolved
#   at load, so the module links clean; nixpkgs hardens with -Wl,-z,now, which
#   makes that resolution eager and therefore fatal at dlopen(). macOS links
#   plugins with -undefined dynamic_lookup and never binds at all, so a green
#   Darwin build proves nothing about Linux.
#
# The comparison is one-directional on purpose: DECLARED must be a subset of
# DEFINED. A backend may export extra symbols of its own (the Rust scaffold
# declares an `extern "Rust"` install hook, for instance) and that is not this
# script's business.
set -euo pipefail

declared="${1:?usage: diff-exports.sh <declared.txt> <defined.txt> <label> [hint]}"
defined="${2:?}"
label="${3:?}"
hint="${4:-}"

for f in "$declared" "$defined"; do
  [ -r "$f" ] || { echo "diff-exports: cannot read $f" >&2; exit 1; }
done

# Anti-vacuity. Reaching here with an empty side means the extraction upstream
# silently produced nothing, and comm would then report no missing symbols — a
# green check over an unexamined ABI.
for f in "$declared" "$defined"; do
  [ -s "$f" ] || { echo "diff-exports: $f is empty; refusing to compare (see the note above)" >&2; exit 1; }
done

missing=$(comm -23 <(sort -u "$declared") <(sort -u "$defined"))

if [ -n "$missing" ]; then
  {
    echo "FAIL: [$label] does not define every module-impl C ABI export."
    echo
    echo "  DECLARED by logos-protocol but NOT DEFINED by this backend:"
    printf '%s\n' "$missing" | sed 's/^/      - /'
    echo
    echo "  A module built from this backend will link cleanly and then fail at"
    echo "  dlopen() on Linux with \"undefined symbol: <name>\". It will appear to"
    echo "  work on macOS. The runtime reports the module as LOADED — the host"
    echo "  process dies just after the token exchange — so what you see instead"
    echo "  is other modules timing out waiting for a replica that never appears."
    echo
    [ -n "$hint" ] && echo "  Add the definition here: $hint" && echo
    echo "  Declared exports come from logos-protocol's own header, at the exact"
    echo "  revision this build pins — so this list is what YOUR protocol asks"
    echo "  for, not a hardcoded expectation."
  } >&2
  exit 1
fi

echo "[$label] defines all $(sort -u "$declared" | wc -l | tr -d ' ') declared module-impl exports."
