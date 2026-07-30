cmake_minimum_required(VERSION 3.14)
project(LogosProtocol)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Find Qt packages — Qt is an implementation detail of the qt_remote (QRO)
# and qt_local transports plus the Qt value types used at the QRO boundary.
# The PUBLIC C ABI (logos_protocol.h) is Qt-free.
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core RemoteObjects)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core RemoteObjects)

# Plain-C++ transport dependencies (no Qt). The `system` component is
# required for boost::system::error_code (Boost.Asio); without it
# Boost::system isn't an exported imported target on Boost 1.87+.
find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)

set(PROTOCOL_SOURCES
    logos_protocol.h
    logos_module_impl.h
    logos_call_error.h
    logos_protocol.cpp
    logos_types.cpp
    logos_types.h
    logos_api_client.cpp
    logos_api_client.h
    logos_api_consumer.cpp
    logos_api_consumer.h
    module_proxy.cpp
    module_proxy.h
    token_manager.cpp
    token_manager.h
    logos_mode.h
    logos_instance.h
    logos_socket_paths.h
    logos_socket_paths.cpp
    plugin_registry.h
    logos_object.h
    logos_thread_marshal.h
    logos_provider_interface.cpp
    logos_provider_interface.h
    logos_json_convert.cpp
    logos_json_convert.h
    logos_transport.h
    logos_transport.cpp
    logos_transport_config.h
    logos_transport_config_json.h
    logos_transport_config_json.cpp
    logos_transport_factory.cpp
    logos_transport_factory.h
    logos_registry.h
    logos_registry_factory.h
    logos_registry_factory.cpp
    implementations/qt_local/local_transport.cpp
    implementations/qt_local/local_transport.h
    implementations/qt_remote/remote_transport.cpp
    implementations/qt_remote/remote_transport.h
    implementations/qt_remote/qt_remote_registry.h
    implementations/qt_remote/qt_remote_registry.cpp
    implementations/mock/mock_store.cpp
    implementations/mock/mock_store.h
    implementations/mock/mock_transport.cpp
    implementations/mock/mock_transport.h
    implementations/mock/mock_registry.h
    implementations/mock/logos_mock.h
    # Plain-C++ wire stack (no Qt)
    implementations/plain/rpc_value.h
    implementations/plain/rpc_message.h
    implementations/plain/rpc_message.cpp
    implementations/plain/wire_codec.h
    implementations/plain/json_mapping.h
    implementations/plain/json_mapping.cpp
    implementations/plain/json_codec.h
    implementations/plain/json_codec.cpp
    implementations/plain/cbor_codec.h
    implementations/plain/cbor_codec.cpp
    implementations/plain/rpc_framing.h
    implementations/plain/rpc_framing.cpp
    implementations/plain/incoming_call_handler.h
    implementations/plain/rpc_connection.h
    implementations/plain/rpc_server.h
    implementations/plain/rpc_server.cpp
    implementations/plain/io_context_pool.h
    implementations/plain/io_context_pool.cpp
    implementations/plain/qvariant_rpc_value.h
    implementations/plain/qvariant_rpc_value.cpp
    implementations/plain/plain_logos_object.h
    implementations/plain/plain_logos_object.cpp
    implementations/plain/plain_transport_host.h
    implementations/plain/plain_transport_host.cpp
    implementations/plain/plain_transport_connection.h
    implementations/plain/plain_transport_connection.cpp
)

add_library(logos_protocol STATIC ${PROTOCOL_SOURCES})

# Link Qt + plain-C++ transport deps. Boost.Asio is header-only
# *except* for `boost::system::error_code` (and friends), which lives
# in libboost_system — see the matching note in logos-cpp-sdk.
target_link_libraries(logos_protocol PUBLIC
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::RemoteObjects
    Boost::headers
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
)

target_include_directories(logos_protocol PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/implementations/qt_local>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/implementations/qt_remote>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/implementations/mock>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/implementations/plain>
    $<INSTALL_INTERFACE:include>
    $<INSTALL_INTERFACE:include/implementations/qt_local>
    $<INSTALL_INTERFACE:include/implementations/qt_remote>
    $<INSTALL_INTERFACE:include/implementations/mock>
    $<INSTALL_INTERFACE:include/implementations/plain>
)

