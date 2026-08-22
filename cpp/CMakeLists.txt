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

# Plain-C++ transport dependencies (no Qt).
#
# Boost.System is header-only, but Boost 1.87 only EXPORTS the Boost::system
# imported target when the component is requested -- hence the explicit
# COMPONENTS below. Boost 1.89 then removed the compiled boost_system library
# outright, so there is no boost_systemConfig.cmake at all and requesting it as
# REQUIRED hard-fails ("Could not find a package configuration file provided by
# boost_system"). Ask for it optionally and fall back to Boost::headers, which
# carries the same header-only error_code either way.
find_package(Boost REQUIRED)
find_package(Boost QUIET OPTIONAL_COMPONENTS system)
if(TARGET Boost::system)
    set(LOGOS_BOOST_SYSTEM Boost::system)
else()
    set(LOGOS_BOOST_SYSTEM Boost::headers)
endif()
message(STATUS "Boost.System target: ${LOGOS_BOOST_SYSTEM} (Boost ${Boost_VERSION})")
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)

set(PROTOCOL_SOURCES
    logos_protocol.h
    logos_module_impl.h
    logos_call_error.h
    logos_shared_api.h
    logos_protocol.cpp
    logos_types.cpp
    logos_types.h
    logos_api_client.cpp
    logos_api_client.h
    logos_api_consumer.cpp
    logos_api_consumer.h
    logos_subscription_state.h
    module_proxy.cpp
    module_proxy.h
    # WHO is calling the dispatch on this thread. Produced by
    # ModuleProxy::authorize, consumed by the host-side invokable the
    # generated glue reaches by name.
    logos_caller_scope.cpp
    logos_caller_scope.h
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
# Boost.Asio's IOCP backend calls the Winsock EXTENSION functions AcceptEx and
# GetAcceptExSockaddrs, which live in mswsock rather than ws2_32. Without this
# the plain TCP transport compiles cleanly and then fails at link with
# "undefined reference to `AcceptEx'" out of
# win_iocp_socket_service_base::start_accept_op. ws2_32 supplies core Winsock
# and is listed explicitly rather than relied upon arriving via Qt::Network.
if(WIN32)
    set(LOGOS_PROTOCOL_PLATFORM_LIBS ws2_32 mswsock)
else()
    set(LOGOS_PROTOCOL_PLATFORM_LIBS "")
endif()

target_link_libraries(logos_protocol PUBLIC
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::RemoteObjects
    Boost::headers
    ${LOGOS_BOOST_SYSTEM}
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    ${LOGOS_PROTOCOL_PLATFORM_LIBS}
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
# linking the STATIC `logos_protocol` archive through the EXPORT set below.
#
# This target is EXPORTED as of this commit, where it was previously installed
# but deliberately kept out of the CMake package. The reason it now has to be is
# a second class of consumer: IN-PROCESS C++ images that link it as the single
# provider of the runtime types which must exist exactly once per process
# (TokenManager, LogosAPIClient, the per-identity StoreRegistry). Linking one
# shared library is what replaces the whole-archive + generated-.def scheme, in
# which liblogos_core absorbed both static archives and re-exported them. A
# consumer cannot link what find_package() does not hand it.
#
# OUT-OF-PROCESS consumers keep linking the STATIC archive, and that is not a
# transitional state: module plugins and ui_qml backends run in their own
# processes, so their own copy of TokenManager is the CORRECT per-process
# singleton. Staying static also keeps a .lgx self-contained -- a .lgx records
# an empty nix closure, so a shared library would not travel with it -- and
# keeps those plugins immune to ABI skew against a separately-updated .so.
add_library(logos_protocol_shared SHARED ${PROTOCOL_SOURCES})

target_link_libraries(logos_protocol_shared PUBLIC
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::RemoteObjects
    Boost::headers
    ${LOGOS_BOOST_SYSTEM}
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    ${LOGOS_PROTOCOL_PLATFORM_LIBS}
)

# Only the SHARED build marks the lp_* C ABI dllexport; the static archive
# leaves LP_API empty so consumers linking it need no import library.
target_compile_definitions(logos_protocol_shared PRIVATE LOGOS_PROTOCOL_BUILDING_SHARED)

# The INSTALL_INTERFACE half mirrors the static target exactly. Without it an
# exported target carries no include directories at all, and the failure is a
# confusing one: the target imports fine and the link succeeds, then the
# consumer fails to COMPILE on `#include "token_manager.h"`.
target_include_directories(logos_protocol_shared PUBLIC
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

# Same basename as the archive (liblogos_protocol.{so,dylib}); the LIBRARY
# install below lands it in $out/lib next to liblogos_protocol.a.
set_target_properties(logos_protocol_shared PROPERTIES
    OUTPUT_NAME logos_protocol
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# ARCHIVE DESTINATION is NOT redundant here. On Windows a shared library's
# import library (.dll.a) is the ARCHIVE artifact and the DLL itself is RUNTIME,
# so without an ARCHIVE destination the import library is never installed and a
# consumer that find_package()s this target gets an imported SHARED target whose
# IMPORTED_IMPLIB does not exist. It links fine on ELF and Mach-O, where there is
# no import library at all, and fails only on Windows.
# PE export table for the shared library, generated from the objects.
#
# Not needed off Windows: ELF and Mach-O export a shared library's non-hidden
# symbols by default, which is exactly why the shortfall this replaces was
# invisible until a Windows link.
if(WIN32)
    # CMAKE_NM is normally set by the toolchain file; fall back to the
    # cross-prefixed binary so a plain `cmake -DCMAKE_TOOLCHAIN_FILE=...` still
    # generates. A missing nm must fail HERE, loudly, rather than silently
    # emitting an empty .def -- an empty export table links clean and fails in a
    # downstream repo as an undefined reference.
    set(_lp_nm "${CMAKE_NM}")
    if(NOT _lp_nm)
        find_program(_lp_nm NAMES "${CMAKE_CXX_COMPILER_TARGET}-nm" x86_64-w64-mingw32-nm nm)
    endif()
    if(NOT _lp_nm)
        message(FATAL_ERROR
            "nm not found, so liblogos_protocol.dll cannot be given an export "
            "list. Windows requires one: PE exports nothing that is not named, "
            "and a consumer linking the empty stand-in archive would then fail "
            "with undefined references to the entire C++ runtime.")
    endif()

    # Generated from the STATIC archive, which is built from the same sources.
    # "*" means every member: ld chooses archive members for reasons unrelated to
    # our symbols, so a partial set is a downstream undefined reference.
    set(_lp_def "${CMAKE_CURRENT_BINARY_DIR}/logos_protocol_shared_exports.def")
    add_custom_command(
        OUTPUT "${_lp_def}"
        COMMAND ${CMAKE_COMMAND} -E env sh
                "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/gen-shared-exports.sh"
                "${_lp_nm}" "${_lp_def}"
                "$<TARGET_FILE:logos_protocol>" "*"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/gen-shared-exports.sh" logos_protocol
        COMMENT "Generating liblogos_protocol export definition"
        VERBATIM
    )
    add_custom_target(logos_protocol_shared_exports DEPENDS "${_lp_def}")
    add_dependencies(logos_protocol_shared logos_protocol_shared_exports)
    target_link_options(logos_protocol_shared PRIVATE "${_lp_def}")
    set_property(TARGET logos_protocol_shared APPEND PROPERTY LINK_DEPENDS "${_lp_def}")
endif()

install(TARGETS logos_protocol_shared
    EXPORT logos-protocolTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
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
    # Windows single-provider marker. token_manager.h / logos_api_client.h /
    # logos_types.h include it, and so does logos-qt-sdk's logos_api.h, so it
    # has to ship even though nothing in this library's own sources needs it.
    logos_shared_api.h
    logos_types.h
    logos_api_client.h
    logos_api_consumer.h
    # Named by logos_api_client.h and logos_api_consumer.h alike; the client only
    # forward-declares the consumer, so the enum cannot live in the consumer header.
    logos_subscription_state.h
    module_proxy.h
    # Included by logos-plugin-qt's logos_api.cpp to answer
    # LogosAPI::currentCallerJson(), so it has to ship with the headers.
    logos_caller_scope.h
    token_manager.h
    logos_mode.h
    logos_instance.h
    logos_socket_paths.h
    plugin_registry.h
    logos_object.h
    logos_thread_marshal.h
    logos_provider_interface.h
    logos_json_convert.h
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
