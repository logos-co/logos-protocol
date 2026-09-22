# Builds only the Qt-free qt_remote_plain wire/runtime and public C ABI.
{ pkgs, common, src }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-plain-lib";
  version = common.version;

  inherit src;
  nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config ];
  buildInputs = [ pkgs.boost pkgs.openssl pkgs.nlohmann_json ];
  propagatedBuildInputs = [ pkgs.boost pkgs.openssl pkgs.nlohmann_json ];
  inherit (common) meta;

  cmakeDir = "../cpp";
  cmakeFlags = [
    "-DLOGOS_PROTOCOL_BUILD_QT=OFF"
  ];
}
