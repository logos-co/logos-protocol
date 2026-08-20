find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)

# ── which of these tests are detectors, and how that was checked ─────────────
#
# A race assertion that has never been seen to fail is not evidence. Three of the
# guarantees in this suite are single points of mechanism — the exactly-once CAS
# in AsyncCall::deliver(), the deadline living on its own thread rather than the
# shared io thread, and the serialization that keeps a concurrent first caller
# from outrunning the completion subscription — so each was checked by running
# the tests against code that does not have it.
#
# That check is done on the PRE-FIX TREE, not on a switch in this one: copy the
# test file onto the commit the fix replaced and run it there. For the completion
# subscription that tree is `master`, which still has both the racy
# ensureCompletionSub() and the raw-`this` handler capture; for the fold's two
# mechanisms it is a local edit to the transport, made in a throwaway checkout
# and thrown away with it. The numbers each run produced are recorded in the test
# files themselves.
#
# Deliberately not a build option, and not the environment-variable probes an
# earlier draft carried. Both have the same cost, which the getenv() version only
# made more obvious: production source ends up holding a second implementation of
# its own contract whose entire purpose is to be wrong, in a change whose entire
# purpose is correctness. Running the real pre-fix code is also better evidence,
# because it proves the test catches the bug that existed rather than a
# hand-written imitation of it.
#
# WHICH TESTS ARE DETECTORS, since it is not all of them and the difference
# matters when one goes green. Only these fail on code missing the mechanism:
#
#   IoFoldTest.ReleaseRacingRepliesInFlightDeliversEachCallOnce
#   IoFoldTest.DeadlineFiresOnTimeWhileTheIoThreadIsBusy
#   IoFoldTest.DeadlineFiresWhileTheIoThreadIsBlockedForever
#   PlainCompletionSubOrderTest.AConcurrentFirstCallCannotOutrunTheSubscription
#   PlainCompletionSubOrderTest.TheSameRaceThroughTheRealHost
#   PlainCancelPendingRaceTest.AnExtractedReplyRacingTeardownDeliversExactlyOnce
#   PlainSendAfterFailTest.*   (all four; validated on cf1b9b0)
#   PlainEventSubSharingTest.ASecondHandleDoesNotStealTheFirstsCompletionChannel
#   PlainEventSubSharingTest.TheSameTheftThroughTheRealHost
#   PlainEventSubSharingTest.EveryHandleGetsEveryEmission
#   PlainEventSubSharingTest.ANamedAndAWildcardSubscriberEachGetOneCopy
#   PlainEventSubSharingTest.ReleasingOneHandleLeavesTheOtherSubscribed
#   PlainEventSubSharingTest.SubscriptionsSurviveChurn
#   PlainEventSubSharingTest.ACompletionThatOvertakesItsOwnResultIsStillDelivered
#   PlainEventSubSharingTest.ANewConsumerAgainstAnOldHost
#   PlainEventSubSharingTest.AnOldConsumersFrameSequenceAgainstTheNewHost
#   SyncCallReleaseRaceTest.ReleaseWithASyncCallInFlightIsReportedAndFatal
#   SyncCallReleaseRaceTest.AParkedSyncCallSurvivesReleaseFromAnotherThread
#   SyncCallReleaseRaceTest.ReleaseDefersDestructionToTheLastCallInFlight
#   SyncCallReleaseRaceTest.TheReleaseWakeRaceIsSafeEveryTime   (under gmalloc)
#   SyncCallReleaseRaceTest.DeletingTheHandleWithACallInFlightIsReportedAndFatal
#   SyncCallReleaseRaceTest.StartingACallAfterReleaseIsReportedAndFatal
#   SyncCallReleaseRaceTest.EveryEntryPointLeavesTheCountAtZero
#   SyncCallReleaseRaceTest.ConcurrentCallersThatHaveReturnedAreNotAccused
#   SyncCallReleaseRaceTest.ReleaseFromInsideAnEventCallbackIsNotAccused
#   every test in NoQtLoopTest (protocol_noqt_tests — see the second executable
#   at the bottom of this file), including the after-main() probe whose verdict
#   is that binary's EXIT CODE
#
# The release-race group is three detectors of three different things, which is
# why all of it is listed. THE FIX: on cf1b9b0 the survival test's child dies by
# signal ("died but not with expected exit code") and the deferred-destruction
# test exits 139, both three runs out of three; the release-wake stress is silent
# there without Guard Malloc and 139 with it, which is why it is marked. THE
# REMAINING MISUSES: the two death tests fail on cf1b9b0 (no diagnostic exists to
# match). A BROKEN DETECTOR: with the decrement removed from EntryGuard's
# destructor the three "not accused" tests abort on a correct program, which is
# the failure mode that would make the whole thing unshippable. Every one of
# those was checked by running it — against the pre-fix commit for the first two
# groups, against a throwaway local edit for the last.
#   PlainHostEventTeardownTest.QueuedEmissionsNeverOutliveTheHost
#
# The last one is a detector for the HARNESS rather than for the transport, and
# it is checked the same way: put a plain `m_host.reset()` back in its fixture
# and it takes the process down with SIGSEGV on the first round, with or without
# a memory detector. The backtrace it produces is recorded in the file.
#
# The subscription-sharing group is nine detectors of ONE mechanism: every handle
# holding its own registration on the shared connection. All nine were run RED on
# the pre-fix tree — where the second handle to subscribe to an event on a
# connection silently takes the first one's channel — and green here; the tenth
# case in that file is a pin, and is named below.
#
# Note what is NOT on that list: the PER-PATH exactly-once tests stay GREEN with
# the CAS removed. They are PINS, NOT DETECTORS — a call resolved once calls
# deliver() once no matter what guards it — and only racing teardown against
# replies in flight (the first entry) and the hand-built extract-then-cancel
# interleaving (the last) actually catch a doubled callback. Do not read a green
# per-path test as evidence the gate is there. Nor is
# PlainEventSubSharingTest.TeardownRemovesEverySubscription, which passes on the
# pre-fix tree: only the tests that assert a SECOND live subscriber still has its
# channel detect the subscription clobber.
#
# AND DO NOT READ ReleaseRacingRepliesInFlightDeliversEachCallOnce AS COVERING
# EVERY DOUBLE. It races teardown of the HANDLE against replies with the
# connection still up, so it never enters sendCallAsync's stopped branch: against
# a deliberately broken version of that branch it reports 0 doubles in 10,000
# calls while PlainSendAfterFailTest's own 10,000-call burst — which stops the
# CONNECTION mid-burst — reports 5 to 10. Same shape, different contended object,
# and only the second one sees that path.
# AND "THE CAS" IS TWO GATES, which is a correction to the paragraph above and
# was found while re-validating this on the no-Qt binary. AsyncCall guards a
# duplicate delivery twice: claim()'s compare-exchange, and the swap in
# takeCallback() that leaves a second caller holding an empty std::function.
# Removing the CAS ALONE leaves every test here green (0 doubles) because the
# swap absorbs the duplicate; removing BOTH makes the release-race shape report
# 22 doubled deliveries in 20 rounds x 500 calls while the per-path tests stay
# green. Measured on the no-Qt twin
# (NoQtLoopTest.ReleaseRacingRepliesDeliversEachCallOnceWithNoQtLoop). A
# validation that removes one of the two proves nothing.

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
    # A THIRD thing release() and the shared RpcConnection get wrong together:
    # m_eventCallbacks is keyed by (object, event) and ASSIGNED, so the second
    # handle a connection hands out for the same module silently takes the
    # first one's subscription — the completion channel included. Reachable with
    # no concurrency at all. Covers the theft over the raw wire and through the
    # shipping host, every-handle-gets-every-emission, one handle leaving without
    # taking another's subscription with it, and randomized churn with a quiesced
    # per-round audit.
    test_plain_event_sub_sharing.cpp
    # cancelPending() and the two things it is NOT. dispatchIncoming copies a
    # handler out under the connection mutex and invokes it unlocked, so a
    # cancel arriving in that gap stops nothing — at-most-once INVOCATION is the
    # extract-and-erase, and exactly-once DELIVERY is AsyncCall::deliver()'s CAS.
    # Constructs the interleaving by hand rather than racing for it, and covers
    # the OTHER handler shape (the promise sendCall registers) too.
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
    # release() racing a call still running on another thread — a PRE-EXISTING
    # use-after-free (reproduced identically on master, on #45 and on #46), now
    # FIXED by a live-reference count: release() drops the owner's reference and
    # the last call to leave destroys the object, so a call that entered first is
    # safe and release() still waits for nothing. This file pins the fix (the
    # reproduction survives, destruction is deferred and happens exactly once,
    # the tight release-wake race 400 times over), pins the two shapes that are
    # still caller errors (`delete obj` with a call in flight, and STARTING a
    # call after release()) as named aborts rather than a SIGSEGV elsewhere, and
    # — the half that decides whether a diagnostic is shippable at all — pins
    # that a correct program is never accused: every entry point, every
    # early-return path, many threads at once, and the shipped
    # release()-from-an-io-thread-event-callback shape.
    test_sync_call_release_race.cpp
    # A test of the HARNESS the five tests above run on. Each of them puts a
    # ModuleProxy on a worker QThread and publishes it through a
    # PlainTransportHost, and each of them used to free that host from the TEST
    # thread — while the worker was inside the direct-connection lambda
    # publishObject() hooked onto eventResponse, which locks the host's m_mu
    # after converting the payload. Shared fixture code, so a fault there can
    # corrupt any run in this binary. Pins the ordering that live_host_teardown.h
    # now owns: the host is destroyed ON the proxy's thread, every emission
    # queued before that runs against a live host, and none runs after.
    test_plain_host_event_teardown.cpp
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

