cmake_minimum_required(VERSION 3.14)
project(LogosProtocolTests)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(LOGOS_PROTOCOL_BUILD_QT
    "Build the legacy Qt adapters and Qt-backed protocol tests" ON)

find_package(GTest REQUIRED)
if(LOGOS_PROTOCOL_BUILD_QT)
    set(CMAKE_AUTOMOC ON)
    find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core RemoteObjects)
endif()

enable_testing()
include(GoogleTest)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../cpp ${CMAKE_CURRENT_BINARY_DIR}/logos_protocol)

add_subdirectory(protocol)
