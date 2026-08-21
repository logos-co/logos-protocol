# The module-impl C ABI, published as data.
#
# logos-protocol DECLARES this ABI; every language backend (logos-cpp-sdk,
# logos-rust-sdk, and any future one) must DEFINE every entry. Those are
# independent facts, and the gap between them has shipped twice —
# grant_host_services at protocol 0.3, the teardown pair at 0.5 — each time
# surfacing three repos downstream as an "undefined symbol" at dlopen, on Linux
# only. See nix/module-impl-abi/diff-exports.sh for why it hides so well.
#
# So the list is derived here, once, by the repo that owns the ABI, and shipped
# as a build output for the backends to check themselves against. Two properties
# fall out of putting it here rather than in each backend:
#
#   * There is ONE parser to keep working. A per-backend regex over the header
#     is a per-backend way to silently stop matching.
#   * The list is VERSION-CORRECT by construction, with no version arithmetic
#     anywhere. The header is itself versioned — at protocol 0.4 it declared
#     eight exports, at 0.5 it declares ten — so "what this protocol requires"
#     is just "what this header declares". A consumer that pins protocol 0.4
#     reads a list of eight and is right to define eight. No @since tags, no
#     MINOR comparisons, nothing for a backend to get wrong.
{ pkgs, common, src }:

pkgs.runCommand "logos-module-impl-abi"
{
  nativeBuildInputs = [ pkgs.gawk pkgs.gnugrep pkgs.coreutils ];
  meta = common.meta or { };
}
''
  set -euo pipefail
  hdr=${src}/cpp/logos_module_impl.h
  ver=${src}/cpp/logos_protocol.h

  mkdir -p $out/bin

  # Fails the build — deliberately, and here rather than downstream — if the
  # header can no longer be parsed. logos-protocol not building is the correct
  # consequence of logos-protocol being unable to state its own ABI.
  bash ${src}/nix/module-impl-abi/extract-exports.sh "$hdr" > $out/exports.txt

  # The version the same header belongs to. Consumers that must pass a protocol
  # version to their generator take it from HERE, so the list and the version
  # can never come from two different revisions.
  sed -n 's/^#define LOGOS_PROTOCOL_VERSION_STRING[[:space:]]*"\(.*\)".*/\1/p' \
      "$ver" > $out/version
  test -s $out/version || {
    echo "module-impl-abi: could not read LOGOS_PROTOCOL_VERSION_STRING from $ver" >&2
    exit 1
  }

  install -m555 ${src}/nix/module-impl-abi/diff-exports.sh \
                $out/bin/logos-module-impl-diff

  echo "module-impl C ABI $(cat $out/version): $(wc -l < $out/exports.txt) declared exports"
  sed 's/^/  /' $out/exports.txt
''
