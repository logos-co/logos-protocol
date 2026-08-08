find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)

# ── validating the detectors ────────────────────────────────────────────────
#
# A race assertion that has never been seen to fail is not evidence, so the race
# tests in this directory are checked against the code they were written for:
# copy the test file onto the pre-fix tree and run it there. For the completion
# subscription that tree is `master` — it has both the racy ensureCompletionSub()
# and the raw-`this` handler capture — and the numbers those runs produced are
# recorded in the test files themselves.
#
# Deliberately no build flag, no environment probe and no second implementation
# in the shipped source. An earlier draft carried both, and the cost is the same
# either way: production code grows a path whose only purpose is to be wrong.
# Running the real pre-fix code is also the stronger evidence, because it proves
# the test catches the bug that actually existed rather than a hand-written
# imitation of it.

# ── detector inversions: OFF, and off in every build that ships ──────────────
#
# An assertion that has never been seen to fail is not evidence. Two of the
# guarantees in this suite are single points of mechanism — the exactly-once CAS
# in AsyncCall::deliver(), and the deadline living on its own thread rather than
# the shared io thread — so the suite has to be runnable ONCE with each of them
# removed, and go red.
#
# This is a build option rather than the environment-variable probes an earlier
# draft carried. Those shipped in the production translation unit, read getenv()
# once per delivery in the hot path, and one of them compiled a raw-`this`
# use-after-free path into the binary that a released build would happily
# execute if the variable were set. A compile-time seam costs nothing at runtime
# and cannot be turned on by accident: it is not referenced by nix/tests.nix or
# by any packaging, so a shipped build cannot have it.
#
#   cmake -S tests -B build-broken -DLOGOS_PROTOCOL_DETECTOR_INVERSIONS=ON
#   ./build-broken/protocol/protocol_tests --gtest_filter='IoFoldTest.*:\
#PlainCompletionSubOrderTest.*:PlainCancelPendingRaceTest.*'
#
# SIX tests must go RED there, and no others:
#
#   IoFoldTest.ReleaseRacingRepliesInFlightDeliversEachCallOnce
#   IoFoldTest.DeadlineFiresOnTimeWhileTheIoThreadIsBusy
#   IoFoldTest.DeadlineFiresWhileTheIoThreadIsBlockedForever
#   PlainCompletionSubOrderTest.AConcurrentFirstCallCannotOutrunTheSubscription
#   PlainCompletionSubOrderTest.TheSameRaceThroughTheRealHost
#   PlainCancelPendingRaceTest.AnExtractedReplyRacingTeardownDeliversExactlyOnce
#
# A green run of any of those in this build means the corresponding real
# assertion is not exercising what it claims to, which is a bug in the test, not
# a licence to trust it. Note what is NOT on the list: the per-path exactly-once
# tests stay GREEN with the CAS removed. They are pins, not detectors — only
# racing teardown against replies in flight (the first entry) and the
# hand-built extract-then-cancel interleaving (the last) actually catch a
# doubled callback.
option(LOGOS_PROTOCOL_DETECTOR_INVERSIONS
       "Compile the plain transport with its exactly-once gate and its \
independent deadline clock REMOVED, so the tests that assert them can be shown \
to fail. Never enable outside a throwaway detector build." OFF)

if(LOGOS_PROTOCOL_DETECTOR_INVERSIONS)
    message(WARNING "LOGOS_PROTOCOL_DETECTOR_INVERSIONS=ON: this build is "
                    "DELIBERATELY BROKEN and is only useful for validating the "
                    "test suite's detectors.")
    target_compile_definitions(logos_protocol PUBLIC
        LOGOS_PLAIN_DETECTOR_BREAK_ONCE=1
        LOGOS_PLAIN_DETECTOR_BREAK_DEADLINE_ISOLATION=1)
endif()

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
    # Teardown of a PlainLogosObject with a call still IN FLIGHT. Written when
    # in-flight calls were threads: joining them closed a use-after-free but left
    # release() blocking for the remainder of the call's timeout, so they were
    # asked to stop first and teardown cost one 25ms wait slice. Public API only,
    # so the io_context fold did not touch it — but its BOUNDS are now loose
    # rather than tight: there is no wait slice any more (0ms, measured) and no
    # join, because nothing captures `this`. What it still pins exactly is the
    # part that matters most: the callback fires EXACTLY ONCE on every outcome
    # including cancellation, since a dropped one turns a stall into a hang.
    test_plain_object_teardown.cpp
    # The other half of the same mechanism: what a COMPLETED call leaves behind.
    # Written for the waiter registry (a map of threads plus a publish list) and
    # retargeted by the io_context fold at the registry that replaced it,
    # CallState::inflight. The claims are unchanged and are why it survived the
    # rework: retention must not grow with call count, a burst that goes idle
    # must drain with no further call, and teardown must still deliver exactly
    # once.
    test_plain_waiter_reaping.cpp
    # test_plain_waiter_publish_is_last.cpp — DELETED BY THE io_context FOLD.
    # It pinned one rule: publishFinishedWaiter() is a waiter thread's LAST
    # access to the object, which was the only reason stopAndJoinWaiters() could
    # return while a reaper was still mid-join on a waiter it had already taken
    # out of m_waiters. There are no waiter threads, no reaper and no publish
    # list any more, so there is no ordering left to pin — the property it
    # protected ("no handler touches the object after teardown") is structural
    # now: handlers hold a weak_ptr to CallState and never dereference the
    # handle. test_iofold.cpp OBSERVES that instead of pinning the protocol that
    # used to be needed for it.
    test_iofold.cpp
    # The OTHER thing release() leaves behind: the deferred-completion event
    # subscription. Its handler captured raw `this` and lives in the
    # RpcConnection, which is shared across every handle and outlives all of
    # them. dispatchIncoming copies the handler out under its mutex and invokes
    # it unlocked, so the unsubscribe release() sends cannot reach one already in
    # flight on the io thread — and nothing joins that thread. Pins the handler
    # not reaching the object at all.
    test_plain_completion_sub_lifetime.cpp
    # The ORDERING half of the same subscription. ensureCompletionSub() raised
    # its flag under the rendezvous mutex and released the mutex before
    # subscribing, so a concurrent first caller could put a Call on the wire
    # ahead of the Subscribe frame; a "multi" provider then completed into a
    # subscription the host had not registered and the completion was dropped.
    # Costs a caller its whole timeout and leaves nothing behind — see the file.
    test_plain_completion_sub_order.cpp
    # cancelPending() and the two things it is NOT. dispatchIncoming copies a
    # handler out under the connection mutex and invokes it unlocked, so a
    # cancel arriving in that gap stops nothing — at-most-once INVOCATION is the
    # extract-and-erase, and exactly-once DELIVERY is AsyncCall::deliver()'s CAS.
    # Constructs the interleaving by hand rather than racing for it, and covers
    # the OTHER handler shape (the promise sendCall registers) too.
    test_plain_cancel_pending_race.cpp
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
    test_deferred_subscription.cpp
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
