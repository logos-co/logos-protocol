# The qt-host/protocol pairing rule, published as an executable.
#
# This repo defines the LogosAPIClient constructor; logos-qt-host bakes the
# object's size into its own `operator new`. Neither repo can check the pairing
# alone -- only a consumer's finished closure holds both -- so the rule is
# shipped from here, where the ABI is owned, for consumers to run against their
# own build. Same division as nix/module-impl-abi.nix.
{ pkgs, common, src }:

pkgs.runCommand "logos-abi-closure-check"
{
  nativeBuildInputs = [ pkgs.makeWrapper ];
  meta = common.meta or { };
}
''
  set -euo pipefail
  mkdir -p $out/bin
  install -m755 ${src}/nix/abi-closure-check/check.sh $out/bin/logos-abi-closure-check
  # Absolute interpreter: the Linux build sandbox has no /usr/bin/env.
  patchShebangs $out/bin
  wrapProgram $out/bin/logos-abi-closure-check \
    --prefix PATH : ${pkgs.lib.makeBinPath [ pkgs.coreutils pkgs.gnugrep pkgs.gnused ]}
''
