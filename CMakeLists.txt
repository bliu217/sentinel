cmake_minimum_required(VERSION 3.20)
project(sentinel LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

include(FetchContent)
FetchContent_Declare(sqlite_amalgamation
    URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
    URL_HASH SHA256=77823cb110929c2bcb0f5d48e4833b5c59a8a6e40cdea3936b99e199dbbe5784
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(sqlite_amalgamation)
add_library(sentinel_sqlite STATIC "${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c")
target_include_directories(sentinel_sqlite PUBLIC "${sqlite_amalgamation_SOURCE_DIR}")

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.zip
    URL_HASH SHA256=04022b05d806eb5ff73023c280b68697d12b93e1b7267a0b22a1a39ec7578069
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

add_library(sentinel_telemetry STATIC
    src/telemetry/system_collector.cpp
    src/telemetry/process_collector.cpp
    src/telemetry/process_tracker.cpp
    src/telemetry/process_aggregator.cpp
    src/storage/ring_buffer.cpp
    src/storage/process_history.cpp
    src/storage/event_store.cpp
    src/storage/sqlite_store.cpp
    src/storage/archive_manager.cpp
    src/attribution/anomaly_event.cpp
    src/detection/anomaly_detector.cpp
)
target_include_directories(sentinel_telemetry PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(sentinel_telemetry PUBLIC sentinel_sqlite nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(sentinel_telemetry PUBLIC Psapi)
endif()

if(MSVC)
    target_compile_options(sentinel_telemetry PUBLIC /W4 /permissive-)
    target_compile_definitions(sentinel_telemetry PUBLIC
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()

add_executable(sentinel src/main.cpp)
target_link_libraries(sentinel PRIVATE sentinel_telemetry)

option(SENTINEL_BUILD_TESTS "Build unit tests" ON)
if(SENTINEL_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_executable(sentinel_tests
        tests/cpu_math_test.cpp
        tests/process_telemetry_test.cpp
        tests/ring_buffer_test.cpp
        tests/anomaly_detector_test.cpp
        tests/event_store_test.cpp
        tests/event_attribution_test.cpp
        tests/persistence_test.cpp
    )
    target_link_libraries(sentinel_tests PRIVATE sentinel_telemetry GTest::gtest_main)
    include(GoogleTest)
    set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)
    gtest_discover_tests(sentinel_tests)
endif()
