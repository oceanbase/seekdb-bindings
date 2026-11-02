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

option(SEEKDB_BUILD_PYTHON "Build the pylibseekdb Python extension" ON)

find_program(CLANG_FORMAT_EXE NAMES clang-format-14 clang-format)

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

# SeekDB uses server error numbers that overlap MariaDB Connector/C's private
# client error range (5001--5026). The pinned connector otherwise replaces a
# valid server ERR packet in that range with CR_MALFORMED_PACKET, losing the
# actual error and message. Build a private copy so the submodule stays intact;
# retain the connector's MySQL client-error and packet validation checks.
set(SEEKDB_MARIADB_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/deps/mariadb-connector-c")
set(SEEKDB_MARIADB_COPY "${CMAKE_CURRENT_BINARY_DIR}/seekdb-mariadb-source")
set(SEEKDB_MARIADB_LIB "${SEEKDB_MARIADB_SOURCE}/libmariadb/mariadb_lib.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SEEKDB_MARIADB_LIB}")
file(COPY "${SEEKDB_MARIADB_SOURCE}/" DESTINATION "${SEEKDB_MARIADB_COPY}"
     PATTERN ".git" EXCLUDE)
file(READ "${SEEKDB_MARIADB_LIB}" SEEKDB_MARIADB_CONTENT)
set(SEEKDB_MARIADB_OLD "if (IS_MYSQL_ERROR(last_errno) || IS_MARIADB_ERROR(last_errno))")
string(FIND "${SEEKDB_MARIADB_CONTENT}" "${SEEKDB_MARIADB_OLD}" SEEKDB_MARIADB_MATCH)
if(SEEKDB_MARIADB_MATCH EQUAL -1)
    message(FATAL_ERROR "Pinned MariaDB connector error handling changed; review SeekDB compatibility patch")
endif()
string(REPLACE "${SEEKDB_MARIADB_OLD}" "if (IS_MYSQL_ERROR(last_errno))"
       SEEKDB_MARIADB_PATCHED "${SEEKDB_MARIADB_CONTENT}")
# The pinned unpack_fields also leaks its temporary metadata rows and leaves
# errno zero if the MYSQL_FIELD allocation fails. All its callers pass mutable
# MYSQL handles; retain its private const signature and set the diagnostic only
# on this allocation-failure path. Do not change malformed-packet validation.
set(SEEKDB_MARIADB_FIELD_OLD [=[  field=result=(MYSQL_FIELD*) ma_alloc_root(alloc,sizeof(MYSQL_FIELD)*fields);
  if (!result)
    return(0);]=])
set(SEEKDB_MARIADB_FIELD_NEW [=[  field=result=(MYSQL_FIELD*) ma_alloc_root(alloc,sizeof(MYSQL_FIELD)*fields);
  if (!result)
  {
    free_rows(data);
    SET_CLIENT_ERROR((MYSQL *)mysql, CR_OUT_OF_MEMORY, SQLSTATE_UNKNOWN, 0);
    return(0);
  }]=])
string(FIND "${SEEKDB_MARIADB_PATCHED}" "${SEEKDB_MARIADB_FIELD_OLD}" SEEKDB_MARIADB_FIELD_MATCH)
if(SEEKDB_MARIADB_FIELD_MATCH EQUAL -1)
    message(FATAL_ERROR "Pinned MariaDB field allocation changed; review metadata OOM patch")
endif()
string(REPLACE "${SEEKDB_MARIADB_FIELD_OLD}" "${SEEKDB_MARIADB_FIELD_NEW}"
       SEEKDB_MARIADB_PATCHED "${SEEKDB_MARIADB_PATCHED}")
# Every protocol field string is required, including empty strings. An unchecked
# ma_strdup_root failure otherwise publishes successful but incomplete metadata
# (and can expose a NULL column name to language bindings). Use the existing
# unpack_fields cleanup path; the driver treats client OOM as a closed protocol.
set(SEEKDB_MARIADB_STRING_OLD [=[      *(char **)(((char *)field) + rset_field_offsets[i*2])=
        ma_strdup_root(alloc, (char *)row->data[i]);
      *(unsigned int *)(((char *)field) + rset_field_offsets[i*2+1])= lengths[i];]=])
set(SEEKDB_MARIADB_STRING_NEW [=[      *(char **)(((char *)field) + rset_field_offsets[i*2])=
        ma_strdup_root(alloc, (char *)row->data[i]);
      if (!*(char **)(((char *)field) + rset_field_offsets[i*2]))
      {
        SET_CLIENT_ERROR((MYSQL *)mysql, CR_OUT_OF_MEMORY, SQLSTATE_UNKNOWN, 0);
        goto error;
      }
      *(unsigned int *)(((char *)field) + rset_field_offsets[i*2+1])= lengths[i];]=])
string(FIND "${SEEKDB_MARIADB_PATCHED}" "${SEEKDB_MARIADB_STRING_OLD}" SEEKDB_MARIADB_STRING_MATCH)
if(SEEKDB_MARIADB_STRING_MATCH EQUAL -1)
    message(FATAL_ERROR "Pinned MariaDB field strings changed; review metadata OOM patch")
endif()
string(REPLACE "${SEEKDB_MARIADB_STRING_OLD}" "${SEEKDB_MARIADB_STRING_NEW}"
       SEEKDB_MARIADB_PATCHED "${SEEKDB_MARIADB_PATCHED}")
file(WRITE "${SEEKDB_MARIADB_COPY}/libmariadb/mariadb_lib.c" "${SEEKDB_MARIADB_PATCHED}")
add_subdirectory("${SEEKDB_MARIADB_COPY}" "${CMAKE_CURRENT_BINARY_DIR}/deps/mariadb-connector-c" EXCLUDE_FROM_ALL)

add_library(mariadb_client INTERFACE)
target_include_directories(mariadb_client INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/deps/mariadb-connector-c/include"
    "${CMAKE_CURRENT_BINARY_DIR}/deps/mariadb-connector-c/include")
target_link_libraries(mariadb_client INTERFACE mariadbclient)

if(BUILD_TESTING)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory(deps/googletest EXCLUDE_FROM_ALL)
    include(GoogleTest)
endif()

add_subdirectory(lib)
if(SEEKDB_BUILD_PYTHON)
    add_subdirectory(python)
endif()

if (CLANG_FORMAT_EXE)
    file(GLOB_RECURSE SEEKDB_CLANG_FORMAT_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/python/src/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/python/src/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/python/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/python/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/typescript/native/*.c")
    add_custom_target(format
        COMMAND "${CLANG_FORMAT_EXE}" -i ${SEEKDB_CLANG_FORMAT_FILES}
        COMMENT "Running clang-format on project sources"
        VERBATIM)
endif()
