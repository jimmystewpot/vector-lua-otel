# Build vendored single-header simdjson
set(simdjson_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/simdjson")
if(NOT EXISTS "${simdjson_dir}/simdjson.h" OR NOT EXISTS "${simdjson_dir}/simdjson.cpp")
  message(FATAL_ERROR
    "simdjson single-header files not found at: ${simdjson_dir}\n"
    "The simdjson dependency is vendored as a single-header library. Please ensure\n"
    "third_party/simdjson/simdjson.h and third_party/simdjson/simdjson.cpp are present.\n"
    "See: third_party/README.md"
  )
endif()

add_library(simdjson STATIC
  "${simdjson_dir}/simdjson.cpp"
)
target_include_directories(simdjson SYSTEM PUBLIC
  "${simdjson_dir}"
)
set_target_properties(simdjson PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)
add_library(simdjson::simdjson ALIAS simdjson)
