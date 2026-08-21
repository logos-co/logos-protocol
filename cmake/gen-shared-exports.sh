#!/bin/sh
# Generate the PE module-definition file that makes liblogos_protocol.dll the
# single provider of the shared C++ runtime (TokenManager, LogosAPIClient,
# ModuleProxy, LogosProviderObject, the transports, and the lp_* C ABI).
#
# Adapted from logos-liblogos/cmake/gen-shared-exports.sh, which generated the
# same table one layer up when liblogos_core absorbed both static archives and
# re-exported them. The migration to real shared libraries moves that
# responsibility down to the library that OWNS the symbols; the mechanism is
# unchanged because the reasons for it are unchanged. It cannot be shared as a
# file: logos-liblogos depends on logos-protocol, not the other way round.
#
# WHY a generated .def rather than __declspec(dllexport) in the headers.
# Measured, not assumed: annotating the classes by hand exported 116 symbols and
# the Qt host runtime still failed to link against it with ELEVEN undefined
# references spanning five classes -- LogosProviderObject (and its vtable),
# ModuleProxy, ModuleHandshakeProxy, LogosTransportFactory -- plus free
# functions such as logos::qvariantToNlohmann. A curated list is a moving target:
# it is correct only until the next consumer touches a symbol nobody marked, and
# the failure lands in a downstream repo, far from the cause.
#
# CMake'"'"'s WINDOWS_EXPORT_ALL_SYMBOLS is not the alternative. It goes inert the
# moment a target contains any __declspec(dllexport), so it and the annotations
# are mutually exclusive -- measured here as 116 exports both with and without
# it. Hence: the .def owns the PE export table, and logos_shared_api.h resolves
# its "building the shared library" branch to NOTHING on Windows so the two
# mechanisms never compete.
#
# WHY the whole archive and not a curated member list: ld chooses archive
# members by object file, for reasons that have nothing to do with our symbols.
# Consumers link an EMPTY stand-in archive and take everything from the DLL,
# which only works if the DLL really does provide everything. A partial export
# set turns into an undefined reference in a downstream repo.
#
# WHY COMDAT symbols are filtered out: they come from inline functions and
# templates in headers, so every consumer TU emits its own copy regardless of
# what the DLL exports. Exporting them turns the import library into a STRONG
# definition that then collides with the consumer'"'"'s own copy. They are also
# precisely the symbols an export cannot deduplicate -- a function-local static
# inside an inline function stays per-image on PE no matter what. Do not
# "simplify" this filter away.
#
# Usage: gen-shared-exports.sh <nm> <out.def> <archive> <obj,obj,...|*> [...]

set -eu

NM="$1"; shift
OUT="$1"; shift

TMP="${OUT}.tmp"
: > "$TMP"

