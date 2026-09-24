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

  # $1 = the subject's closure, $2 = the qt-host's closure, $3 = a file holding
  # its build graph, as `nix derivation show -r` prints it (none by default).
  mkstub() {
    mkdir -p stub
    { echo '#!${pkgs.bash}/bin/bash'
      echo 'p="''${!#}"'
      echo "graph=''${3:-}"
      echo 'if [ "$1 $2" = "derivation show" ]; then [ -z "$graph" ] || cat "$graph"; exit 0; fi'
      echo 'case "$p" in'
      echo "  *SUBJECT*) printf '%s' \"$1\" ;;"
      echo "  *logos-qt-host*) printf '%s' \"$2\" ;;"
      echo '  *) ;;'
      echo 'esac'
    } > stub/nix
    { echo '#!${pkgs.bash}/bin/bash'
      echo "graph=''${3:-}"
      echo '[ -z "$graph" ] || echo /nix/store/ffffffffffffffffffffffffffffffff-subject.drv'
    } > stub/nix-store
    chmod +x stub/nix stub/nix-store
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

  # Static links: nothing in the runtime closure, the pairing only in the
  # build graph. A host binary takes the protocol-lib through an aggregate and
  # a qt-host built against $1.
  graph() {
    cat <<JSON
  {"version": 4, "derivations": {
    "ffffffffffffffffffffffffffffffff-subject.drv": {"inputs": {"drvs": {"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee-logos-host-bin.drv": {"outputs": ["out"]}}}},
    "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee-logos-host-bin.drv": {"inputs": {"drvs": {
      "dddddddddddddddddddddddddddddddd-logos-protocol.drv": {"outputs": ["out"]},
      "cccccccccccccccccccccccccccccccc-logos-qt-host-0.1.0.drv": {"outputs": ["out"]}}}},
    "dddddddddddddddddddddddddddddddd-logos-protocol.drv": {"inputs": {"drvs": {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-logos-protocol-lib-0.12.0.drv": {"outputs": ["out"]}}}},
    "cccccccccccccccccccccccccccccccc-logos-qt-host-0.1.0.drv": {"inputs": {"drvs": {"$1": {"outputs": ["out"]}}}},
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-logos-protocol-lib-0.12.0.drv": {"inputs": {"drvs": {}}},
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb-logos-protocol-lib-0.9.0.drv": {"inputs": {"drvs": {}}}}}
JSON
  }
  graph bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb-logos-protocol-lib-0.9.0.drv > static-mispaired.json
  graph aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-logos-protocol-lib-0.12.0.drv > static-paired.json
  # The same graph as Nix before 2.33 prints it: store paths, inputDrvs.
  cat > static-paired-old.json <<'JSON'
  {"/nix/store/ffffffffffffffffffffffffffffffff-subject.drv": {"inputDrvs": {"/nix/store/eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee-logos-host-bin.drv": ["out"]}},
   "/nix/store/eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee-logos-host-bin.drv": {"inputDrvs": {
     "/nix/store/dddddddddddddddddddddddddddddddd-logos-protocol.drv": ["out"],
     "/nix/store/cccccccccccccccccccccccccccccccc-logos-qt-host-0.1.0.drv": ["out"]}},
   "/nix/store/dddddddddddddddddddddddddddddddd-logos-protocol.drv": {"inputDrvs": {"/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-logos-protocol-lib-0.12.0.drv": ["out"]}},
   "/nix/store/cccccccccccccccccccccccccccccccc-logos-qt-host-0.1.0.drv": {"inputDrvs": {"/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-logos-protocol-lib-0.12.0.drv": ["out"]}},
   "/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-logos-protocol-lib-0.12.0.drv": {"inputDrvs": {}}}
JSON
  cat > static-none.json <<'JSON'
  {"version": 4, "derivations": {
    "ffffffffffffffffffffffffffffffff-subject.drv": {"inputs": {"drvs": {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-logos-protocol-lib-0.12.0.drv": {"outputs": ["out"]}}}},
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-logos-protocol-lib-0.12.0.drv": {"inputs": {"drvs": {}}}}}
JSON

  mkstub "" "" "$PWD/static-mispaired.json"
  expect_fail "static link to a qt-host built against another protocol-lib" "statically linked"

  mkstub "" "" "$PWD/static-none.json"
  expect_fail "a build graph with no qt-host link is not a pass" "inspected nothing"

  echo "=== and the coherent cases still pass ==="
  mkstub "/nix/store/a-logos-protocol-lib-0.9.0
/nix/store/b-logos-qt-host-0.1.0" "/nix/store/a-logos-protocol-lib-0.9.0"
  PATH="$PWD/stub:$PATH" "$check" "$subject" | sed 's/^/    /'

  mkstub "$store/p-logos-protocol-plain-lib-0.12.0" ""
  PATH="$PWD/stub:$PATH" "$check" "$subject" | sed 's/^/    /'

  mkstub "$store/q-logos-protocol-lib-0.12.0
$store/p-logos-protocol-plain-lib-0.12.0" ""
  PATH="$PWD/stub:$PATH" "$check" "$subject" | sed 's/^/    /'

  mkstub "" "" "$PWD/static-paired.json"
  PATH="$PWD/stub:$PATH" "$check" "$subject" | sed 's/^/    /'

  mkstub "" "" "$PWD/static-paired-old.json"
  PATH="$PWD/stub:$PATH" "$check" "$subject" | sed 's/^/    /'

  mkdir -p $out
  echo passed > $out/results.txt
''
