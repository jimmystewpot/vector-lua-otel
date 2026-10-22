cmake_minimum_required(VERSION 3.20)
project(vector_lua_otel VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)


option(ENABLE_NATIVE_TUNING "Enable -march=native" OFF)
option(ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)
option(ENABLE_SANITIZERS "Enable ASAN and UBSAN instrumentation" OFF)
set(VECTOR_OTEL_EMIT_BATCH_SIZE 1 CACHE STRING "Compile-time emit batch size (default: 1)")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(ENABLE_NATIVE_TUNING)
  add_compile_options(-march=native)
endif()

if(ENABLE_COVERAGE)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
  endif()
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-fprofile-update=atomic)
  endif()
endif()

if(ENABLE_SANITIZERS)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer -g)
  endif()
endif()

find_package(Lua 5.4 REQUIRED)

# All dependencies are vendored under third_party/ and built with
# add_subdirectory. This setting guarantees FetchContent can never reach the
# network. An un-vendored FetchContent dependency is skipped (CMake < 3.30) or
# rejected (CMP0170 NEW) and shows up as a missing target at link time.
set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "" FORCE)

include(cmake/simdjson.cmake)
include(cmake/googletest.cmake)

add_library(vector_otel_core STATIC
  src/core/parser.cpp
  src/core/fast_json_writer.cpp
  src/core/timestamp.cpp
  src/core/lua_bridge.cpp
  src/core/otlp_common.cpp
  src/core/table_reader.cpp
  src/extractors/logs_extractor.cpp
  src/extractors/traces_extractor.cpp
  src/extractors/metrics_extractor.cpp
)
set_target_properties(vector_otel_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(vector_otel_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${LUA_INCLUDE_DIR}
)
target_link_libraries(vector_otel_core PUBLIC simdjson)
target_compile_definitions(vector_otel_core PUBLIC
  VECTOR_OTEL_EMIT_BATCH_SIZE=${VECTOR_OTEL_EMIT_BATCH_SIZE}
)

# Production shared module: MUST NOT link liblua
add_library(vector_otel MODULE src/module.cpp)
set_target_properties(vector_otel PROPERTIES PREFIX "")
if(APPLE)
  set_target_properties(vector_otel PROPERTIES LINK_FLAGS "-undefined dynamic_lookup")
endif()
target_link_libraries(vector_otel PRIVATE vector_otel_core)

if(NOT DEFINED LIBDIR)
  set(LIBDIR lib)
endif()
install(TARGETS vector_otel LIBRARY DESTINATION "${LIBDIR}")

# Unit test runner: MUST link ${LUA_LIBRARIES} so standalone tests can call luaL_newstate()
enable_testing()
add_executable(vector_otel_tests
  src/module.cpp
  tests/cpp/test_main.cpp
  tests/cpp/test_fast_json_writer.cpp
  tests/cpp/test_timestamp.cpp
  tests/cpp/test_parser.cpp
  tests/cpp/test_lua_bridge.cpp
  tests/cpp/test_otlp_common.cpp
  tests/cpp/test_logs_extractor.cpp
  tests/cpp/test_traces_extractor.cpp
  tests/cpp/test_metrics_extractor.cpp
  tests/cpp/test_module_exports.cpp
  tests/cpp/test_table_mode.cpp
  tests/cpp/test_scratch.cpp
)
target_link_libraries(vector_otel_tests PRIVATE
  vector_otel_core
  ${LUA_LIBRARIES}
  GTest::gtest
)
add_test(NAME vector_otel_tests COMMAND vector_otel_tests)
