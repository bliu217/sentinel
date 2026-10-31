add_executable(sentinel_tests
    cpu_math_test.cpp
    process_telemetry_test.cpp
    ring_buffer_test.cpp
    anomaly_detector_test.cpp
    event_store_test.cpp
    event_attribution_test.cpp
    persistence_test.cpp
)
set_target_properties(sentinel_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
target_link_libraries(sentinel_tests PRIVATE sentinel_engine GTest::gtest_main)
include(GoogleTest)
set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)
gtest_discover_tests(sentinel_tests)
