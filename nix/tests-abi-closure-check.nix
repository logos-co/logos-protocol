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

  echo "=== and the coherent case still passes ==="
  mkstub "/nix/store/a-logos-protocol-lib-0.9.0
/nix/store/b-logos-qt-host-0.1.0" "/nix/store/a-logos-protocol-lib-0.9.0"
  PATH="$PWD/stub:$PATH" "$check" "$subject" | sed 's/^/    /'

  mkdir -p $out
  echo passed > $out/results.txt
''
