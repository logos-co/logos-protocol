# Common build configuration shared across all packages
{ pkgs }:

let
  # wrapQtAppsNoGuiHook does not even EVALUATE for a mingw host, and would be
  # inert anyway: wrap-qt-apps-hook.sh skips anything that is not ELF or
  # Mach-O, so a PE is never wrapped. qtbase's own setup hook then hard-errors
  # unless dontWrapQtApps is set -- hence both halves of this, not just one.
  isWindows = pkgs.stdenv.hostPlatform.isWindows;
in
{
  pname = "logos-protocol";
  inherit isWindows;
  version = "0.2.0";

  # Common native build inputs
  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config
  ]
  ++ pkgs.lib.optional (!isWindows) pkgs.qt6.wrapQtAppsNoGuiHook;

  # Common runtime dependencies. Qt is an implementation detail of the
  # qt_remote (QRO) / qt_local transports — the public C ABI is Qt-free.
  buildInputs = [
    pkgs.qt6.qtbase
    pkgs.qt6.qtremoteobjects
    pkgs.boost                # Boost.Asio for plain-C++ TCP transports
    pkgs.openssl              # TLS for TcpSsl
    pkgs.nlohmann_json        # JSON data model of the C ABI + wire codec
  ];

  # Subset of buildInputs that is safe to propagate to downstream
  # consumers via `propagatedBuildInputs`. Excludes Qt: qtbase's
  # setup-hook fires `qtPreHook` which errors unless `wrapQtAppsHook`
  # (or the no-GUI variant) was sourced first, and propagation order
  # through nixpkgs can't reliably guarantee that ordering. So
  # consumers must list `qtbase` + `wrapQtAppsNoGuiHook` themselves;
  # this set carries the rest (OpenSSL, Boost, nlohmann_json) so they
  # don't have to retype it just to satisfy `find_dependency(...)`
  # inside the protocol's CMake Config.
  propagatedBuildInputs = [
    pkgs.boost
    pkgs.openssl
    pkgs.nlohmann_json
  ];

  # Common CMake flags
  cmakeFlags = [ "-GNinja" ];

  # Metadata
  meta = with pkgs.lib; {
    description = "Logos protocol — transports, token exchange and the language-neutral lp_* C ABI";
    platforms = platforms.unix ++ platforms.windows;
  };
}
