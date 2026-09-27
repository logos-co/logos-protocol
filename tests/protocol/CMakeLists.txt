if(LOGOS_PROTOCOL_BUILD_QT)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)

# ── which of these tests are detectors, and how that was checked ─────────────
#
# A race assertion that has never been seen to fail is not evidence, so each
# detector here was run against the code the fix replaced (the pre-fix commit,
# not a switch in this tree), and the numbers each run produced are recorded in
# the test file:
#
#   PlainSendAfterFailTest.*   (all three; validated on cf1b9b0)
#
# The detectors of the Qt runtime's own plain transport (tcp and tcp_ssl) went
# with it in 0.15.

add_executable(protocol_tests
    test_main.cpp
    # New protocol-level tests
    test_protocol_version.cpp
    # Byte-for-byte reference checks against Qt 6.9.2 QDataStream and the
    # current ModuleProxy meta-object. The production codec under test has no
    # Qt dependency; Qt lives only on this reference side.
    test_qt_remote_plain_compat.cpp
    # Real local-socket interoperability in both directions: raw plain codec
    # client against QRemoteObjectHost, then QRemoteObjectNode against a raw
    # plain server.
    test_qt_remote_plain_socket_compat.cpp
    test_qt_remote_plain_adapter.cpp
    test_json_convert_bytes.cpp
    # The `result` half of the canonical converter pair: qvariantToNlohmann has
    # always owned LogosResult -> JSON; jsonToLogosResult is its inverse, and
    # the state it exists to protect is an ABSENT error (which no
    # std::string-typed intermediate can carry).
    test_json_convert_result.cpp
    test_universal_provider_dispatch.cpp
    # Event payload fidelity across the universal -> Qt bridge
    # (setEventListenerStdBridge). Every Qt-free provider emits through that one
    # conversion; test_universal_provider_dispatch references it only to satisfy
    # the pure virtual and asserts nothing about payloads.
    test_event_payload_fidelity.cpp
    test_lp_client.cpp
    # The event-delivery regression matrix: transport x provider kind x consumer
    # path x timing. Event delivery has broken silently three times, each in a
    # cell nothing covered — a one-shot subscription, a deferred one that never
    # armed outside Remote mode, and a plain host dropping a Subscribe for a
    # not-yet-published object. Every case here has a control that is green
    # independently of those fixes.
    test_event_delivery_matrix.cpp
    # The canonical LIDL <-> JSON codec (cpp/logos_codec.h): one implementation
    # of an encoding that used to exist six times with divergent semantics.
    # Pins the contract — accepted shapes, rejected shapes, and bytes staying
    # tagged at any nesting depth.
    test_codec.cpp
    # Owner-thread anchoring for lp clients: a client created from a worker
    # thread (the generated bind_<iface>() wrapper is lazy, so the first caller
    # wins) must still be serviced by the Qt main thread's event loop instead of
    # falling out on replica-acquire timeouts.
    test_lp_client_owner_thread.cpp
    test_call_error.cpp
    # The call-error channel over the DEFAULT transport (LocalSocket/QtRO).
    # Exercises the deferred-completion timeout, which needs no blocking
    # provider and therefore no second event loop, and an unknown method: the
    # provider's unknown_method refusal, carried intact as a RESULT.
    test_call_error_qt_remote.cpp
    # cancelPending() and the two things it is NOT. dispatchIncoming copies a
    # handler out under the connection mutex and invokes it unlocked, so a
    # cancel arriving in that gap stops nothing — at-most-once INVOCATION is the
    # extract-and-erase, and exactly-once DELIVERY is the handler's. Constructs
    # the interleaving by hand rather than racing for it, for the promise shape
    # sendCall registers and over a real connection.
    test_plain_cancel_pending_race.cpp
    # The gap between sendCallAsync's stopped-check and its registration under
    # m_mu. A fail() that completes in it sweeps a map the caller has not
    # written to yet, so the handler lands on a connection nobody will ever
    # sweep again and the call is answered by NOTHING — the caller waits out its
    # whole deadline and is then told "timeout" for a connection that was
    # already gone. Predates the fold (master has it on the promise path).
    # Builds the interleaving by parking the caller on the connection's own
    # mutex rather than racing for it.
    test_plain_send_after_fail.cpp
    # Component tests that moved here with their code (from logos-cpp-sdk)
    test_token_manager.cpp
    test_mock_store.cpp
    test_mock_transport.cpp
    test_logos_mode.cpp
    test_logos_instance.cpp
    test_plugin_registry.cpp
    test_transport_factory.cpp
    test_registry_factory.cpp
    test_rpc_framing.cpp
    test_json_codec.cpp
    test_cbor_codec.cpp
    # uint64 across the plain wire. RpcValue had no unsigned alternative, so a
    # value above int64max wrapped to -1 independently in each direction; no
    # plain-tier test used an integer outside int32 range. Also pins the
    # off-by-one in the QJsonValue::Double -> int64 guard.
    test_plain_uint64.cpp
    # Qt-free socket-access + stale-socket reaper helpers (multi-user local
    # transport): applySocketPerms group/mode policy, isSocketDead predicate,
    # reapStaleSockets never touching live sockets or regular files. POSIX only.
    $<$<NOT:$<PLATFORM_ID:Windows>>:test_socket_paths.cpp>
    # The token-only handshake surface published before a module's initializer.
    test_handshake_surface.cpp
    test_handshake_surface_transport.cpp
    # The trust-root grant and the two host services it gates. The grant has to
    # be recorded in the SAME image as the gate that reads it (a host binary and
    # a module cdylib each link their own copy of this library), which is the
    # property a "simplification" would quietly break; these cases fail loudly
    # if the gates stop being closed by default.
    test_host_services_grant.cpp
    # The in-band pending-call sentinel's shape check. Detection used to be a
    # bare contains(), so any user map carrying the key hung the call for the
    # full timeout. Includes what the guard does NOT close.
    test_pending_sentinel.cpp
    # Teardown race: fail() must close the socket on the connection's strand,
    # not on the caller's thread — a caller-thread close ran concurrently with
    # the io thread initiating async_write and segfaulted inside asio's
    # reactor. Also guards the fix against leaking descriptors or hanging.
    test_rpc_connection_teardown.cpp
    # Event delivery + reentrant-release teardown over the qt_remote (LocalSocket)
    # transport — the path a deferred "multi" completion event takes, and where the
    # refresh_balances onClientRead re-entrancy crash lived.
    test_remote_transport_events.cpp
    # Freeing a dynamic replica that never reached Valid leaves QtRO holding it
    # as a raw pointer (m_parentsNeedingConnect) and the implementation walks
    # that list when the source finally publishes. requestObject()'s timeout
    # branch did exactly that; it killed the json_rpc_bridge host whenever
    # storage_module came back after a wedge.
    #
    # Its two crash cases are DETECTORS, validated on the pre-fix tree dcf4f05:
    # SIGSEGV 5/5 runs, with the production reports' stack. The third case,
    # ParksExactlyOneFacade, is a GUARD on the bound rather than on the crash --
    # it cannot run on dcf4f05 because it asserts through a hook the fix adds, so
    # it was validated against the park-WITHOUT-reuse mechanism instead (a
    # throwaway edit making takeParked() return nullptr): it reports 8 parked
    # facades for 8 attempts, and 8 still parked after recovery.
    test_unconfigured_replica_lifetime.cpp
    test_deferred_subscription.cpp
    # The other half of deferred subscription: a provider that dies UNDER an
    # already-armed subscription. It re-arms on the same node with the event
    # helper still attached, so the stream resumed silently with a hole in it
    # and nothing was even polling for the death. Pins the generation counter
    # (visible to a plain lp_subscribe caller) and the LOST -> ARMED pair the
    # client's status callback delivers, with a provider-stays-up control.
    test_subscription_continuity.cpp
    # The other half of continuity: a per-module restart policy. The
    # load-bearing test is ManualStillArmsDeferred — manual must mean "do not
    # RE-arm", never "do not arm", or every subscription taken during init()
    # before its provider listens stops working.
    test_subscription_restart_policy.cpp
    # Provider swapped faster than the 1 s poll. Detectors: four RED on 9e29120 (and without bindToSource), the gate one RED without
    # the helper's gate, the late-subscriber/rearm three RED on 4ceea79 (rearm also without its replay). Churn* is a control.
    test_subscription_fast_restart.cpp
    # Token caching: regression for the IPC token-rotation race that surfaced
    # on Linux when storage_ui issued back-to-back sync invokeRemoteMethod
    # calls. Pre-fix the client re-mints per call and the target's per-caller
    # token slot is overwritten, causing earlier in-flight calls to be
    # rejected as unauthorized. Post-fix exactly one handshake fires per
    # (client, target) pair.
    test_token_cache.cpp
    # Readiness-gated exchange: the token exchange used to ask "is the target there?"
    # on capability_module's own budget while the call about to follow asked the same
    # question on the caller's, much longer, one — so a module that had not finished
    # publishing lost its grant to a wait its caller never agreed to. These pin the
    # ORDER (wait, then mint) and both ways of getting it wrong: minting early costs a
    # wasted unauthorized round, waiting twice is a fleet-wide latency regression.
    test_readiness_gated_exchange.cpp
    # A blocking acquire for a module nothing listens for fails within a short grace instead of
    # spending the caller's whole budget; a module that is listening keeps the full budget.
    test_absent_target_fails_fast.cpp
    # The handshake negative cache: an absence is remembered only once the business object
    # has PROVEN the module is up. A probe also misses when the module has not published
    # yet, and nothing re-probes an entry — so caching that reason writes a merely-late
    # module off for the life of the process.
    test_handshake_negative_cache.cpp
    # Rejection-driven token re-exchange: when a provider rejects a call for a
    # stale/unrecognized token it returns the structured sentinel
    # (logos_rpc_status.h); the client drops the cached token, re-runs
    # requestModule and retries once. Covers success, the false-positive guard
    # (a legit empty return must NOT re-exchange), bounded retry (no loop), and
    # the async path.
    test_token_reexchange.cpp
    # Per-identity token stores. The escalation being closed is that a host
    # loading plugins in-process gives all of them ONE store holding every
    # module's root token, and a client presents a cached token before it ever
    # mints one — so a plugin reaches an undeclared backend with no handshake at
    # all, and per-plugin origin STRINGS change nothing because origin was never
    # consulted. Half the file pins that forIdentity() returns the SAME OBJECT
    # instance() does until a name is explicitly isolated (the whole
    # back-compatibility argument); the other half pins that isolated identities
    # cannot see each other's or the ring's tokens. Every escalation case carries
    # an ambient CONTROL asserting the token IS reachable without isolation —
    # without that control the isolated assertions would pass on a store that
    # never held the token in the first place.
    test_token_store_identity.cpp
    # Module identity: name()/version() answered by the proxy from
    # providerName()/providerVersion() for a provider whose own dispatch does
    # not, so a legacy module gains them with no edit. Pins the two properties
    # the design rests on -- the fallback runs only after dispatch returned
    # "unknown method" (no silent shadowing), and the listing agrees with what
    # is callable -- plus the auth gate, which introspection deliberately lacks.
    test_module_identity.cpp
    # The INBOUND token store. ModuleProxy::m_tokens is the proxy's own
    # caller-keyed record, and it had ZERO production writers: every real
    # authorization decision came out of TokenManager, which was then
    # direction-mixed (outbound tokens keyed by callee sat in the same flat map
    # as inbound ones keyed by caller; test_token_direction.cpp split it). Two
    # detectors, one per mechanism —
    # AnInformedTokenLandsInTheProxysOwnStore fails when informModuleToken does
    # not record, AnAmbientTokenDoesNotAuthorizeAnIsolatedProxy fails when
    # isAuthorized scans TokenManager::instance() instead of the proxy's own
    # store — plus the composed production shape and four pins. All three
    # detectors were run RED against the unfixed tree; the runs are recorded in
    # the file.
    test_inbound_token_store.cpp
    # WHO is calling, resolved out of that same inbound record as a
    # by-product of the authorization scan. Production of the caller
    # DOCUMENT and its lifetime on the dispatching thread only — nothing
    # in this repo delivers it into a module cdylib yet. Most of the file
    # is about the cases that must REFUSE to name anyone; the outbound-token
    # case is the one an obvious simplification would delete, and since the
    # direction split it asserts a refusal to AUTHORIZE as well.
    test_call_caller.cpp
    # An isolated identity's OWN credential: the store is born empty, the host
    # mints/registers/adopts, and both halves — no host anchor, and no lockout —
    # are asserted against real ModuleProxy authorization. See the file header
    # for the two neutered builds that make each half a detector.
    test_consumer_credential.cpp
    # DIRECTION: outbound vs inbound in one store. The detector for the split,
    # and the pins that say what the split must not break. Three of its
    # assertions also detect whether logos-plugin-qt's companion one-line move
    # (informModuleToken -> saveInboundToken) has landed; see the note on
    # DirectionProvider.
    test_token_direction.cpp
    # THE LAYOUT of TokenManager, measured on a live object. The store split
    # shipped as three members, which moved m_mutex from +24 to +56 on an object
    # the HOST allocates and MODULE images mutate through their own statically
    # linked copy of these accessors. Mixing a host and a module .lgx built
    # either side of that deadlocked the module's host process on the first
    # inbound token push -- silently, with the daemon still reporting
    # "loaded"/"crashed":0, because the only load gate compares MAJOR. Direction
    # is now a KEY NAMESPACE inside the original single QHash; these tests are
    # what keep it that way, and they are RED on e514c53.
    test_token_manager_abi.cpp
    # THE INBOUND DOOR across the C ABI (lp_token_save_inbound, protocol 0.8):
    # a caller's token is never filed as an outbound credential, granted or
    # not. The file names the one-line neutering that turns each case red.
    test_inbound_door.cpp
)

