# The Qt adapters cross-built for Windows: only the resolution test, which checks
# that every local transport there resolves to the plain implementation.
{ pkgs, common, src }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-tests-qt";
  version = common.version;

  inherit src;

  # qtbase's setup hook refuses a mingw host without this.
  dontWrapQtApps = true;

  nativeBuildInputs = common.nativeBuildInputs;
  buildInputs = common.buildInputs ++ [ pkgs.gtest ];
  cmakeDir = "../tests";
  cmakeFlags = common.cmakeFlags ++ [
    "-DCMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE=PRE_TEST"
  ];
  ninjaFlags = [ "transport_resolution_tests" ];

  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin
    cp protocol/transport_resolution_tests.exe $out/bin/
    runHook postInstall
  '';

  inherit (common) meta;
}
