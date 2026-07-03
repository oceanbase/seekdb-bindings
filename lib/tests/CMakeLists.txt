if(WIN32)
    set(SEEKDB_TEST_DATA_ROOT "${CMAKE_CURRENT_BINARY_DIR}/test_data")
else()
    set(SEEKDB_TEST_DATA_ROOT "/tmp/seekdb_tests")
endif()

set(_seekdb_test_includes
    ${CMAKE_SOURCE_DIR}/lib/src
    "${CMAKE_SOURCE_DIR}/deps/mariadb-connector-c/include"
    "${CMAKE_BINARY_DIR}/deps/mariadb-connector-c/include")

if (NOT WIN32)
    add_executable(test_one_client_process test_one_client_process.cc)
    target_include_directories(test_one_client_process PRIVATE ${_seekdb_test_includes})
    target_compile_definitions(test_one_client_process PRIVATE
        SEEKDB_TEST_DATA_ROOT="${SEEKDB_TEST_DATA_ROOT}")
    target_link_libraries(test_one_client_process PRIVATE
        seekdb GTest::gtest_main)
    gtest_discover_tests(test_one_client_process DISCOVERY_MODE PRE_TEST)

    add_executable(test_parameter_persistence test_parameter_persistence.cc)
    target_include_directories(test_parameter_persistence PRIVATE ${_seekdb_test_includes})
    target_compile_definitions(test_parameter_persistence PRIVATE
        SEEKDB_TEST_DATA_ROOT="${SEEKDB_TEST_DATA_ROOT}")
    target_link_libraries(test_parameter_persistence PRIVATE
        seekdb GTest::gtest_main)
    gtest_discover_tests(test_parameter_persistence DISCOVERY_MODE PRE_TEST)
endif()

add_executable(test_two_clients_threads test_two_clients_threads.cc)
target_include_directories(test_two_clients_threads PRIVATE ${_seekdb_test_includes})
target_compile_definitions(test_two_clients_threads PRIVATE
    SEEKDB_TEST_DATA_ROOT="${SEEKDB_TEST_DATA_ROOT}")
target_link_libraries(test_two_clients_threads PRIVATE
    seekdb GTest::gtest_main)
gtest_discover_tests(test_two_clients_threads DISCOVERY_MODE PRE_TEST)

add_custom_target(seekdb_tests DEPENDS test_two_clients_threads)
if (NOT WIN32)
    add_dependencies(seekdb_tests test_one_client_process test_parameter_persistence)
endif()