target_include_directories(protocol_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(protocol_tests PRIVATE
    logos_protocol
    GTest::gtest
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::RemoteObjects
    Qt${QT_VERSION_MAJOR}::Test
)

gtest_discover_tests(protocol_tests)

# These cases regress lock cycles, so each runs in its own process with a CTest
# deadline. A broken implementation fails as a timeout instead of wedging the
# rest of protocol_tests and hiding which shutdown path deadlocked.
add_executable(qt_remote_plain_regression_probe
    qt_remote_plain_shutdown_probe.cpp
)
target_link_libraries(qt_remote_plain_regression_probe PRIVATE
    logos_protocol
    Qt${QT_VERSION_MAJOR}::Core
)
add_test(NAME QtRemotePlainRegression.adapter-shutdown
         COMMAND qt_remote_plain_regression_probe)
set_tests_properties(QtRemotePlainRegression.adapter-shutdown PROPERTIES TIMEOUT 8)

# ELF only: one process loading both shared protocol libraries, each ahead of
# the other's consumer. Mach-O and PE bind every import to a named library.
if(UNIX AND NOT APPLE)
add_library(plain_consumer_probe SHARED protocol_consumer_probe.cpp)
target_compile_definitions(plain_consumer_probe PRIVATE PROBE_FUNCTION=plainConsumerResolvesTo)
target_link_libraries(plain_consumer_probe PRIVATE logos_protocol_plain_shared ${CMAKE_DL_LIBS})
add_library(qt_consumer_probe SHARED protocol_consumer_probe.cpp)
target_compile_definitions(qt_consumer_probe PRIVATE PROBE_FUNCTION=qtConsumerResolvesTo)
target_link_libraries(qt_consumer_probe PRIVATE logos_protocol_shared ${CMAKE_DL_LIBS})
# basecamp's order: the Qt library ahead of a plain consumer.
add_executable(protocol_symbol_isolation_plain_tests test_protocol_symbol_isolation.cpp)
target_compile_definitions(protocol_symbol_isolation_plain_tests PRIVATE
    PROBE_FUNCTION=plainConsumerResolvesTo EXPECTED_LIBRARY="liblogos_protocol_plain.")
target_link_options(protocol_symbol_isolation_plain_tests PRIVATE "LINKER:--no-as-needed")
target_link_libraries(protocol_symbol_isolation_plain_tests PRIVATE
    logos_protocol_shared plain_consumer_probe GTest::gtest_main)
gtest_discover_tests(protocol_symbol_isolation_plain_tests)
# The reverse order: the plain library ahead of a Qt consumer.
add_executable(protocol_symbol_isolation_qt_tests test_protocol_symbol_isolation.cpp)
target_compile_definitions(protocol_symbol_isolation_qt_tests PRIVATE
    PROBE_FUNCTION=qtConsumerResolvesTo EXPECTED_LIBRARY="liblogos_protocol.")
target_link_options(protocol_symbol_isolation_qt_tests PRIVATE "LINKER:--no-as-needed")
target_link_libraries(protocol_symbol_isolation_qt_tests PRIVATE
    logos_protocol_plain_shared qt_consumer_probe GTest::gtest_main)
gtest_discover_tests(protocol_symbol_isolation_qt_tests)
endif()

if(NOT WIN32)
add_executable(qt_remote_plain_relative_path_probe
    qt_remote_plain_relative_path_probe.cpp
)
target_link_libraries(qt_remote_plain_relative_path_probe PRIVATE
    logos_qt_remote_plain_transport
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Network
)
add_test(NAME QtRemotePlainRegression.relative-local-path
         COMMAND qt_remote_plain_relative_path_probe)
set_tests_properties(QtRemotePlainRegression.relative-local-path PROPERTIES TIMEOUT 5)
endif()
endif()

# A genuinely Qt-free executable for the codec's bounds, frame and semantic
# round trips. Keeping it separate prevents an accidental Qt include or link
# dependency from being hidden by protocol_tests' reference-side Qt linkage.
add_executable(qt_remote_plain_wire_tests
    test_qt_remote_plain_wire.cpp
    test_qt_remote_plain_transport.cpp
)
target_link_libraries(qt_remote_plain_wire_tests PRIVATE
    logos_qt_remote_plain_transport
    GTest::gtest_main
)
gtest_discover_tests(qt_remote_plain_wire_tests)

# Public C ABI linked to the production Qt-free runtime.  Keep this in its own
# executable so any accidental Qt dependency is visible in the link closure.
add_executable(qt_remote_plain_cabi_tests
    # Run the version and allocator-ownership C ABI checks against the plain
    # implementation too. This executable is the one available in native
    # Windows builds where LOGOS_PROTOCOL_BUILD_QT is disabled.
    test_protocol_version.cpp
    test_qt_remote_plain_cabi.cpp
    test_plain_dispatch_gate.cpp
    test_plain_local_inproc.cpp
    test_plain_caller_resolver.cpp
    test_plain_token_revocation.cpp
    test_plain_remote_session.cpp
)
target_link_libraries(qt_remote_plain_cabi_tests PRIVATE
    logos_protocol_plain
    GTest::gtest_main
)
gtest_discover_tests(qt_remote_plain_cabi_tests PROPERTIES TIMEOUT 8)

# A runtime delegate is installed once per image, so its test has a process of its own.
add_executable(plain_runtime_delegate_tests test_plain_runtime_delegate.cpp)
target_link_libraries(plain_runtime_delegate_tests PRIVATE
    logos_protocol_plain
    GTest::gtest_main
)
gtest_discover_tests(plain_runtime_delegate_tests PROPERTIES TIMEOUT 20)

# A host on the shared runtime and a module image with its own hidden static runtime.
add_library(plain_delegate_image MODULE plain_delegate_image.cpp)
target_link_libraries(plain_delegate_image PRIVATE logos_protocol_plain)
set_target_properties(plain_delegate_image PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
if(APPLE)
    target_link_options(plain_delegate_image PRIVATE
        "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/plain_delegate_image.exports")
elseif(UNIX)
    target_link_options(plain_delegate_image PRIVATE
        "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/plain_delegate_image.map")
endif()
add_executable(plain_runtime_delegate_image_tests test_plain_runtime_delegate_image.cpp)
target_link_libraries(plain_runtime_delegate_image_tests PRIVATE
    logos_protocol_plain_shared
    GTest::gtest_main
    ${CMAKE_DL_LIBS}
)
target_compile_definitions(plain_runtime_delegate_image_tests PRIVATE
    DELEGATE_IMAGE_PATH="$<TARGET_FILE:plain_delegate_image>"
    DELEGATE_IMAGE_NAME="$<TARGET_FILE_NAME:plain_delegate_image>")
add_dependencies(plain_runtime_delegate_image_tests plain_delegate_image)
gtest_discover_tests(plain_runtime_delegate_image_tests PROPERTIES TIMEOUT 20)

# The same C ABI checks against the shared plain runtime: the form liblogos,
# its hosts and logoscore share in one process.
add_executable(qt_remote_plain_cabi_shared_tests
    test_protocol_version.cpp
    test_qt_remote_plain_cabi.cpp
)
target_link_libraries(qt_remote_plain_cabi_shared_tests PRIVATE
    logos_protocol_plain_shared
    GTest::gtest_main
)
gtest_discover_tests(qt_remote_plain_cabi_shared_tests PROPERTIES TIMEOUT 8)

# A process that called over a tls_tcp session through the shared runtime must
# exit: the child makes one call and returns, and the test runs it many times.
find_package(OpenSSL REQUIRED)
add_executable(plain_exit_child plain_exit_child.cpp)
target_link_libraries(plain_exit_child PRIVATE logos_protocol_plain_shared)
add_executable(qt_remote_plain_exit_tests test_plain_exit.cpp)
target_link_libraries(qt_remote_plain_exit_tests PRIVATE
    logos_protocol_plain_shared
    OpenSSL::Crypto
    GTest::gtest_main
)
target_compile_definitions(qt_remote_plain_exit_tests PRIVATE
    PLAIN_EXIT_CHILD="$<TARGET_FILE:plain_exit_child>")
add_dependencies(qt_remote_plain_exit_tests plain_exit_child)
gtest_discover_tests(qt_remote_plain_exit_tests PROPERTIES TIMEOUT 600)

# A real Qt Remote Objects host exercises callback-initiated teardown of the
# Qt-free client across the legacy/plain interoperability boundary. Qt's local
# socket path compatibility is required on Unix; Windows modules use plain.
if(LOGOS_PROTOCOL_BUILD_QT AND UNIX)
add_executable(qt_remote_plain_qro_callback_destroy_tests
    test_qt_remote_plain_qro_callback_destroy.cpp
)
target_link_libraries(qt_remote_plain_qro_callback_destroy_tests PRIVATE
    logos_protocol_plain
    GTest::gtest
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::RemoteObjects
)
gtest_discover_tests(qt_remote_plain_qro_callback_destroy_tests PROPERTIES TIMEOUT 8)
endif()

# Which implementation each transport resolves to. Also the Qt-configured test
# Windows runs (nix/tests-windows-qt.nix), where local must mean plain.
if(LOGOS_PROTOCOL_BUILD_QT)
add_executable(transport_resolution_tests test_transport_resolution.cpp)
target_link_libraries(transport_resolution_tests PRIVATE
    logos_protocol
    GTest::gtest
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::RemoteObjects
)
gtest_discover_tests(transport_resolution_tests)
endif()
