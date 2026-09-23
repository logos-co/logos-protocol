# Cross-builds the Qt-free test executables for Windows, the configuration
# Windows modules run (LOGOS_PROTOCOL_BUILD_QT=OFF). They cannot run on the
# build machine, so nothing is discovered or run here; a Windows box runs them.
{ pkgs, common, src }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-tests";
  version = common.version;

  inherit src;

  # No Qt at all, so the build proves this configuration needs none.
  nativeBuildInputs = common.nativeBuildInputs;
  buildInputs = common.propagatedBuildInputs ++ [ pkgs.gtest ];
  cmakeDir = "../tests";
  cmakeFlags = common.cmakeFlags ++ [
    "-DLOGOS_PROTOCOL_BUILD_QT=OFF"
    "-DCMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE=PRE_TEST"
  ];

  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin
    cp protocol/qt_remote_plain_wire_tests.exe protocol/qt_remote_plain_cabi_tests.exe $out/bin/
    runHook postInstall
  '';

  inherit (common) meta;
}
