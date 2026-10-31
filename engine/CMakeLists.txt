add_library(sentinel_engine STATIC
    src/monitoring/monitor.cpp
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
    target_link_libraries(sentinel_engine PUBLIC Psapi Shell32 Ole32 Advapi32)
endif()

# The reusable client has no SQLite/collector dependency.
add_library(sentinel_client STATIC src/monitoring/status_codec.cpp src/monitoring/pipe_transport.cpp)
target_include_directories(sentinel_client PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(sentinel_client PUBLIC nlohmann_json::nlohmann_json Advapi32)

if(MSVC)
    foreach(target sentinel_engine sentinel_client)
        target_compile_options(${target} PUBLIC /W4 /permissive-)
        target_compile_definitions(${target} PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    endforeach()
endif()

# Preserve the existing library target for downstream CMake users.
add_library(sentinel_telemetry ALIAS sentinel_engine)

if(SENTINEL_BUILD_TESTS)
    add_subdirectory(tests)
endif()
