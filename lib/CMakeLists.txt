set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ----------------------------------------------------------------------------
# libseekdb shared library
# ----------------------------------------------------------------------------

add_library(seekdb SHARED
    src/seekdb.c
    src/port.c)

set_target_properties(seekdb PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

# Backward-compatible name: libseekdb_driver.{so,dylib,dll} -> libseekdb.{so,dylib,dll}
set(_seekdb_compat_name
    "${CMAKE_SHARED_LIBRARY_PREFIX}seekdb_driver${CMAKE_SHARED_LIBRARY_SUFFIX}")
set(_seekdb_real_name
    "${CMAKE_SHARED_LIBRARY_PREFIX}seekdb${CMAKE_SHARED_LIBRARY_SUFFIX}")
if (UNIX)
    add_custom_command(TARGET seekdb POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -f "${_seekdb_compat_name}"
        COMMAND ${CMAKE_COMMAND} -E create_symlink
                "${_seekdb_real_name}" "${_seekdb_compat_name}"
        WORKING_DIRECTORY $<TARGET_FILE_DIR:seekdb>
        COMMENT "Creating compatibility symlink ${_seekdb_compat_name} -> ${_seekdb_real_name}")
elseif (WIN32)
    set(_seekdb_compat_implib
        "${CMAKE_IMPORT_LIBRARY_PREFIX}seekdb_driver${CMAKE_IMPORT_LIBRARY_SUFFIX}")
    add_custom_command(TARGET seekdb POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:seekdb>"
                "$<TARGET_FILE_DIR:seekdb>/${_seekdb_compat_name}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_LINKER_FILE:seekdb>"
                "$<TARGET_LINKER_FILE_DIR:seekdb>/${_seekdb_compat_implib}"
        COMMENT "Creating compatibility copies ${_seekdb_compat_name} and ${_seekdb_compat_implib}")
endif()

target_include_directories(seekdb
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

if (NOT MSVC)
    target_compile_options(seekdb PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if (NOT WIN32)
    target_compile_definitions(seekdb PRIVATE _GNU_SOURCE)
endif()

if (SEEKDB_DRIVER_ENABLE_LOG)
    target_compile_definitions(seekdb PRIVATE SEEKDB_DRIVER_ENABLE_LOG)
endif()

target_link_libraries(seekdb PRIVATE mariadb_client Threads::Threads)

if (SEEKDB_BIN)
    if (NOT EXISTS "${SEEKDB_BIN}")
        message(FATAL_ERROR "SEEKDB_BIN does not exist: ${SEEKDB_BIN}")
    endif()
    add_custom_command(TARGET seekdb POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SEEKDB_BIN}" "$<TARGET_FILE_DIR:seekdb>")
    if (WIN32)
        get_filename_component(_seekdb_bin_dir "${SEEKDB_BIN}" DIRECTORY)
        file(GLOB _seekdb_runtime_dlls "${_seekdb_bin_dir}/*.dll")
        foreach(_dll ${_seekdb_runtime_dlls})
            add_custom_command(TARGET seekdb POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_dll}" "$<TARGET_FILE_DIR:seekdb>")
        endforeach()
    endif()
endif()

# ----------------------------------------------------------------------------
# CLI tool
# ----------------------------------------------------------------------------

add_executable(seekdb_cli src/seekdb_cli.c)
target_include_directories(seekdb_cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    "${CMAKE_SOURCE_DIR}/deps/mariadb-connector-c/include"
    "${CMAKE_BINARY_DIR}/deps/mariadb-connector-c/include")
target_link_libraries(seekdb_cli PRIVATE seekdb)

if(APPLE)
    set(_seekdb_origin "@loader_path")
else()
    set(_seekdb_origin "\$ORIGIN")
endif()
set_target_properties(seekdb_cli PROPERTIES
    INSTALL_RPATH         "${_seekdb_origin}"
    BUILD_RPATH           "${_seekdb_origin}"
    BUILD_WITH_INSTALL_RPATH TRUE)

# ----------------------------------------------------------------------------
# Tests
# ----------------------------------------------------------------------------

add_subdirectory(tests)
