cmake_minimum_required(VERSION 3.20)
project(seekdb_driver C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_FLAGS_RELEASE   "-O2 -DNDEBUG" CACHE STRING "")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "")

include(CTest)

find_program(CLANG_FORMAT_EXE NAMES clang-format)

if (WIN32)
    add_compile_definitions(WIN32_LEAN_AND_MEAN)
endif()

if (ANDROID AND DEFINED OPENSSL_ROOT_DIR)
    list(APPEND CMAKE_FIND_ROOT_PATH "${OPENSSL_ROOT_DIR}")
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(SEEKDB_DRIVER_ENABLE_LOG "Enable tlog() debug prints in seekdb_driver" OFF)

set(SEEKDB_BIN "$ENV{SEEKDB_BIN}" CACHE FILEPATH "Path to the seekdb server binary")

set(WITH_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_CURL       OFF CACHE BOOL "" FORCE)
set(WITH_DOCS       OFF CACHE BOOL "" FORCE)
if(WIN32)
    set(ZLIB_LIBRARY zlib CACHE STRING "" FORCE)
endif()

include(GNUInstallDirs)
set(INSTALL_PLUGINDIR "lib/mariadb/plugin")
set(INSTALL_LIBDIR    "lib")
add_subdirectory(deps/mariadb-connector-c EXCLUDE_FROM_ALL)

add_library(mariadb_client INTERFACE)
target_include_directories(mariadb_client INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/deps/mariadb-connector-c/include"
    "${CMAKE_CURRENT_BINARY_DIR}/deps/mariadb-connector-c/include")
target_link_libraries(mariadb_client INTERFACE mariadbclient)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
add_subdirectory(deps/googletest EXCLUDE_FROM_ALL)
include(GoogleTest)

add_subdirectory(driver)
add_subdirectory(python)

if (CLANG_FORMAT_EXE)
    file(GLOB SEEKDB_CLANG_FORMAT_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/driver/include/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/driver/src/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/driver/src/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/driver/tests/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/driver/tests/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/python/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/python/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/python/src/*.h")
    add_custom_target(format
        COMMAND "${CLANG_FORMAT_EXE}" -i ${SEEKDB_CLANG_FORMAT_FILES}
        COMMENT "Running clang-format on project sources"
        VERBATIM)
endif()
