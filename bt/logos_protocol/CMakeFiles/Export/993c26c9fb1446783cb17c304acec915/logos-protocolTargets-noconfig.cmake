#----------------------------------------------------------------
# Generated CMake target import file.
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "logos-protocol::logos_protocol" for configuration ""
set_property(TARGET logos-protocol::logos_protocol APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(logos-protocol::logos_protocol PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_NOCONFIG "CXX"
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/lib/liblogos_protocol.a"
  )

list(APPEND _cmake_import_check_targets logos-protocol::logos_protocol )
list(APPEND _cmake_import_check_files_for_logos-protocol::logos_protocol "${_IMPORT_PREFIX}/lib/liblogos_protocol.a" )

# Import target "logos-protocol::logos_protocol_shared" for configuration ""
set_property(TARGET logos-protocol::logos_protocol_shared APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(logos-protocol::logos_protocol_shared PROPERTIES
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/lib/liblogos_protocol.dylib"
  IMPORTED_SONAME_NOCONFIG "@rpath/liblogos_protocol.dylib"
  )

list(APPEND _cmake_import_check_targets logos-protocol::logos_protocol_shared )
list(APPEND _cmake_import_check_files_for_logos-protocol::logos_protocol_shared "${_IMPORT_PREFIX}/lib/liblogos_protocol.dylib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
