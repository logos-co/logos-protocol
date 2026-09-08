#!/usr/bin/env bash
# Assert a build links ONE logos-protocol-lib and ONE logos-qt-host, and that
# the qt-host was built against the very protocol-lib the build links.
#
# logos-qt-host bakes sizeof(LogosAPIClient) into its own `operator new`; this
# repo DEFINES the constructor. Split those and the allocation and the
# constructor disagree. On macOS an undersized request rounds up into the next
# malloc size class, so the overrun lands in allocator slack and nothing
# happens; on glibc it is `malloc(): corrupted top size`, blamed on an
# unrelated later allocation.
set -euo pipefail

out=${1:?usage: logos-abi-closure-check <store-path>}

closure() { nix path-info -r "$1" 2>/dev/null; }
pick() { closure "$1" | grep -E -- "-$2-[0-9]" || true; }
count() { printf '%s' "$1" | grep -c . || true; }
fail() { echo "FAIL: $*" >&2; exit 1; }

[ -e "$out" ] || fail "$out does not exist"

protos=$(pick "$out" logos-protocol-lib)
hosts=$(pick "$out" logos-qt-host)
np=$(count "$protos")
nh=$(count "$hosts")

echo "subject: $out"
echo "  logos-protocol-lib: $np"
[ "$np" -gt 0 ] && printf '%s\n' "$protos" | sed 's#^#    #'
echo "  logos-qt-host:      $nh"
[ "$nh" -gt 0 ] && printf '%s\n' "$hosts" | sed 's#^#    #'

# A zero-length list compares clean against anything. A core module statically
# links both and legitimately has none -- it is not this check's subject, and
# passing it here would let the check be wired to the wrong output and report
# green over nothing.
if [ "$np" -eq 0 ] && [ "$nh" -eq 0 ]; then
  fail "no logos-protocol-lib and no logos-qt-host in this closure.
      Either this output statically links them -- a module, which is not this
      check's subject -- or the package naming moved and this check just
      inspected nothing."
fi

[ "$np" -le 1 ] || fail "$np distinct logos-protocol-lib in ONE closure.
      Two copies of the protocol runtime in one process. Invisible on macOS,
      fatal on glibc."

[ "$nh" -le 1 ] || fail "$nh distinct logos-qt-host in ONE closure.
      Consumers split between them, so a Qt-facing API added to qt-host is
      invisible to whichever one loses -- silently, with a green build."

# Same version string is not the same library: this is the assertion that
# catches a consumer relocking logos-protocol without moving logos-plugin-qt.
if [ "$nh" -eq 1 ] && [ "$np" -eq 1 ]; then
  hp=$(pick "$hosts" logos-protocol-lib)
  [ "$(count "$hp")" -eq 1 ] || fail "logos-qt-host references $(count "$hp") logos-protocol-lib; expected 1."
  [ "$hp" = "$protos" ] || fail "logos-qt-host was built against a DIFFERENT logos-protocol-lib
      than this output links:
        qt-host built against : $hp
        this output links     : $protos
      Its baked sizeof(LogosAPIClient) therefore need not match the constructor
      that runs. Repin so both resolve to one revision."
  echo "  paired: qt-host built against the protocol-lib this output links"
fi

echo "PASS"
