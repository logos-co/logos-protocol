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

    # This derivation sets dontUseCmakeConfigure and invokes cmake by hand, so
    # nixpkgs' cmakeConfigurePhase never runs -- and with it the step that
    # gives cmake a CMAKE_PREFIX_PATH covering the buildInputs. That goes
    # unnoticed natively but breaks the cross build: nixpkgs puts every Qt
    # module in its own store path, Qt6Config resolves components through
    # CMAKE_PREFIX_PATH, and with it empty `find_package(Qt6 COMPONENTS
    # RemoteObjects)` looks only under qtbase's own prefix and fails with
    # "Expected Config file at <qtbase>/lib/cmake/Qt6RemoteObjects ... does
    # NOT exist".
    #
    # nixpkgs does populate QT_ADDITIONAL_PACKAGES_PREFIX_PATH with each Qt
    # module's prefix (this Qt's Qt6Config does not read that variable), so
    # reuse it. Colon-separated in the environment, semicolon-separated as a
    # CMake list.
    qtPrefixes="$(printf '%s' "''${CMAKE_PREFIX_PATH-}:''${QT_ADDITIONAL_PACKAGES_PREFIX_PATH-}" \
        | tr ':' ';' | sed 's/^;*//; s/;*$//; s/;;*/;/g')"
    cmake ../cpp -GNinja -DCMAKE_INSTALL_PREFIX=$out \
        ''${qtPrefixes:+-DCMAKE_PREFIX_PATH="$qtPrefixes"} $cmakeFlags
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
