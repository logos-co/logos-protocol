# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "")
  file(REMOVE_RECURSE
  "logos_protocol/CMakeFiles/logos_protocol_autogen.dir/AutogenUsed.txt"
  "logos_protocol/CMakeFiles/logos_protocol_autogen.dir/ParseCache.txt"
  "logos_protocol/CMakeFiles/logos_protocol_shared_autogen.dir/AutogenUsed.txt"
  "logos_protocol/CMakeFiles/logos_protocol_shared_autogen.dir/ParseCache.txt"
  "logos_protocol/logos_protocol_autogen"
  "logos_protocol/logos_protocol_shared_autogen"
  "protocol/CMakeFiles/protocol_noqt_tests_autogen.dir/AutogenUsed.txt"
  "protocol/CMakeFiles/protocol_noqt_tests_autogen.dir/ParseCache.txt"
  "protocol/CMakeFiles/protocol_tests_autogen.dir/AutogenUsed.txt"
  "protocol/CMakeFiles/protocol_tests_autogen.dir/ParseCache.txt"
  "protocol/protocol_noqt_tests_autogen"
  "protocol/protocol_tests_autogen"
  )
endif()
