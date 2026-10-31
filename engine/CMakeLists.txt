add_library(sentinel_engine STATIC
    src/paths.cpp
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
target_include_directories(sentinel_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(sentinel_engine PUBLIC sentinel_sqlite nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(sentinel_engine PUBLIC Psapi Shell32 Ole32)
endif()

if(MSVC)
    target_compile_options(sentinel_engine PUBLIC /W4 /permissive-)
    target_compile_definitions(sentinel_engine PUBLIC
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()


# Preserve the existing library target for downstream CMake users.
add_library(sentinel_telemetry ALIAS sentinel_engine)

if(SENTINEL_BUILD_TESTS)
    add_subdirectory(tests)
endif()
