#ifndef LOGOS_SHARED_API_H
#define LOGOS_SHARED_API_H

/**
 * @file logos_shared_api.h
 * @brief Marks the runtime types that must exist EXACTLY ONCE per process.
 *
 * Every image that links liblogos_protocol.a / liblogos_qt_host.a statically
 * gets its OWN copy of the code, and therefore its own copy of every
 * function-local static inside it: TokenManager::instance, the per-identity
 * StoreRegistry, the host-services grant, the deferred event-subscription
 * registry. The host saves a capability token into its copy, another in-process
 * image reads its own empty copy, and every cross-module call is refused
 * ("ModuleProxy: rejecting unauthorized call") — the package manager never
 * appears in the sidebar.
 *
 * WHICH PLATFORMS. This was long documented as Windows-only, on the premise
 * that "ELF and Mach-O interpose symbols across the whole process image set".
 * That is true of ELF and FALSE of Mach-O, and the false half was measured:
 *
 *   - PE     no interposition at all. A symbol is either in a DLL's export
 *            table and reached through an import thunk, or it is resolved
 *            image-locally. Measured on the Basecamp payload: NINE images each
 *            defining TokenManager::instance()::instance.
 *   - Mach-O two-level namespace, so it behaves like PE, not like ELF. It
 *            appears to work only while the consumer image has NO definition of
 *            its own, so ld binds the undefined symbol to the provider. The
 *            moment any reference drags an archive member in, that image gets
 *            its own copy, silently. Measured in logos-basecamp: ONE reference
 *            to LogosAPI::forIdentity pulled logos_api.cpp.o and
 *            token_manager.cpp.o into the executable, which then produced 31
 *            refused calls against a baseline of 0.
 *   - ELF    flat namespace, first definition wins process-wide. This one
 *            genuinely does collapse duplicates.
 *
 * The consumers empty their static archives on every platform anyway
 * (logos-basecamp/cmake/LogosSharedFromDll.cmake), so the invariant is ONE rule
 * everywhere rather than three — and nix/symbol-gate.nix can assert it
 * uniformly instead of encoding a per-platform exception.
 *
 * The obvious fixes are both wrong, and the wrongness is not obvious, so:
 *
 *   - Exporting everything from liblogos_core (-Wl,--export-all-symbols) makes
 *     its import library a second definition of symbols that the static
 *     archives also define, and the link dies with "multiple definition of
 *     `LogosAPI::LogosAPI'". See the note in logos-liblogos/src/CMakeLists.txt.
 *   - Exporting nothing (a C-API-only narrowing) links, and silently gives
 *     every image its own statics. That is the bug above.
 *
 * The resolution is ONE PROVIDER, and the macro below is how a symbol is
 * assigned to one. On Windows the in-process consumers additionally compile
 * with LOGOS_SHARED_USE_DLL so their references become __declspec(dllimport).
 *
 * The dllimport is the load-bearing half, not the export. It rewrites the
 * reference to go through `__imp_`, so the plain symbol is never undefined and
 * GNU ld never pulls the archive member that would have redefined it —
 * regardless of where the static archive sits on the link line. Without it the
 * link still succeeds and binds to the archive, with no diagnostic at all.
 *
 * Note that logos_host, ui-host and the module plugins do NOT opt in. They are
 * separate processes that do not load the provider, so they keep their own —
 * correct, per-process — statics.
 */

/* The primitives. Kept separate so the per-library macros below read as a
 * three-state choice (export / import / neither) rather than as nested #ifdefs.
 * Off Windows there is nothing to import: a shared library exports its
 * non-hidden symbols by default, and consumers just reference them. */
#if defined(_WIN32)
#  define LOGOS_SHARED_EXPORT __declspec(dllexport)
#  define LOGOS_SHARED_IMPORT __declspec(dllimport)
#else
#  define LOGOS_SHARED_EXPORT __attribute__((visibility("default")))
#  define LOGOS_SHARED_IMPORT
#endif

/* logos-protocol's own single-instance types: TokenManager, LogosAPIClient,
 * and the LogosResult stream operators.
 *
 * EXPORT while building the shared library that owns them, IMPORT while
 * consuming that library, and EMPTY for the static archive — which is what
 * every current consumer gets, so this is a no-op until a build opts in. */
#if defined(LOGOS_PROTOCOL_BUILDING_SHARED)
/* Building the library that owns the symbol.
 *
 * On Windows this resolves to NOTHING, and that is deliberate: the PE export
 * table is generated from the objects (cmake/gen-shared-exports.sh), because a
 * hand-marked list is a moving target -- it exported 116 symbols and the Qt host
 * runtime still failed to link with eleven undefined references across five
 * classes. Marking here as well would put two mechanisms on the same table for
 * no gain, and __declspec(dllexport) additionally makes CMake's
 * WINDOWS_EXPORT_ALL_SYMBOLS inert, so the annotation actively forecloses the
 * automatic route.
 *
 * Off Windows the annotation is what a hidden-visibility build would need, and
 * is harmless today because nothing sets -fvisibility=hidden. */
#  if defined(_WIN32)
#    define LOGOS_SHARED_API
#  else
#    define LOGOS_SHARED_API LOGOS_SHARED_EXPORT
#  endif
#elif defined(_WIN32) && defined(LOGOS_SHARED_USE_DLL)
#  define LOGOS_SHARED_API LOGOS_SHARED_IMPORT
#else
#  define LOGOS_SHARED_API
#endif

/* logos-plugin-qt's LogosAPI, which lives in a DIFFERENT library.
 *
 * It needs its own macro rather than reusing LOGOS_SHARED_API, because the two
 * are not the same choice in the same translation unit: while building the Qt
 * host runtime shared library, LogosAPI must be EXPORTED while TokenManager —
 * owned by logos-protocol — must be IMPORTED. One macro cannot say both, and on
 * PE getting it wrong means the type is defined twice in the process.
 *
 * Off Windows this distinction is moot (both resolve to default visibility),
 * which is exactly why it would go unnoticed until a Windows build. */
#if defined(LOGOS_QT_HOST_BUILDING_SHARED)
/* Building the library that owns the symbol.
 *
 * On Windows this resolves to NOTHING, and that is deliberate: the PE export
 * table is generated from the objects (cmake/gen-shared-exports.sh), because a
 * hand-marked list is a moving target -- it exported 116 symbols and the Qt host
 * runtime still failed to link with eleven undefined references across five
 * classes. Marking here as well would put two mechanisms on the same table for
 * no gain, and __declspec(dllexport) additionally makes CMake's
 * WINDOWS_EXPORT_ALL_SYMBOLS inert, so the annotation actively forecloses the
 * automatic route.
 *
 * Off Windows the annotation is what a hidden-visibility build would need, and
 * is harmless today because nothing sets -fvisibility=hidden. */
#  if defined(_WIN32)
#    define LOGOS_QT_HOST_API
#  else
#    define LOGOS_QT_HOST_API LOGOS_SHARED_EXPORT
#  endif
#elif defined(_WIN32) && defined(LOGOS_SHARED_USE_DLL)
#  define LOGOS_QT_HOST_API LOGOS_SHARED_IMPORT
#else
#  define LOGOS_QT_HOST_API
#endif

#endif // LOGOS_SHARED_API_H
