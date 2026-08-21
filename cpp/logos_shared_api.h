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
 * WHERE THE DEFINITIONS LIVE NOW. Each type is owned by ONE shared library, and
 * every in-process image imports it:
 *
 *     liblogos_protocol   TokenManager, LogosAPIClient, the StoreRegistry
 *     liblogos_qt_host    LogosAPI
 *
 * liblogos_core is NOT the provider. It used to be: it absorbed both static
 * archives with --whole-archive and re-published their symbols through a
 * generated .def, so that one image could stand in for types it did not own.
 * That scheme is gone (logos-protocol#65, logos-plugin-qt#22,
 * logos-liblogos#182) and liblogos_core imports the runtime like every other
 * consumer. Measured after the migration: liblogos_core defines ZERO runtime
 * symbols, liblogos_protocol 78, liblogos_qt_host 23.
 *
 * The two fixes that look obvious are both wrong, and were both tried:
 *
 *   - Exporting everything from one image (-Wl,--export-all-symbols) makes its
 *     import library a second definition of symbols the static archives also
 *     define, and the link dies with "multiple definition of
 *     `LogosAPI::LogosAPI'".
 *   - Hand-marking a class list with __declspec(dllexport) is not a smaller
 *     version of the right answer, it is a moving target: it exported 116
 *     symbols and the Qt host runtime still failed to link with ELEVEN
 *     undefined references across five classes, plus free functions nobody had
 *     thought to mark. A curated list is correct only until the next consumer
 *     touches a symbol nobody marked, and the failure lands in a downstream
 *     repo, far from the cause.
 *
 * So on Windows the export table is GENERATED from the objects
 * (cmake/gen-shared-exports.sh) rather than curated, and this header resolves
 * its "building the shared library" branch to nothing there, so the two
 * mechanisms never compete. Off Windows a shared library exports its non-hidden
 * symbols by default and there is nothing to generate.
 *
 * The dllimport half is the load-bearing one for consumers. It rewrites the
 * reference to go through `__imp_`, so the plain symbol is never undefined and
 * GNU ld never pulls an archive member that would redefine it — regardless of
 * where a static archive sits on the link line. Without it the link still
 * succeeds and binds to the archive, with no diagnostic at all.
 *
 * IN-PROCESS vs OUT-OF-PROCESS, which is the distinction that decides who links
 * what, and the one most likely to be "simplified" away by someone tidying up:
 *
 *   IN-PROCESS  the app executable, liblogos_core, and anything QPluginLoader
 *               pulls into the app (view replica factories, legacy widget
 *               plugins, logos-standalone-app's third-party plugins). These
 *               link the SHARED libraries. A second copy here is the
 *               split-brain.
 *
 *   OUT-OF-PROCESS  logos_host, ui-host, module plugins and ui_qml backends.
 *               These keep linking the STATIC archive, and that is CORRECT, not
 *               a leftover. Each runs in its own process, so its own copy of
 *               TokenManager IS the right per-process singleton — measured,
 *               logos_host and ui-host define ~100 runtime symbols each and are
 *               deliberately exempt from the symbol gates. Staying static also
 *               keeps a .lgx self-contained: a .lgx records an EMPTY nix
 *               closure, so a shared library would not travel with it, and it
 *               keeps those plugins immune to ABI skew against a separately
 *               updated .so.
 *
 * Do not "unify" the two by moving modules onto the shared libraries. It would
 * couple every .lgx to the exact runtime build it was packaged against, to fix
 * a duplication that is not a bug in a separate process.
 *
 * WHAT ASSERTS ANY OF THIS. Nothing did, for a long time, which is how nine
 * images came to define the same singleton. There are now symbol gates in
 * logos-basecamp, logos-logoscore-cli and logos-standalone-app
 * (nix/symbol-gate.nix) asserting that each type has exactly one definer across
 * the in-process image set, each shipped with a negative control that plants a
 * real duplicate and requires the gate to reject it.
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