# ── the Qt-LOOP-FREE half ────────────────────────────────────────────────────
#
# A SECOND executable, for one reason: QCoreApplication is a process-wide
# singleton and protocol_tests' main() constructs it before the first test runs,
# so no test in that binary can ever observe the state a Qt-free host process is
# permanently in — instance() == nullptr. That state is where the async delivery
# hop dropped every callback on the floor, and a test that cannot reach it
# cannot detect it. Destroying the application partway through the main run to
# get there would be a far worse trade than one more binary.
#
# Everything here links the same library and exercises the same code paths; only
# main() differs. See test_main_noqt.cpp.
add_executable(protocol_noqt_tests
    test_main_noqt.cpp
    # The async delivery hop, in a process with no Qt event loop. It read
    # QCoreApplication::instance() and RETURNED when it was null, so in a
    # Qt-free host — the deployment the plain transport exists for — every
    # async callback was dropped: exactly-once became exactly-never, silently,
    # on all four resolvers (reply, deferred completion, deadline,
    # cancellation). Pins that each of them now lands, that none of them lands
    # inline or on the io thread (which is the re-entrancy class that has
    # already cost a SIGSEGV here, so a "just call it inline" fix would be
    # worse than the bug), and that exactly-once survives the release-racing-
    # replies shape against the new delivery vehicle.
    #
    # It also carries a probe that runs AFTER main() returns — a delivery issued
    # from a static destructor, which is where a lazily-constructed delivery
    # vehicle is already destroyed. Its verdict is this binary's EXIT CODE (70 on
    # failure), because no test case runs in that window; see LateDeliveryProbe.
    # That is also why a failure there shows up as a build-time error from
    # gtest_discover_tests, which runs this binary to enumerate its tests.
    test_delivery_without_qt.cpp
)

target_include_directories(protocol_noqt_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(protocol_noqt_tests PRIVATE
    logos_protocol
    GTest::gtest
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::RemoteObjects
)

gtest_discover_tests(protocol_noqt_tests)
