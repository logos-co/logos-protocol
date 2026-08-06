# Builds the logos-protocol static library + CMake package config
{ pkgs, common, src }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-lib";
  version = common.version;

  inherit src;
  inherit (common) nativeBuildInputs cmakeFlags meta;
  buildInputs = common.buildInputs;

  # Propagate the transitive non-Qt deps so downstream Nix derivations
  # automatically get OpenSSL / Boost / nlohmann_json in their own
  # configure-time CMAKE_PREFIX_PATH and link-time search path. Qt is
  # intentionally excluded — see the comment in nix/default.nix.
  propagatedBuildInputs = common.propagatedBuildInputs;

  dontUseCmakeConfigure = true;
  # Required whenever the Qt wrapper hooks are absent (Windows) -- qtbase's
  # setup hook errors out in qtPreHook otherwise.
  dontWrapQtApps = true;

  buildPhase = ''
    runHook preBuild

    mkdir -p build-protocol
    cd build-protocol
    cmake ../cpp -GNinja -DCMAKE_INSTALL_PREFIX=$out $cmakeFlags
    ninja
    cd ..

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall

    # Run cmake's install rules so the EXPORT set + generated
    # logos-protocolConfig.cmake / Targets.cmake land under
    # $out/lib/cmake/logos-protocol/. This is what makes
    # `find_package(logos-protocol)` work in consumers.
    cmake --install build-protocol

    runHook postInstall
  '';
}
