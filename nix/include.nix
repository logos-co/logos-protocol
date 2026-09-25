# Installs the logos-protocol headers AND sources in the source-export
# layout ($out/include/cpp/...). This mirrors the layout logos-cpp-sdk has
# always shipped: logos-cpp-sdk's own include output re-exports these files
# at the exact paths downstream build systems (logos-plugin-qt's
# LogosModule.cmake source layout, etc.) already reference, which is what
# keeps the cpp-sdk artifact byte-compatible while the sources live here.
{ pkgs, common, src }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-headers";
  version = common.version;

  inherit src;
  inherit (common) meta;

  dontBuild = true;
  dontConfigure = true;

  installPhase = ''
    runHook preInstall

    mkdir -p $out/include/cpp/implementations/qt_local
    mkdir -p $out/include/cpp/implementations/qt_remote
    mkdir -p $out/include/cpp/implementations/qt_remote_plain
    mkdir -p $out/include/cpp/implementations/mock
    mkdir -p $out/include/cpp/implementations/plain
    mkdir -p $out/include/cpp/implementations/plain_local

    # Top-level headers + sources (source-export layout)
    for file in cpp/*.h cpp/*.cpp; do
      cp "$file" $out/include/cpp/
    done

    for dir in qt_local qt_remote qt_remote_plain mock plain plain_local; do
      for file in cpp/implementations/$dir/*; do
        cp "$file" $out/include/cpp/implementations/$dir/
      done
    done

    # Convenience copies at include/ root (matches logos-cpp-sdk shipping
    # logos_mode.h at $out/include/).
    cp cpp/logos_protocol.h $out/include/
    cp cpp/logos_mode.h $out/include/

    runHook postInstall
  '';
}
