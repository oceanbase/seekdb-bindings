if(WIN32)
    set(_default_test_data_root "${CMAKE_CURRENT_BINARY_DIR}/test_data")
else()
    set(_default_test_data_root "/tmp/seekdb_tests")
endif()
set(SEEKDB_TEST_DATA_ROOT "${_default_test_data_root}" CACHE PATH
    "Root directory for test databases created by gtest cases")

if (NOT WIN32)
    add_executable(test_one_client_process test_one_client_process.cc)
    target_include_directories(test_one_client_process PRIVATE
        ${CMAKE_SOURCE_DIR}/driver/src)
    target_compile_definitions(test_one_client_process PRIVATE
        SEEKDB_TEST_DATA_ROOT="${SEEKDB_TEST_DATA_ROOT}")
    target_link_libraries(test_one_client_process PRIVATE
        seekdb_driver mariadb_client GTest::gtest_main)
    gtest_discover_tests(test_one_client_process DISCOVERY_MODE PRE_TEST)

    add_executable(test_parameter_persistence test_parameter_persistence.cc)
    target_include_directories(test_parameter_persistence PRIVATE
        ${CMAKE_SOURCE_DIR}/driver/src)
    target_compile_definitions(test_parameter_persistence PRIVATE
        SEEKDB_TEST_DATA_ROOT="${SEEKDB_TEST_DATA_ROOT}")
    target_link_libraries(test_parameter_persistence PRIVATE
        seekdb_driver mariadb_client GTest::gtest_main)
    gtest_discover_tests(test_parameter_persistence DISCOVERY_MODE PRE_TEST)
endif()

add_executable(test_two_clients_threads test_two_clients_threads.cc)
target_include_directories(test_two_clients_threads PRIVATE
    ${CMAKE_SOURCE_DIR}/driver/src)
target_compile_definitions(test_two_clients_threads PRIVATE
    SEEKDB_TEST_DATA_ROOT="${SEEKDB_TEST_DATA_ROOT}")
target_link_libraries(test_two_clients_threads PRIVATE
    seekdb_driver mariadb_client GTest::gtest_main)
gtest_discover_tests(test_two_clients_threads DISCOVERY_MODE PRE_TEST)

add_custom_target(seekdb_driver_tests DEPENDS test_two_clients_threads)
if (NOT WIN32)
    add_dependencies(seekdb_driver_tests test_one_client_process test_parameter_persistence)
endif()
