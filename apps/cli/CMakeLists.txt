add_executable(sentinel main.cpp)
target_link_libraries(sentinel PRIVATE sentinel_engine sentinel_client)
set_target_properties(sentinel PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
