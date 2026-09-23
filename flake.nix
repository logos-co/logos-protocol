{
  description = "Logos Protocol - transports, token exchange and the language-neutral lp_* C ABI";

  inputs.logos-nix.url = "github:logos-co/logos-nix";
  inputs.nixpkgs.follows = "logos-nix/nixpkgs";

  outputs = { self, nixpkgs, logos-nix }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = import nixpkgs { inherit system; };
      });

      # Adds the "x86_64-windows" pseudo-system. A cross derivation's `system`
      # attribute is its BUILD platform, so packages.x86_64-windows.* evaluates
      # anywhere but realises on x86_64-linux.
      forAllTargets = logos-nix.lib.forAllTargets;
    in
    {
      packages = forAllTargets ({ pkgs, ... }:
        let
          common = import ./nix/default.nix { inherit pkgs; };
          src = ./.;

          lib = import ./nix/lib.nix { inherit pkgs common src; };
          plain-lib = import ./nix/plain-lib.nix { inherit pkgs common src; };
          include = import ./nix/include.nix { inherit pkgs common src; };
          tests = import (if common.isWindows then ./nix/tests-windows.nix else ./nix/tests.nix) {
            inherit pkgs common src;
          };

          # The module-impl C ABI as data, for the language backends to check
          # themselves against. See nix/module-impl-abi.nix.
          module-impl-abi = import ./nix/module-impl-abi.nix { inherit pkgs common src; };

          # The qt-host/protocol pairing rule, for consumers to run against
          # their own closure. See nix/abi-closure-check.nix.
          abi-closure-check = import ./nix/abi-closure-check.nix { inherit pkgs common src; };

          # Combined package: static lib + cmake config + source-export
          # headers. propagatedBuildInputs re-declared on the join because
          # symlinkJoin doesn't forward propagation from `paths`. Qt is
          # excluded for the same setup-hook ordering reason as in
          # nix/default.nix; consumers list qt6.qtbase +
          # qt6.wrapQtAppsNoGuiHook themselves.
          protocol = pkgs.symlinkJoin {
            name = "logos-protocol";
            paths = [ lib include ];
            propagatedBuildInputs = common.propagatedBuildInputs;
          };
          plain-protocol = pkgs.symlinkJoin {
            name = "logos-protocol-plain";
            paths = [ plain-lib include ];
            propagatedBuildInputs = [ pkgs.boost pkgs.openssl pkgs.nlohmann_json ];
          };
        in
        {
          logos-protocol-lib = lib;
          logos-protocol-plain-lib = plain-lib;
          logos-protocol-plain = plain-protocol;
          logos-protocol-include = include;
          inherit tests module-impl-abi abi-closure-check;

          logos-protocol = protocol;
          default = protocol;
        } // pkgs.lib.optionalAttrs common.isWindows {
          tests-qt = import ./nix/tests-windows-qt.nix { inherit pkgs common src; };
        }
      );

      checks = forAllSystems ({ pkgs, ... }:
        let
          common = import ./nix/default.nix { inherit pkgs; };
          src = ./.;
          tests = import ./nix/tests.nix { inherit pkgs common src; };
          module-impl-abi = import ./nix/module-impl-abi.nix { inherit pkgs common src; };
          abi-closure-check = import ./nix/abi-closure-check.nix { inherit pkgs common src; };
          lib = import ./nix/lib.nix { inherit pkgs common src; };
          plain-lib = import ./nix/plain-lib.nix { inherit pkgs common src; };
        in
        {
          inherit tests;

          # The configuration Qt-free consumers build, built with no Qt.
          inherit plain-lib;

          # One shared plain runtime, shipped by the Qt-free output: two builds
          # under one name let a process load both.
          plain-runtime-once = pkgs.runCommand "logos-protocol-plain-runtime-once" { } ''
            for f in ${lib}/lib/liblogos_protocol_plain.so ${lib}/lib/liblogos_protocol_plain.dylib \
                     ${lib}/bin/liblogos_protocol_plain.dll; do
              if [ -e "$f" ]; then echo "the Qt output ships $f" >&2; exit 1; fi
            done
            shared=0
            for f in ${plain-lib}/lib/liblogos_protocol_plain.so ${plain-lib}/lib/liblogos_protocol_plain.dylib \
                     ${plain-lib}/bin/liblogos_protocol_plain.dll; do
              [ -e "$f" ] && shared=1
            done
            [ "$shared" -eq 1 ] || { echo "the Qt-free output ships no shared plain runtime" >&2; exit 1; }
            [ "$(cat ${lib}/share/logos-protocol/source)" = "$(cat ${plain-lib}/share/logos-protocol/source)" ]
            touch $out
          '';

          # Proves the ABI manifest every backend checks itself against can
          # still fail. See nix/tests-module-impl-abi.nix.
          module-impl-abi-tests = import ./nix/tests-module-impl-abi.nix {
            inherit pkgs common src module-impl-abi;
          };

          # Proves the qt-host/protocol pairing rule can still fail.
          # See nix/tests-abi-closure-check.nix.
          abi-closure-check-tests = import ./nix/tests-abi-closure-check.nix {
            inherit pkgs common src abi-closure-check;
          };
        }
      );

      devShells = forAllSystems ({ pkgs, ... }: {
        default = pkgs.mkShell {
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
          ];
          buildInputs = [
            pkgs.qt6.qtbase
            pkgs.qt6.qtremoteobjects
            pkgs.gtest
            pkgs.boost
            pkgs.openssl
            pkgs.nlohmann_json
          ];
        };
      });
    };
}
