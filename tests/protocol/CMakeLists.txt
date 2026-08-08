find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)

add_executable(protocol_tests
    test_main.cpp
    # New protocol-level tests
    test_protocol_version.cpp
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
    # The ASYNC half of the same channel. lp_invoke_async subscribed with the
    # value-only transport overload and hard-coded cb(1, ...), so a failed call
    # reached lp_result_cb as a success carrying a default value — the exact
    # thing lp_result_cb's own doc comment (and its sync twin lp_invoke) says
    # cannot happen. Matched pair: a failing call must report ok=0 with the
    # canonical error object, a succeeding one must still report ok=1 with its
    # value. Both over a real (plain TCP) transport.
    test_lp_invoke_async_error.cpp
    # The rest of that channel: everything the TRANSPORT learns once acquire has
    # already succeeded — a timeout, and MODULE_NOT_LOADED against a host that is
    # up (which is not an acquire failure on the plain transport, because
    # requestObject never checks publication). Both were discarded by
    # PlainLogosObject and by the hard-coded empty CallError above it, on the
    # SYNC path as much as the async one. Matched set: the two failures must
    # report the canonical error object, the successes must still report their
    # value, and the one case that genuinely cannot be fixed here (an unknown
    # method, which every provider answers with a bare null) is pinned as-is.
    test_call_error_after_acquire.cpp
    # The same channel over the DEFAULT transport (LocalSocket/QtRO), so the fix
    # is not silently plain-TCP-only. Exercises the deferred-completion timeout,
    # which needs no blocking provider and therefore no second event loop.
    test_call_error_qt_remote.cpp
    # Teardown of a PlainLogosObject with a call still IN FLIGHT. Joining the
    # per-call waiters closed a use-after-free but left release() blocking for
    # the remainder of the call's timeout (up to the 20s default), because
    # joinWaiters() could only join, never ask a waiter to stop. Pins all three
    # halves of the fix: teardown costs one wait slice, the callback still fires
    # EXACTLY ONCE on every outcome including cancellation (a waiter that just
    # returns turns a stall into a hang), and the join that keeps `this` alive
    # under the waiter is still there.
    test_plain_object_teardown.cpp
    # The other half of the same mechanism: what a COMPLETED call leaves behind.
    # A waiter cannot join itself, so while the registry was a plain vector the
    # only thing that ever emptied it was teardown, and every finished call
    # parked an unjoined thread (~one page of resident memory) for the life of
    # the handle — unbounded under the cached-handle shape LogosAPIConsumer
    # actually uses. Pins the registry not growing with call count, the
    # reap-vs-publish race not deadlocking, and teardown still joining what is
    # left.
    test_plain_waiter_reaping.cpp
    # The ordering rule the other two rest on: publishFinishedWaiter() is a
    # waiter's LAST access to the object, which is the only reason
    # stopAndJoinWaiters() can return while a reaper is still mid-join on a
    # waiter it has already taken out of m_waiters. No supported caller can
    # provoke the use-after-free an extra access below the publish would create,
    # so this observes the accesses directly: the object is placed across a page
    # boundary at m_waiterMu and its non-registry page is guarded while a waiter
    # runs.
    test_plain_waiter_publish_is_last.cpp
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
    # reapStaleSockets never touching live sockets or regular files.
    test_socket_paths.cpp
    # Per-module concurrent dispatch (concurrency:"multi") over the plain transport.
    test_concurrent_dispatch.cpp
    # The token-only handshake surface published before a module's initializer.
    test_handshake_surface.cpp
    test_handshake_surface_transport.cpp
    # The in-band pending-call sentinel's shape check. Detection used to be a
    # bare contains(), so any user map carrying the key hung the call for the
    # full timeout. Includes what the guard does NOT close.
    test_pending_sentinel.cpp
    # Teardown race: fail() must close the socket on the connection's strand,
    # not on the caller's thread — a caller-thread close ran concurrently with
    # the io thread initiating async_write and segfaulted inside asio's
    # reactor. Also guards the fix against leaking descriptors or hanging.
    test_rpc_connection_teardown.cpp
    # The acceptor half of the same race: RpcServer::stop() closed m_acceptor on
    # the caller's thread while doAccept() re-armed async_accept on the io
    # thread. Also guards that arming the first accept on the strand (rather
    # than inline in start()) still serves a client that connects immediately.
    test_rpc_server_teardown.cpp
    # Event delivery + reentrant-release teardown over the qt_remote (LocalSocket)
    # transport — the path a deferred "multi" completion event takes, and where the
    # refresh_balances onClientRead re-entrancy crash lived.
    test_remote_transport_events.cpp
    # Token caching: regression for the IPC token-rotation race that surfaced
    # on Linux when storage_ui issued back-to-back sync invokeRemoteMethod
    # calls. Pre-fix the client re-mints per call and the target's per-caller
    # token slot is overwritten, causing earlier in-flight calls to be
    # rejected as unauthorized. Post-fix exactly one handshake fires per
    # (client, target) pair.
    test_token_cache.cpp
    # Rejection-driven token re-exchange: when a provider rejects a call for a
    # stale/unrecognized token it returns the structured sentinel
    # (logos_rpc_status.h); the client drops the cached token, re-runs
    # requestModule and retries once. Covers success, the false-positive guard
    # (a legit empty return must NOT re-exchange), bounded retry (no loop), and
    # the async path.
    test_token_reexchange.cpp
    # test_plain_transport_tcp.cpp — entirely #if 0 (see the note inside:
    # in-process Qt-event-loop deadlock between consumer + provider under
    # nix's test sandbox; exercised cross-process by the integration matrix).
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
