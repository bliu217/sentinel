add_executable(sentinel_tests
    cpu_math_test.cpp
    process_telemetry_test.cpp
    ring_buffer_test.cpp
    anomaly_detector_test.cpp
    event_store_test.cpp
    event_attribution_test.cpp
    persistence_test.cpp
    monitoring_test.cpp
)
set_target_properties(sentinel_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
target_link_libraries(sentinel_tests PRIVATE sentinel_engine sentinel_client GTest::gtest_main)
target_include_directories(sentinel_tests PRIVATE "${PROJECT_SOURCE_DIR}/engine/src")
add_executable(sentinel_test_owner monitoring_test_owner.cpp)
target_link_libraries(sentinel_test_owner PRIVATE sentinel_engine sentinel_client)
set_target_properties(sentinel_test_owner PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
add_dependencies(sentinel_tests sentinel_test_owner)
include(GoogleTest)
set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)
gtest_discover_tests(sentinel_tests)
