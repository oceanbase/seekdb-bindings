cmake_minimum_required(VERSION 3.18)

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

# ------ seekdb_driver: in-tree target (subdirectory) or prebuilt lib (standalone) ------
if (SEEKDB_PYTHON_STANDALONE)
    set(SEEKDB_DRIVER_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build" CACHE PATH
        "Build dir containing the prebuilt seekdb_driver library and seekdb binary")

    set(_seekdb_lib_name "${CMAKE_SHARED_LIBRARY_PREFIX}seekdb${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set(_seekdb_compat_lib_name
        "${CMAKE_SHARED_LIBRARY_PREFIX}seekdb_driver${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set(_seekdb_dir "")
    foreach(_cand
            "${SEEKDB_DRIVER_LIB_DIR}"
            "${SEEKDB_DRIVER_LIB_DIR}/Debug"
            "${SEEKDB_DRIVER_LIB_DIR}/Release"
            "${SEEKDB_DRIVER_LIB_DIR}/RelWithDebInfo"
            "${SEEKDB_DRIVER_LIB_DIR}/MinSizeRel")
        if (EXISTS "${_cand}/${_seekdb_lib_name}")
            set(_seekdb_dir "${_cand}")
            break()
        elseif (EXISTS "${_cand}/${_seekdb_compat_lib_name}")
            set(_seekdb_dir "${_cand}")
            set(_seekdb_lib_name "${_seekdb_compat_lib_name}")
            break()
        endif()
    endforeach()
    if (_seekdb_dir STREQUAL "")
        message(FATAL_ERROR
            "${CMAKE_SHARED_LIBRARY_PREFIX}seekdb${CMAKE_SHARED_LIBRARY_SUFFIX} not found under ${SEEKDB_DRIVER_LIB_DIR} (or its Debug/Release subdirs).\n"
            "Build the top-level project first, or pass\n"
            "    -DSEEKDB_DRIVER_LIB_DIR=<path-to-build-dir>")
    endif()

    set(_seekdb_driver_lib "${_seekdb_dir}/${_seekdb_lib_name}")
    set(_seekdb_compat_lib "${_seekdb_dir}/${_seekdb_compat_lib_name}")
    set(_seekdb_bin        "${_seekdb_dir}/seekdb${CMAKE_EXECUTABLE_SUFFIX}")
    if (WIN32)
        set(_seekdb_driver_link
            "${_seekdb_dir}/${CMAKE_IMPORT_LIBRARY_PREFIX}seekdb${CMAKE_IMPORT_LIBRARY_SUFFIX}")
        if (NOT EXISTS "${_seekdb_driver_link}")
            set(_seekdb_driver_link
                "${_seekdb_dir}/${CMAKE_IMPORT_LIBRARY_PREFIX}seekdb_driver${CMAKE_IMPORT_LIBRARY_SUFFIX}")
        endif()
    else()
        set(_seekdb_driver_link "${_seekdb_driver_lib}")
    endif()

    if (NOT EXISTS "${_seekdb_bin}")
        message(FATAL_ERROR
            "seekdb binary not found at ${_seekdb_bin}.\n"
            "Configure the top-level build with -DSEEKDB_BIN=<path-to-seekdb>.")
    endif()
else()
    set(_seekdb_driver_link seekdb_driver)
    set(_seekdb_driver_lib  "$<TARGET_FILE:seekdb_driver>")
    set(_seekdb_bin         "${SEEKDB_BIN}")
endif()

# ------ Build the extension ------
pybind11_add_module(pylibseekdb src/bindings.cpp)

target_include_directories(pylibseekdb PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../driver/include")
target_link_libraries(pylibseekdb PRIVATE
    "${_seekdb_driver_link}"
)

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
                "${_seekdb_driver_lib}" "$<TARGET_FILE_DIR:pylibseekdb>")
    if (EXISTS "${_seekdb_compat_lib}")
        add_custom_command(TARGET pylibseekdb POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_seekdb_compat_lib}" "$<TARGET_FILE_DIR:pylibseekdb>")
    endif()
    if (_seekdb_bin)
        add_custom_command(TARGET pylibseekdb POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_seekdb_bin}" "$<TARGET_FILE_DIR:pylibseekdb>")
    endif()
endif()

if (SEEKDB_PYTHON_STANDALONE)
    install(TARGETS pylibseekdb LIBRARY DESTINATION pylibseekdb)
    install(FILES "${_seekdb_driver_lib}"
            DESTINATION pylibseekdb)
    if (EXISTS "${_seekdb_compat_lib}")
        install(FILES "${_seekdb_compat_lib}"
                DESTINATION pylibseekdb)
    endif()
    install(PROGRAMS "${_seekdb_bin}"
            DESTINATION pylibseekdb)
    install(FILES "${SEEKDB_DRIVER_LIB_DIR}/libaio.so.1"
            DESTINATION pylibseekdb
            OPTIONAL)
    if (WIN32)
        file(GLOB _seekdb_runtime_dlls "${_seekdb_dir}/*.dll")
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
        DEPENDS seekdb_driver
        COMMENT "Building Python wheel via cibuildwheel"
        VERBATIM)
endif()
