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

# ------ Find Python + nanobind ------
find_package(Python 3.11
    REQUIRED COMPONENTS Interpreter Development.Module
    OPTIONAL_COMPONENTS Development.SABIModule)
execute_process(
    COMMAND "${Python_EXECUTABLE}" -c "import nanobind; print(nanobind.cmake_dir())"
    OUTPUT_VARIABLE nanobind_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _nanobind_rc
)
if(NOT _nanobind_rc EQUAL 0)
    message(FATAL_ERROR "nanobind not importable in ${Python_EXECUTABLE}. Install with: pip install nanobind")
endif()
message(STATUS "nanobind cmake dir: ${nanobind_DIR}")
find_package(nanobind CONFIG REQUIRED)

# ------ libseekdb: in-tree target or prebuilt IMPORTED lib (standalone) ------
if (SEEKDB_PYTHON_STANDALONE)
    set(SEEKDB_SDK_DIR "" CACHE PATH "Prebuilt C SDK including its complete runtime")
    if(SEEKDB_SDK_DIR)
        set(SEEKDB_LIB_DIR "${SEEKDB_SDK_DIR}/lib" CACHE PATH "Prebuilt C SDK libraries" FORCE)
    endif()
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
nanobind_add_module(pylibseekdb STABLE_ABI src/bindings.cpp)
if(DEFINED SKBUILD_PROJECT_VERSION_FULL)
    target_compile_definitions(pylibseekdb PRIVATE SEEKDB_PYTHON_VERSION="${SKBUILD_PROJECT_VERSION_FULL}")
endif()

if(SEEKDB_SDK_DIR)
    target_include_directories(pylibseekdb PRIVATE "${SEEKDB_SDK_DIR}/include")
else()
    target_include_directories(pylibseekdb PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../lib/include")
endif()
target_link_libraries(pylibseekdb PRIVATE seekdb)

if(APPLE)
    set(_seekdb_rpath "@loader_path")
else()
    set(_seekdb_rpath "\$ORIGIN")
endif()
if(SEEKDB_SDK_DIR)
    string(APPEND _seekdb_rpath "/bin")
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
    if(SEEKDB_SDK_DIR)
        install(DIRECTORY "${SEEKDB_SDK_DIR}/lib/" DESTINATION pylibseekdb/bin
                USE_SOURCE_PERMISSIONS PATTERN "*.lib" EXCLUDE PATTERN "*.a" EXCLUDE)
        foreach(_resource etc share libexec licenses)
            if(EXISTS "${SEEKDB_SDK_DIR}/${_resource}")
                install(DIRECTORY "${SEEKDB_SDK_DIR}/${_resource}" DESTINATION pylibseekdb
                        USE_SOURCE_PERMISSIONS)
            endif()
        endforeach()
        install(FILES "${SEEKDB_SDK_DIR}/BUILD-INFO.json" DESTINATION pylibseekdb)
    else()
        install(FILES "${_seekdb_lib}" DESTINATION pylibseekdb)
        install(PROGRAMS "${_seekdb_bin}" DESTINATION pylibseekdb)
        install(FILES "${SEEKDB_LIB_DIR}/libaio.so.1"
                DESTINATION pylibseekdb
                OPTIONAL)
        if (WIN32)
            file(GLOB _seekdb_runtime_dlls "${SEEKDB_LIB_DIR}/*.dll")
            install(FILES ${_seekdb_runtime_dlls} DESTINATION pylibseekdb)
        endif()
        # Without the SDK there is no engine bundle to install from; render the
        # bindings layer so the wheel still ships its own license materials.
        # install(CODE) takes exactly one code argument; statements are separated by \n.
        install(CODE "execute_process(COMMAND \"${Python_EXECUTABLE}\" \"${CMAKE_CURRENT_SOURCE_DIR}/../scripts/stage-license-bundle.py\" --output \"\${CMAKE_INSTALL_PREFIX}/pylibseekdb/licenses\" --version \"${SKBUILD_PROJECT_VERSION}\" RESULT_VARIABLE _seekdb_licenses_rc)\nif(NOT _seekdb_licenses_rc EQUAL 0)\n  message(FATAL_ERROR \"staging the bindings license bundle failed: \${_seekdb_licenses_rc}\")\nendif()")
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

    add_custom_target(verify-wheel
        COMMAND ${CMAKE_SOURCE_DIR}/scripts/verify-wheel.sh
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Verify wheels in build/wheelhouse with seekdb_test.py"
        VERBATIM)
endif()
