find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)

add_executable(protocol_tests
    test_main.cpp
    # New protocol-level tests
    test_protocol_version.cpp
    test_json_convert_bytes.cpp
    test_lp_client.cpp
    test_call_error.cpp
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
    # Per-module concurrent dispatch (concurrency:"multi") over the plain transport.
    test_concurrent_dispatch.cpp
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
