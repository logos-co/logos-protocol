#!/usr/bin/env bash
#
# Print the module-impl C ABI exports declared in logos_module_impl.h, one name
# per line, sorted.
#
# This runs in logos-protocol's OWN build, which is the point: the repo that
# declares the ABI is the one that fails when the ABI can no longer be read.
# Every backend then consumes the resulting list as data instead of re-deriving
# it, so there is one parser to keep working rather than one per language.
#
# Under-reporting is the dangerous direction. A short list makes a consumer's
# diff pass VACUOUSLY — the check stays green while the ABI is incomplete,
# which is strictly worse than having no check. So this fails loudly rather
# than print a list it is not sure about.
set -euo pipefail

hdr="${1:?usage: extract-exports.sh <logos_module_impl.h>}"
[ -r "$hdr" ] || { echo "extract-exports: cannot read $hdr" >&2; exit 1; }

# A declaration is `LOGOS_MODULE_IMPL_EXPORT <ret> <name>(<params>);`, and the
# parameter list may continue over several lines. Collect each LOGICAL
# declaration (marker through the `;`) rather than assuming one per line — a
# line-oriented parser silently loses a declaration the moment someone reflows
# the header, and losing one is exactly the failure this file exists to prevent.
# `#define LOGOS_MODULE_IMPL_EXPORT ...` is excluded explicitly.
names=$(awk '
  /LOGOS_MODULE_IMPL_EXPORT/ && !/^[[:space:]]*#[[:space:]]*define/ {
    buf = $0; collecting = 1
  }
  collecting && buf !~ /;/ && $0 != buf { buf = buf " " $0 }
  collecting && buf ~ /;/ {
    sub(/.*LOGOS_MODULE_IMPL_EXPORT[[:space:]]+/, "", buf)
    # The function name is the first identifier immediately followed by "(".
    # Return types (void, int, char*, const char*) are never followed by one.
    if (match(buf, /[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(/)) {
      n = substr(buf, RSTART, RLENGTH); sub(/[[:space:]]*\($/, "", n); print n
    }
    collecting = 0; buf = ""
  }
' "$hdr" | { grep -E '^logos_module_[a-z0-9_]+$' || true; } | sort -u)

count=$(printf '%s\n' "$names" | { grep -c . || true; })

# The floor is ASSERTED, not derived from the header, and that is deliberate:
# it is the one statement here that a header change cannot silently satisfy. If
# the parser stops understanding the file, the derived count collapses and this
# catches it; a guard derived from the same parse would collapse with it.
#
# These seven have existed since the ABI was introduced (logos-protocol #3).
# The ABI only grows, so this never needs touching to ADD an export. If one is
# ever genuinely removed, editing this line is the deliberate acknowledgement.
floor="logos_module_dispatch
logos_module_get_methods
logos_module_set_context
logos_module_set_emit_callback
logos_module_accept_token
logos_module_get_protocol_version
logos_module_string_free"

missing=""
while IFS= read -r f; do
  [ -n "$f" ] || continue
  printf '%s\n' "$names" | grep -qx "$f" || missing="$missing $f"
done <<< "$floor"

if [ -n "$missing" ]; then
  {
    echo "extract-exports: parsed $count export(s) from $hdr, but these founding"
    echo "                 exports are absent:$missing"
    echo
    echo "  Either the header changed shape and the parser above needs fixing, or an"
    echo "  export was genuinely removed. Refusing to publish a list that would make"
    echo "  every backend's ABI check pass vacuously."
  } >&2
  exit 1
fi

printf '%s\n' "$names"
