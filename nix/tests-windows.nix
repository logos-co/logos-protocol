# Cross-builds the Qt-free test executables for Windows, the configuration
# Windows modules run (LOGOS_PROTOCOL_BUILD_QT=OFF). They cannot run on the
# build machine, so nothing is discovered or run here; a Windows box runs them.
{ pkgs, common, src }:

let
  # What Windows CI runs, with the per-case deadlines tests/protocol/CMakeLists.txt gives ctest.
  manifest = builtins.toFile "protocol-plain-tests.json" (builtins.toJSON {
    suites = [
      { name = "wire"; exe = "bin/qt_remote_plain_wire_tests.exe"; }
      { name = "cabi"; exe = "bin/qt_remote_plain_cabi_tests.exe"; timeout = 8; }
      { name = "cabi_shared"; exe = "bin/qt_remote_plain_cabi_shared_tests.exe"; timeout = 8; }
    ];
  });
in
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
    mkdir -p $out/bin $out/share/logos-tests
    cp protocol/qt_remote_plain_wire_tests.exe protocol/qt_remote_plain_cabi_tests.exe \
       protocol/qt_remote_plain_cabi_shared_tests.exe bin/liblogos_protocol_plain.dll $out/bin/
    cp ${manifest} $out/share/logos-tests/protocol-plain.json
    runHook postInstall
  '';

  inherit (common) meta;
}
