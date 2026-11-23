if(NOT ANDROID)
    find_package(JNI REQUIRED)
endif()

add_library(seekdb_jni SHARED jni/seekdb_jni.c)
target_include_directories(seekdb_jni PRIVATE ${JNI_INCLUDE_DIRS})
target_link_libraries(seekdb_jni PRIVATE seekdb)
if(APPLE)
    set(_jni_origin "@loader_path")
else()
    set(_jni_origin "\$ORIGIN")
endif()
set_target_properties(seekdb_jni PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    INSTALL_RPATH "${_jni_origin}"
    BUILD_WITH_INSTALL_RPATH TRUE)
if(APPLE)
    set_target_properties(seekdb_jni PROPERTIES
        INSTALL_NAME_DIR "@rpath"
        BUILD_WITH_INSTALL_NAME_DIR TRUE)
endif()
