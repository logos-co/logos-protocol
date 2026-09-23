# Proves logos-abi-closure-check can still FAIL.
#
# Every assertion it makes has "no finding" as its pass verdict, which is the
# shape that goes vacuous without anyone noticing: a check that stopped
# resolving store paths would report a clean pairing over an empty set. So each
# assertion is exercised in the failing direction here, against synthetic
# closures fed through a stub `nix` on PATH.
{ pkgs, common, src, abi-closure-check }:

pkgs.runCommand "logos-abi-closure-check-tests"
{
  nativeBuildInputs = [ pkgs.bash pkgs.coreutils pkgs.gnugrep pkgs.gnused ];
  meta = common.meta or { };
}
''
  set -euo pipefail
  check=${abi-closure-check}/bin/logos-abi-closure-check

  mkdir -p stub && cd "$PWD"
  subject=$PWD/SUBJECT
  touch "$subject"

  # $1 = the subject's closure, $2 = the qt-host's closure.
  mkstub() {
    mkdir -p stub
    { echo '#!${pkgs.bash}/bin/bash'
      echo 'p="''${!#}"'
      echo 'case "$p" in'
      echo "  *SUBJECT*) printf '%s' \"$1\" ;;"
      echo "  *logos-qt-host*) printf '%s' \"$2\" ;;"
      echo '  *) ;;'
      echo 'esac'
    } > stub/nix
    chmod +x stub/nix
  }

  expect_fail() {
    local label="$1"; shift
    if PATH="$PWD/stub:$PATH" "$check" "$subject" > out.txt 2>&1; then
      echo "FAIL: [$label] the check PASSED where it must fail:" >&2
      sed 's/^/    /' out.txt >&2
      exit 1
    fi
    grep -q "$1" out.txt || {
      echo "FAIL: [$label] failed for the wrong reason (expected /$1/):" >&2
      sed 's/^/    /' out.txt >&2
      exit 1
    }
    echo "  ok: $label"
  }

  echo "=== each assertion, exercised in the failing direction ==="

  mkstub "" ""
  expect_fail "empty closure is not a pass" "inspected nothing"

  mkstub "/nix/store/a-logos-protocol-lib-0.9.0
/nix/store/b-logos-protocol-lib-0.9.0" ""
  expect_fail "two protocol-lib" "distinct logos-protocol-lib"

  mkstub "/nix/store/a-logos-qt-host-0.1.0
/nix/store/b-logos-qt-host-0.1.0" ""
  expect_fail "two qt-host" "distinct logos-qt-host"

  mkstub "/nix/store/a-logos-protocol-lib-0.9.0
/nix/store/b-logos-qt-host-0.1.0" "/nix/store/c-logos-protocol-lib-0.8.0"
  expect_fail "qt-host paired with another protocol-lib" "built against a DIFFERENT"

  mkstub "/nix/store/a-logos-protocol-plain-lib-0.12.0
/nix/store/b-logos-protocol-plain-lib-0.12.0" ""
  expect_fail "two protocol-plain-lib" "distinct logos-protocol-plain-lib"

  # Runtimes that record the source they were built from.
  store=$PWD/store
  for runtime in q-logos-protocol-lib p-logos-protocol-plain-lib o-logos-protocol-plain-lib; do
    mkdir -p $store/$runtime-0.12.0/share/logos-protocol
  done
  echo src-one > $store/q-logos-protocol-lib-0.12.0/share/logos-protocol/source
  echo src-one > $store/p-logos-protocol-plain-lib-0.12.0/share/logos-protocol/source
  echo src-two > $store/o-logos-protocol-plain-lib-0.12.0/share/logos-protocol/source

  mkstub "$store/q-logos-protocol-lib-0.12.0
$store/o-logos-protocol-plain-lib-0.12.0" ""
  expect_fail "Qt and plain runtimes from different sources" "different sources"

  echo "=== and the coherent cases still pass ==="
  mkstub "/nix/store/a-logos-protocol-lib-0.9.0
/nix/store/b-logos-qt-host-0.1.0" "/nix/store/a-logos-protocol-lib-0.9.0"
  PATH="$PWD/stub:$PATH" "$check" "$subject" | sed 's/^/    /'

  mkstub "$store/p-logos-protocol-plain-lib-0.12.0" ""
  PATH="$PWD/stub:$PATH" "$check" "$subject" | sed 's/^/    /'

  mkstub "$store/q-logos-protocol-lib-0.12.0
$store/p-logos-protocol-plain-lib-0.12.0" ""
  PATH="$PWD/stub:$PATH" "$check" "$subject" | sed 's/^/    /'

  mkdir -p $out
  echo passed > $out/results.txt
''
