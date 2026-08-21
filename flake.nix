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
          include = import ./nix/include.nix { inherit pkgs common src; };
          tests = import ./nix/tests.nix { inherit pkgs common src; };

          # The module-impl C ABI as data, for the language backends to check
          # themselves against. See nix/module-impl-abi.nix.
          module-impl-abi = import ./nix/module-impl-abi.nix { inherit pkgs common src; };

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
        in
        {
          logos-protocol-lib = lib;
          logos-protocol-include = include;
          inherit tests module-impl-abi;

          logos-protocol = protocol;
          default = protocol;
        }
      );

      checks = forAllSystems ({ pkgs, ... }:
        let
          common = import ./nix/default.nix { inherit pkgs; };
          src = ./.;
          tests = import ./nix/tests.nix { inherit pkgs common src; };
          module-impl-abi = import ./nix/module-impl-abi.nix { inherit pkgs common src; };
        in
        {
          inherit tests;
          # Proves the ABI manifest every backend checks itself against can
          # still fail. See nix/tests-module-impl-abi.nix.
          module-impl-abi-tests = import ./nix/tests-module-impl-abi.nix {
            inherit pkgs common src module-impl-abi;
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
