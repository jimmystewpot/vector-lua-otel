# Build vendored simdjson instead of fetching from network
set(simdjson_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/simdjson")
if(NOT EXISTS "${simdjson_source_dir}/CMakeLists.txt")
  message(FATAL_ERROR
    "simdjson CMakeLists.txt not found at: ${simdjson_source_dir}\n"
    "The simdjson dependency is vendored. Please ensure third_party/simdjson is present.\n"
    "See: third_party/README.md"
  )
endif()

set(SIMDJSON_DEVELOPER_MODE OFF CACHE BOOL "" FORCE)
add_subdirectory("${simdjson_source_dir}" "${CMAKE_BINARY_DIR}/third_party/simdjson")
