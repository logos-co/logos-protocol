# Install script for directory: /private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/var/empty/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/nix/store/zzx6cfl99zknfrj9v7lmrshwzslfjv26-clang-wrapper-19.1.7/bin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/bt/lib/liblogos_protocol.a")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/liblogos_protocol.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/liblogos_protocol.a")
    execute_process(COMMAND "/nix/store/zzx6cfl99zknfrj9v7lmrshwzslfjv26-clang-wrapper-19.1.7/bin/ranlib" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/liblogos_protocol.a")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE SHARED_LIBRARY FILES "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/bt/lib/liblogos_protocol.dylib")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/liblogos_protocol.dylib" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/liblogos_protocol.dylib")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/nix/store/zzx6cfl99zknfrj9v7lmrshwzslfjv26-clang-wrapper-19.1.7/bin/strip" -x "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/liblogos_protocol.dylib")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/logos-protocol/logos-protocolTargets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/logos-protocol/logos-protocolTargets.cmake"
         "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/bt/logos_protocol/CMakeFiles/Export/993c26c9fb1446783cb17c304acec915/logos-protocolTargets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/logos-protocol/logos-protocolTargets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/logos-protocol/logos-protocolTargets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/logos-protocol" TYPE FILE FILES "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/bt/logos_protocol/CMakeFiles/Export/993c26c9fb1446783cb17c304acec915/logos-protocolTargets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^()$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/logos-protocol" TYPE FILE FILES "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/bt/logos_protocol/CMakeFiles/Export/993c26c9fb1446783cb17c304acec915/logos-protocolTargets-noconfig.cmake")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/logos-protocol" TYPE FILE FILES
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/bt/logos_protocol/logos-protocolConfig.cmake"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/bt/logos_protocol/logos-protocolConfigVersion.cmake"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE FILE FILES
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/logos_protocol.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/logos_module_impl.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/logos_codec.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/logos_call_error.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/logos_rpc_status.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/logos_shared_api.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/logos_types.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/logos_api_client.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/logos_api_consumer.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/logos_subscription_state.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/module_proxy.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/logos_caller_scope.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/token_manager.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/logos_mode.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/logos_instance.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/logos_socket_paths.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/plugin_registry.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/logos_object.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/logos_thread_marshal.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/logos_provider_interface.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/logos_json_convert.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/logos_transport.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/logos_transport_config.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/logos_transport_config_json.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/logos_transport_factory.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/logos_registry.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/logos_registry_factory.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/implementations/plain" TYPE FILE FILES
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/implementations/plain/rpc_value.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/implementations/plain/rpc_message.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/implementations/plain/wire_codec.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/implementations/plain/json_codec.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/implementations/plain/rpc_framing.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/implementations/qt_local" TYPE FILE FILES "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/implementations/qt_local/local_transport.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/implementations/qt_remote" TYPE FILE FILES
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/implementations/qt_remote/remote_transport.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/implementations/qt_remote/qt_remote_registry.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/implementations/mock" TYPE FILE FILES
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/implementations/mock/mock_store.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/implementations/mock/mock_transport.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/implementations/mock/mock_registry.h"
    "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/cpp/implementations/mock/logos_mock.h"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/private/tmp/claude-501/-Users-dlipicar-repos-logos-workspace/3967c77d-93dd-4877-b52f-2cb26918787b/scratchpad/lp/bt/logos_protocol/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
