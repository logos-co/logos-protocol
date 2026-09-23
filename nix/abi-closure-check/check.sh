#!/usr/bin/env bash
# Assert a build links ONE logos-protocol-lib and ONE logos-qt-host, and that
# the qt-host was built against the very protocol-lib the build links. Also at
# most ONE logos-protocol-plain-lib, built from the source the Qt one was.
# A static link names neither in the runtime closure, so every derivation in
# the build graph that links both is checked the same way.
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
# One line per qt-host a derivation links alongside a protocol-lib, looking
# through one aggregate (logos-protocol, logos-qt-sdk) to the libraries.
static_links() {
  nix derivation show -r "$1" 2>/dev/null | jq -r '
    (.derivations // .) | with_entries(.key |= sub("^/nix/store/"; "")) as $g
    | def ins($d): ($g[$d].inputs.drvs // $g[$d].inputDrvs // {}) | keys | map(sub("^/nix/store/"; ""));
      def lib($d; $re): $d | sub("^[a-z0-9]{32}-"; "") | test($re);
    $g | keys[] as $l
    | ((ins($l) + [ins($l)[] as $i | ins($i)[]]) | unique) as $near
    | [$near[] | select(lib(.; "^logos-protocol-lib-[0-9]"))] as $links
    | [$near[] | select(lib(.; "^logos-qt-host-[0-9]"))][] as $host
    | select($links | length > 0)
    | [ins($host)[] | select(lib(.; "^logos-protocol-lib-[0-9]"))] as $against
    | "\(if ($links | length) == 1 and $against == $links then "paired" else "MISPAIRED" end) \($l) qt-host \($host) built against \($against | join(" ")) links \($links | join(" "))"'
}
pick() { closure "$1" | grep -E -- "-$2-[0-9]" || true; }
count() { printf '%s' "$1" | grep -c . || true; }
fail() { echo "FAIL: $*" >&2; exit 1; }

[ -e "$out" ] || fail "$out does not exist"

case "$out" in
  *.drv) drv=$out; protos=""; plains=""; hosts="" ;;
  *) drv=$(nix-store --query --deriver "$out" 2>/dev/null || true)
     protos=$(pick "$out" logos-protocol-lib)
     plains=$(pick "$out" logos-protocol-plain-lib)
     hosts=$(pick "$out" logos-qt-host) ;;
esac
statics=""
[ -n "$drv" ] && statics=$(static_links "$drv" || true)
np=$(count "$protos")
nl=$(count "$plains")
nh=$(count "$hosts")
ns=$(count "$statics")

echo "subject: $out"
echo "  logos-protocol-lib:       $np"
[ "$np" -gt 0 ] && printf '%s\n' "$protos" | sed 's#^#    #'
echo "  logos-protocol-plain-lib: $nl"
[ "$nl" -gt 0 ] && printf '%s\n' "$plains" | sed 's#^#    #'
echo "  logos-qt-host:            $nh"
[ "$nh" -gt 0 ] && printf '%s\n' "$hosts" | sed 's#^#    #'
echo "  static qt-host links:     $ns"
[ "$ns" -gt 0 ] && printf '%s\n' "$statics" | sed 's#^#    #'

# A zero-length list compares clean against anything: passing it here would let
# the check be wired to the wrong output and report green over nothing.
if [ "$np" -eq 0 ] && [ "$nl" -eq 0 ] && [ "$nh" -eq 0 ] && [ "$ns" -eq 0 ]; then
  fail "no logos-protocol-lib, logos-protocol-plain-lib or logos-qt-host in this closure,
      and no derivation in its build links a qt-host. Either this is the wrong
      output, its derivation is not in the local store, or the package naming
      moved and this check just inspected nothing."
fi

[ "$np" -le 1 ] || fail "$np distinct logos-protocol-lib in ONE closure.
      Two copies of the protocol runtime in one process. Invisible on macOS,
      fatal on glibc."

[ "$nl" -le 1 ] || fail "$nl distinct logos-protocol-plain-lib in ONE closure.
      Two plain runtimes, each with its own token registry."

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

mispaired=$(printf '%s\n' "$statics" | grep '^MISPAIRED' || true)
[ -z "$mispaired" ] || fail "a qt-host is statically linked with a logos-protocol-lib other than
      the one it was built against:
$(printf '%s\n' "$mispaired" | sed 's#^#        #')
      Its baked sizeof(LogosAPIClient) therefore need not match the constructor
      linked beside it. Repin so both resolve to one revision."
[ "$ns" -gt 0 ] && echo "  paired: every static qt-host link takes the protocol-lib it was built against"

# Both runtimes in one process speak one wire: they must be one revision.
if [ "$np" -eq 1 ] && [ "$nl" -eq 1 ]; then
  qs=$(cat "$protos/share/logos-protocol/source" 2>/dev/null || true)
  ps=$(cat "$plains/share/logos-protocol/source" 2>/dev/null || true)
  [ -n "$qs" ] && [ "$qs" = "$ps" ] || fail "the Qt and plain protocol runtimes come from different sources:
        logos-protocol-lib       : ${qs:-(unrecorded)}
        logos-protocol-plain-lib : ${ps:-(unrecorded)}
      Repin so both resolve to one logos-protocol revision."
  echo "  paired: the Qt and plain runtimes come from one source"
fi

echo "PASS"
