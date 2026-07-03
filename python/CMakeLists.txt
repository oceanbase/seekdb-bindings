cmake_minimum_required(VERSION 3.18)

# True when python/ is the CMake source root (pip / cibuildwheel). False when
# configured as a subdirectory of the top-level seekdb-bindings project.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(SEEKDB_PYTHON_STANDALONE TRUE)
else()
    set(SEEKDB_PYTHON_STANDALONE FALSE)
endif()

if (SEEKDB_PYTHON_STANDALONE)
    project(python-client LANGUAGES CXX)

    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
    set(CMAKE_C_FLAGS_RELEASE   "-O2 -DNDEBUG" CACHE STRING "" FORCE)
    set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "" FORCE)
endif()

# ------ Find Python + pybind11 ------
find_package(Python3 COMPONENTS Interpreter Development.Module REQUIRED)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c "import pybind11; print(pybind11.get_cmake_dir())"
    OUTPUT_VARIABLE pybind11_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _pybind11_rc
)
if(NOT _pybind11_rc EQUAL 0)
    message(FATAL_ERROR "pybind11 not importable in ${Python3_EXECUTABLE}. Install with: pip install --user pybind11")
endif()
message(STATUS "pybind11 cmake dir: ${pybind11_DIR}")
find_package(pybind11 CONFIG REQUIRED)

# ------ libseekdb: in-tree target or prebuilt IMPORTED lib (standalone) ------
if (SEEKDB_PYTHON_STANDALONE)
    set(SEEKDB_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build" CACHE PATH
        "Directory containing libseekdb and the seekdb server binary")

    set(_seekdb_lib_name "${CMAKE_SHARED_LIBRARY_PREFIX}seekdb${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set(_seekdb_lib "${SEEKDB_LIB_DIR}/${_seekdb_lib_name}")
    set(_seekdb_bin "${SEEKDB_LIB_DIR}/seekdb${CMAKE_EXECUTABLE_SUFFIX}")

    if (NOT EXISTS "${_seekdb_lib}")
        message(FATAL_ERROR
            "${_seekdb_lib} not found.\n"
            "Build the top-level project first, or pass -DSEEKDB_LIB_DIR=<build-dir>")
    endif()
    if (NOT EXISTS "${_seekdb_bin}")
        message(FATAL_ERROR
            "seekdb binary not found at ${_seekdb_bin}.\n"
            "Configure the top-level build with -DSEEKDB_BIN=<path-to-seekdb>.")
    endif()

    add_library(seekdb SHARED IMPORTED GLOBAL)
    set_target_properties(seekdb PROPERTIES IMPORTED_LOCATION "${_seekdb_lib}")
    if (WIN32)
        set(_seekdb_implib
            "${SEEKDB_LIB_DIR}/${CMAKE_IMPORT_LIBRARY_PREFIX}seekdb${CMAKE_IMPORT_LIBRARY_SUFFIX}")
        if (NOT EXISTS "${_seekdb_implib}")
            message(FATAL_ERROR "import library not found: ${_seekdb_implib}")
        endif()
        set_target_properties(seekdb PROPERTIES IMPORTED_IMPLIB "${_seekdb_implib}")
    endif()
else()
    set(_seekdb_lib "$<TARGET_FILE:seekdb>")
    set(_seekdb_bin "${SEEKDB_BIN}")
endif()

# ------ Build the extension ------
pybind11_add_module(pylibseekdb src/bindings.cpp)

target_include_directories(pylibseekdb PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../lib/include")
target_link_libraries(pylibseekdb PRIVATE seekdb)

if(APPLE)
    set(_seekdb_rpath "@loader_path")
else()
    set(_seekdb_rpath "\$ORIGIN")
endif()
set_target_properties(pylibseekdb PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    INSTALL_RPATH            "${_seekdb_rpath}"
    BUILD_RPATH              "${_seekdb_rpath}"
    BUILD_WITH_INSTALL_RPATH TRUE
)

if (SEEKDB_PYTHON_STANDALONE)
    add_custom_command(TARGET pylibseekdb POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_seekdb_lib}" "$<TARGET_FILE_DIR:pylibseekdb>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_seekdb_bin}" "$<TARGET_FILE_DIR:pylibseekdb>")
endif()

if (SEEKDB_PYTHON_STANDALONE)
    install(TARGETS pylibseekdb LIBRARY DESTINATION pylibseekdb)
    install(FILES "${_seekdb_lib}" DESTINATION pylibseekdb)
    install(PROGRAMS "${_seekdb_bin}" DESTINATION pylibseekdb)
    install(FILES "${SEEKDB_LIB_DIR}/libaio.so.1"
            DESTINATION pylibseekdb
            OPTIONAL)
    if (WIN32)
        file(GLOB _seekdb_runtime_dlls "${SEEKDB_LIB_DIR}/*.dll")
        install(FILES ${_seekdb_runtime_dlls} DESTINATION pylibseekdb)
    endif()
endif()

if (NOT SEEKDB_PYTHON_STANDALONE)
    add_custom_target(wheel
        COMMAND ${CMAKE_COMMAND} -E env
                "SEEKDB_BIN=${SEEKDB_BIN}"
                python -m cibuildwheel
                --platform auto
                --output-dir ${CMAKE_BINARY_DIR}/wheelhouse
                ${CMAKE_SOURCE_DIR}/python
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        DEPENDS seekdb
        COMMENT "Building Python wheel via cibuildwheel"
        VERBATIM)
endif()
