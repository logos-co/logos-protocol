# Does the ABI manifest actually notice when something is wrong?
#
# A check that cannot fail is worse than no check: it reports "green" over an
# unexamined ABI and buys false confidence. The manifest in nix/module-impl-abi.nix
# is consumed by every language backend, so its failure modes are asserted here,
# in the repo that owns it, rather than trusted.
#
# Each case below is a way the extractor or the diff could silently degrade into
# a pass. They are the controls for exactly those, not decoration.
{ pkgs, common, src, module-impl-abi }:

pkgs.runCommand "logos-protocol-module-impl-abi-tests"
{
  nativeBuildInputs = [ pkgs.gawk pkgs.gnugrep pkgs.coreutils pkgs.diffutils ];
  meta = common.meta or { };
}
''
  set -uo pipefail
  X=${src}/nix/module-impl-abi/extract-exports.sh
  D=${module-impl-abi}/bin/logos-module-impl-diff
  H=${src}/cpp/logos_module_impl.h
  fails=0

  ok()   { echo "  PASS  $1"; }
  bad()  { echo "  FAIL  $1"; fails=$((fails + 1)); }

  # Expect a DELIBERATE refusal — exit 1, and specifically not 126/127.
  #
  # "Any non-zero exit counts as a refusal" is how a suite like this goes
  # vacuous: a script that cannot be executed at all exits 126 or 127, and every
  # negative case then reports PASS while proving nothing. This file caught
  # exactly that in its own first version — the helper's `#!/usr/bin/env bash`
  # did not resolve inside the Linux sandbox, so all five refusal cases "passed"
  # and only the two positive cases exposed it.
  expect_fail() { local what="$1"; shift
    local rc=0; "$@" >/dev/null 2>&1 || rc=$?
    case "$rc" in
      0)       bad "$what (exited 0; it must refuse)" ;;
      126|127) bad "$what (exit $rc — could not execute; this is NOT a refusal)" ;;
      *)       ok "$what" ;;
    esac; }
  expect_pass() { local what="$1"; shift
    local rc=0; "$@" >/dev/null 2>&1 || rc=$?
    if [ "$rc" -eq 0 ]; then ok "$what"; else bad "$what (exit $rc)"; fi; }

  echo "-- the extractor agrees with the published manifest --"
  bash "$X" "$H" > got.txt
  if diff -u ${module-impl-abi}/exports.txt got.txt > /dev/null; then
    ok "extractor output matches the shipped exports.txt"
  else
    bad "extractor drifted from the shipped exports.txt"
    diff -u ${module-impl-abi}/exports.txt got.txt || true
  fi

  echo "-- the extractor REFUSES rather than under-reporting --"
  # Under-reporting is the dangerous direction: a short list makes every
  # backend's diff pass over an ABI nobody checked.
  : > empty.h
  expect_fail "empty header is refused"        bash "$X" empty.h
  sed 's/LOGOS_MODULE_IMPL_EXPORT/LOGOS_MODULE_API/g' "$H" > renamed.h
  expect_fail "renamed export macro is refused" bash "$X" renamed.h
  expect_fail "missing file is refused"         bash "$X" /nonexistent.h
  # A founding export removed: the floor in extract-exports.sh is asserted, not
  # derived, precisely so this cannot be satisfied by the same broken parse.
  grep -v 'logos_module_string_free' "$H" > shrunk.h
  expect_fail "a founding export going missing is refused" bash "$X" shrunk.h

  echo "-- the extractor survives a reflowed declaration --"
  # A line-oriented parser loses a declaration the moment someone puts the
  # macro on its own line. That would under-report, and silently.
  awk '{ if ($0 ~ /^LOGOS_MODULE_IMPL_EXPORT[^\n]*logos_module_about_to_unload/) {
           print "LOGOS_MODULE_IMPL_EXPORT"; sub(/^LOGOS_MODULE_IMPL_EXPORT[[:space:]]*/, "");
         } print }' "$H" > reflowed.h
  bash "$X" reflowed.h > reflowed.txt 2>/dev/null || true
  if diff -u got.txt reflowed.txt > /dev/null; then
    ok "a macro on its own line still yields the full set"
  else
    bad "reflowing a declaration changed the extracted set"
    diff -u got.txt reflowed.txt || true
  fi

  echo "-- the diff helper --"
  expect_pass "identical sets pass"          "$D" got.txt got.txt "self" ""
  grep -v logos_module_about_to_unload got.txt > short.txt
  expect_fail "a backend missing one export fails" "$D" got.txt short.txt "mock" "some/emitter.rs"
  : > nothing.txt
  # If a consumer's symbol extraction silently yields nothing, `comm` reports no
  # missing symbols and the check passes. That is the vacuum this rejects.
  expect_fail "an EMPTY defined-set is refused, not treated as a match" \
              "$D" got.txt nothing.txt "mock" ""
  expect_fail "an empty declared-set is refused" "$D" nothing.txt got.txt "mock" ""
  # Extra symbols on the backend side are its own business (the Rust scaffold
  # declares an extern "Rust" install hook, for one), so the comparison is
  # one-directional and this must NOT fail.
  { cat got.txt; echo logos_module_something_extra; } | sort > extra.txt
  expect_pass "extra backend symbols are allowed" "$D" got.txt extra.txt "mock" ""

  echo
  if [ "$fails" -ne 0 ]; then echo "$fails case(s) failed"; exit 1; fi
  echo "module-impl ABI manifest: all cases passed"
  touch $out
''