set_target_properties(logos_protocol PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

install(TARGETS logos_protocol
    EXPORT logos-protocolTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

# Shared variant of the same sources, exporting the language-neutral lp_*
# C ABI (extern "C", default visibility) as a loadable library. This exists
# purely for OUT-OF-PLUGIN callers that bind lp_* at runtime via dlopen/FFI
# (e.g. logos-js-sdk's koffi.load, logos-rust-sdk's callerBuildSupport) —
# the role liblogos_module_client previously filled. In-plugin code keeps
# linking the STATIC `logos_protocol` archive through the EXPORT set below,
# so this target is intentionally NOT exported and NOT installed into the
# CMake package; `find_package(logos-protocol)` is unaffected.
add_library(logos_protocol_shared SHARED ${PROTOCOL_SOURCES})

target_link_libraries(logos_protocol_shared PUBLIC
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::RemoteObjects
    Boost::headers
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
)

target_include_directories(logos_protocol_shared PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/implementations/qt_local>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/implementations/qt_remote>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/implementations/mock>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/implementations/plain>
)

# Same basename as the archive (liblogos_protocol.{so,dylib}); the LIBRARY
# install below lands it in $out/lib next to liblogos_protocol.a.
set_target_properties(logos_protocol_shared PROPERTIES
    OUTPUT_NAME logos_protocol
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

install(TARGETS logos_protocol_shared
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(EXPORT logos-protocolTargets
    FILE logos-protocolTargets.cmake
    NAMESPACE logos-protocol::
    DESTINATION lib/cmake/logos-protocol
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/logos-protocolConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/logos-protocolConfig.cmake"
    INSTALL_DESTINATION lib/cmake/logos-protocol
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/logos-protocolConfigVersion.cmake"
    VERSION 0.1.0
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/logos-protocolConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/logos-protocolConfigVersion.cmake"
    DESTINATION lib/cmake/logos-protocol
)

# Install headers. Moved headers keep the exact relative paths they had when
# they shipped from logos-cpp-sdk, so downstream `#include "token_manager.h"`
# (etc.) keep resolving unchanged through propagated include dirs.
install(FILES
    logos_protocol.h
    logos_module_impl.h
    # The canonical LIDL <-> JSON codec. Generated module glue includes it, so it
    # has to ship with the library's headers, not just in the source export.
    logos_codec.h
    logos_call_error.h
    logos_rpc_status.h
    logos_types.h
    logos_api_client.h
    logos_api_consumer.h
    module_proxy.h
    token_manager.h
    logos_mode.h
    logos_instance.h
    logos_socket_paths.h
    plugin_registry.h
    logos_object.h
    logos_thread_marshal.h
    logos_provider_interface.h
    logos_json_convert.h
    # The Qt face of the codec. Generated provider dispatch includes it, so it
    # ships with the library's headers for the same reason logos_codec.h does.
    logos_transport.h
    logos_transport_config.h
    logos_transport_config_json.h
    logos_transport_factory.h
    logos_registry.h
    logos_registry_factory.h
    DESTINATION include
)

install(FILES
    implementations/plain/rpc_value.h
    implementations/plain/rpc_message.h
    implementations/plain/wire_codec.h
    implementations/plain/json_codec.h
    implementations/plain/rpc_framing.h
    DESTINATION include/implementations/plain
)

install(FILES
    implementations/qt_local/local_transport.h
    DESTINATION include/implementations/qt_local
)

install(FILES
    implementations/qt_remote/remote_transport.h
    implementations/qt_remote/qt_remote_registry.h
    DESTINATION include/implementations/qt_remote
)

install(FILES
    implementations/mock/mock_store.h
    implementations/mock/mock_transport.h
    implementations/mock/mock_registry.h
    implementations/mock/logos_mock.h
    DESTINATION include/implementations/mock
)
