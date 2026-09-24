# The Qt-configured suites cross-built for Windows: what Linux and macOS run as
# protocol_tests and protocol_noqt_tests, plus the check that every local
# transport there resolves to the plain implementation.
{ pkgs, common, src }:

let
  known = import ./tests-windows-skips.nix;
  skip = builtins.concatMap (cause: map (pattern: { inherit pattern; reason = known.reasons.${cause}; })
    known.${cause}) (builtins.attrNames known.reasons);
  manifest = builtins.toFile "protocol-qt-tests.json" (builtins.toJSON {
    suites = [
      { name = "protocol"; exe = "bin/protocol_tests.exe"; inherit skip; }
      { name = "noqt"; exe = "bin/protocol_noqt_tests.exe"; }
      { name = "transport_resolution"; exe = "bin/transport_resolution_tests.exe"; }
      { name = "adapter_shutdown"; exe = "bin/qt_remote_plain_regression_probe.exe";
        kind = "exe"; timeout = 8; }
    ];
  });
in
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
  ninjaFlags = [
    "protocol_tests" "protocol_noqt_tests" "transport_resolution_tests"
    "qt_remote_plain_regression_probe"
  ];

  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin $out/share/logos-tests
    cp protocol/protocol_tests.exe protocol/protocol_noqt_tests.exe \
       protocol/transport_resolution_tests.exe protocol/qt_remote_plain_regression_probe.exe $out/bin/
    cp ${manifest} $out/share/logos-tests/protocol-qt.json
    runHook postInstall
  '';

  inherit (common) meta;
}
