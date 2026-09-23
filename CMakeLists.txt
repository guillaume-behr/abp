cmake_minimum_required(VERSION 3.16)

project(abp
    VERSION 1.1.0
    DESCRIPTION "ADB-based full backup and restore tool for Android devices"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

option(ABP_BUILD_TESTS "Build the abp unit test suite" ON)
option(ABP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/abp/Version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/abp/Version.h"
    @ONLY
)

add_subdirectory(src)

if(ABP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(TARGETS abp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES README.md LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
