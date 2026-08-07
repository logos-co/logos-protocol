#ifndef LOGOS_SHARED_API_H
#define LOGOS_SHARED_API_H

/**
 * @file logos_shared_api.h
 * @brief Marks the runtime types that must exist EXACTLY ONCE per process.
 *
 * ELF and Mach-O give this for free. Both formats interpose symbols across the
 * whole process image set, so when liblogos_core.{so,dylib} exports
 * TokenManager::instance() every other image in the process — the host binary,
 * the UI plugin — binds to that one definition and the function-local
 * `static TokenManager instance;` inside it is genuinely a singleton.
 *
 * PE has no interposition. A symbol is either in a DLL's export table and
 * reached through an import thunk, or it is resolved image-locally; there is no
 * "first definition wins across the process" rule. So on Windows every image
 * that links liblogos_protocol.a / liblogos_qt_sdk.a statically gets its OWN
 * copy of the code, and therefore its own copy of every function-local static
 * inside it. Measured on the Basecamp payload: nine images each define
 * TokenManager::instance()::instance. The host saves a capability token into
 * its copy, the UI plugin reads its own empty copy, and every cross-module call
 * is refused ("ModuleProxy: rejecting unauthorized call") — the package manager
 * never appears in the sidebar.
 *
 * The obvious fixes are both wrong, and the wrongness is not obvious, so:
 *
 *   - Exporting everything from liblogos_core (-Wl,--export-all-symbols) makes
 *     its import library a second definition of symbols that liblogos_qt_sdk.a
 *     also defines, and the link dies with "multiple definition of
 *     `LogosAPI::LogosAPI'". See the note in logos-liblogos/src/CMakeLists.txt.
 *   - Exporting nothing (today's C-API-only narrowing) links, and silently
 *     gives every image its own statics. That is the bug above.
 *
 * The resolution is ONE PROVIDER: liblogos_core.dll exports these types
 * explicitly (a generated .def, see logos-liblogos/cmake/gen-shared-exports.sh)
 * and the in-process consumers — LogosBasecamp.exe and main_ui.dll — compile
 * with LOGOS_SHARED_USE_DLL so their references become `__declspec(dllimport)`.
 *
 * The dllimport is the load-bearing half, not the export. It rewrites the
 * reference to go through `__imp_`, so the plain symbol is never undefined and
 * GNU ld never pulls the archive member that would have redefined it —
 * regardless of where the static archive sits on the link line. Without it the
 * link still succeeds and binds to the archive, with no diagnostic at all.
 *
 * Everything here is deliberately a no-op unless a consumer opts in:
 *
 *   - Off Windows the macro is empty; ELF/Mach-O already do the right thing.
 *   - Inside logos-protocol / logos-qt-sdk / liblogos_core the macro is empty,
 *     so the static archives are compiled byte-identically to before and the
 *     export side is driven purely by the .def at liblogos_core's link.
 *   - logos_host.exe, ui-host.exe and the module plugin DLLs do not define
 *     LOGOS_SHARED_USE_DLL. They are separate processes that do not load
 *     liblogos_core, so they keep their own (correct, per-process) statics.
 */
#if defined(_WIN32) && defined(LOGOS_SHARED_USE_DLL)
#  define LOGOS_SHARED_API __declspec(dllimport)
#else
#  define LOGOS_SHARED_API
#endif

#endif // LOGOS_SHARED_API_H
