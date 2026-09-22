cmake_minimum_required(VERSION 3.20)
project(sentinel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

add_library(sentinel_telemetry STATIC
    src/telemetry/system_collector.cpp
)
target_include_directories(sentinel_telemetry PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

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
    add_executable(sentinel_tests tests/cpu_math_test.cpp)
    target_link_libraries(sentinel_tests PRIVATE sentinel_telemetry GTest::gtest_main)
    include(GoogleTest)
    set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)
    gtest_discover_tests(sentinel_tests)
endif()