while [ "$#" -gt 0 ]; do
    archive="$1"; members="$2"; shift 2
    [ -f "$archive" ] || { echo "gen-shared-exports: missing archive $archive" >&2; exit 1; }
    "$NM" -A --defined-only "$archive" | awk -v members="$members" -v arch="$archive" '
    BEGIN {
        if (members == "*") { all = 1 }
        else { n = split(members, a, ","); for (i = 1; i <= n; i++) want[a[i]] = 1 }
    }
    {
        # nm -A on an archive prints "<archive>:<member>:<addr> <type> <name>".
        split($1, p, ":")
        mem = p[2]; typ = $2; name = $3

        # COMDAT sections are named ".text$<mangled>" / ".rdata$<mangled>" /
        # etc. Record the mangled tail so the symbol itself can be dropped.
        if (name ~ /^\.[a-z]+\$/) {
            tail = name; sub(/^\.[a-z]+\$/, "", tail)
            # EXCEPT vtables and typeinfo (_ZTV / _ZTI / _ZTS).
            #
            # PE has no weak symbols -- COMDAT IS the mechanism for weak and
            # inline linkage -- so GCC emits a vtable into .rdata$_ZTV... even
            # when the class has a key function and the vtable is a single
            # strong definition. The section name therefore cannot distinguish
            # "one definition that nobody duplicates" from "every TU emits its
            # own", and dropping all of them drops these too.
            #
            # The reasoning above does not apply to them. A consumer of a class
            # WITH a key function does not emit a copy: it emits a .refptr, an
            # external reference, and needs ours. Measured here -- the Qt host
            # runtime emits .refptr._ZTV19LogosProviderObject and failed to link
            # with an undefined reference to the vtable for LogosProviderObject
            # while this filter dropped the only definition. A class WITHOUT a
            # key function emits its own copy in every TU and never references
            # ours, so exporting it is inert rather than colliding.
            #
            # This did not matter while liblogos_core absorbed both archives:
            # definition and consumer landed in ONE image and the reference
            # never crossed a boundary. Splitting the runtime into real shared
            # libraries is what made class vtables cross-image symbols.
            if (tail !~ /^_ZT[VIS]/) comdat[mem SUBSEP tail] = 1
            next
        }
        if (!all && !(mem in want)) next
        # T=text D=data R=rodata B=bss, all uppercase == external linkage.
        # Weak (V/W) is COMDAT by another name; lowercase is file-local and
        # cannot be exported at all (that includes the function-local statics
        # themselves — we export the ACCESSOR so callers reach ours).
        if (typ != "T" && typ != "D" && typ != "R" && typ != "B") next
        # Itanium-mangled C++ (^_Z), PLUS the logos-protocol C ABI (^lp_).
        #
        # The ^_Z test alone keeps toolchain bookkeeping such as
        # qt_version_tag_6_11_used — which every image legitimately defines —
        # out of the export table. But `lp_*` is `extern "C"`, so it is
        # UNMANGLED and the same test silently dropped the entire C ABI.
        #
        # That was invisible until B5: before it, only C++ callers reached the
        # shared runtime. B5 re-emits every Qt-typed dependency wrapper as a
        # VENEER over the lp path, so a consumer that compiles such a wrapper —
        # logos-basecamp compiles package_manager_api.cpp into its own exe —
        # now calls lp_invoke / lp_client_create / lp_token_save directly. With
        # them absent from the .def the exe cannot import them, and the link
        # fails with plain `undefined reference to 'lp_invoke'`.
        #
        # Exporting is the correct fix rather than letting the consumer link
        # liblogos_protocol.a itself: lp_token_save and friends operate on the
        # TokenManager singleton, so a static copy in the exe would reinstate
        # exactly the split-brain token store this whole .def scheme exists to
        # prevent. Module plugins are separate processes and keep their own
        # per-image copy by design (see logos_module_grant_host_services).
        #
        # The prefix is deliberately tight: `lp_` only, not "anything unmangled".
        if (name !~ /^_Z/ && name !~ /^lp_/) next
        seenmem[mem] = 1
        k++; recmem[k] = mem; rectyp[k] = typ; recnam[k] = name
    }
    END {
        if (!all) for (m in want) if (!(m in seenmem)) {
            printf("gen-shared-exports: %s has no member %s\n", arch, m) > "/dev/stderr"
            bad = 1
        }
        if (bad) exit 1
        for (i = 1; i <= k; i++) {
            if ((recmem[i] SUBSEP recnam[i]) in comdat) continue
            print recnam[i] (rectyp[i] == "T" ? "" : " DATA")
        }
    }' >> "$TMP"
done

# DATA matters: a data export reached without the DATA keyword hands the
# consumer the CONTENTS of the slot instead of its address, which corrupts at
# runtime rather than at link. The keyword is derived from nm's type letter
# above, never written by hand.
{
    echo "EXPORTS"
    sort -u "$TMP"
} > "$OUT"
rm -f "$TMP"

count=$(grep -c . "$OUT" || true)
echo "gen-shared-exports: wrote $OUT ($((count - 1)) symbols)"
