cmake_minimum_required(VERSION 3.20)
project(vector_lua_otel VERSION 0.3.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)


option(ENABLE_NATIVE_TUNING "Enable -march=native" OFF)
option(ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)
option(ENABLE_SANITIZERS "Enable ASAN and UBSAN instrumentation" OFF)
option(BUILD_EXTRACTION_BENCHMARKS "Build the opt-in extraction latency benchmark (vector_otel_latency_bench)" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer instrumentation (exclusive with ENABLE_SANITIZERS)" OFF)
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

if(ENABLE_TSAN)
  if(ENABLE_SANITIZERS)
    message(FATAL_ERROR "ENABLE_TSAN cannot be combined with ENABLE_SANITIZERS")
  endif()
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread -fno-omit-frame-pointer -g)
  endif()
endif()

find_package(Lua 5.4 REQUIRED)

# All dependencies are vendored under third_party/ and built with
# add_subdirectory. This setting guarantees FetchContent can never reach the
# network. An un-vendored FetchContent dependency is skipped (CMake < 3.30) or
# rejected (CMP0170 NEW) and shows up as a missing target at link time.
set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "" FORCE)

include(cmake/extraction_limits.cmake)
vector_otel_configure_extraction_limits()

include(cmake/simdjson.cmake)
include(cmake/googletest.cmake)

add_library(vector_otel_core STATIC
  src/core/parser.cpp
  src/core/fast_json_writer.cpp
  src/core/timestamp.cpp
  src/core/otlp_common.cpp
  src/core/table_reader.cpp
  src/core/module_state.cpp
  src/core/protected_lua.cpp
  src/core/staged_row.cpp
  src/core/admission.cpp
  src/core/process_runner.cpp
  src/core/module_registration.cpp
  src/extractors/logs_extractor.cpp
  src/extractors/traces_extractor.cpp
  src/extractors/metrics_extractor.cpp
  src/native_extractors/native_logs_extractor.cpp
  src/native_extractors/native_traces_extractor.cpp
  src/native_extractors/native_metrics_extractor.cpp
)
set_target_properties(vector_otel_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(vector_otel_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_BINARY_DIR}/generated
  ${LUA_INCLUDE_DIR}
)
target_link_libraries(vector_otel_core PUBLIC simdjson)
if(NOT DEFINED VECTOR_NATIVE_MODULE_NAME)
  set(VECTOR_NATIVE_MODULE_NAME "vector_native")
endif()
target_compile_definitions(vector_otel_core PUBLIC
  VECTOR_OTEL_EMIT_BATCH_SIZE=${VECTOR_OTEL_EMIT_BATCH_SIZE}
  VECTOR_NATIVE_MODULE_NAME="${VECTOR_NATIVE_MODULE_NAME}"
)

# Production shared modules: MUST NOT link liblua
add_library(vector_otel MODULE src/module.cpp)
set_target_properties(vector_otel PROPERTIES PREFIX "" SUFFIX ".so")
target_link_libraries(vector_otel PRIVATE vector_otel_core)

add_library(vector_native MODULE src/native_module.cpp)
set_target_properties(vector_native PROPERTIES PREFIX "" SUFFIX ".so")
target_link_libraries(vector_native PRIVATE vector_otel_core)

if(APPLE)
  set_target_properties(vector_otel vector_native PROPERTIES LINK_FLAGS "-undefined dynamic_lookup")
endif()

if(NOT DEFINED LIBDIR)
  set(LIBDIR lib)
endif()
install(TARGETS vector_otel vector_native LIBRARY DESTINATION "${LIBDIR}")

# Unit test runner: MUST link ${LUA_LIBRARIES} so standalone tests can call luaL_newstate()
enable_testing()
add_executable(vector_otel_tests
  src/module.cpp
  src/native_module.cpp
  tests/cpp/test_main.cpp
  tests/cpp/test_fast_json_writer.cpp
  tests/cpp/test_timestamp.cpp
  tests/cpp/test_parser.cpp
  tests/cpp/test_otlp_common.cpp
  tests/cpp/test_logs_extractor.cpp
  tests/cpp/test_traces_extractor.cpp
  tests/cpp/test_metrics_extractor.cpp
  tests/cpp/test_module_exports.cpp
  tests/cpp/test_table_mode.cpp
  tests/cpp/test_scratch.cpp
  tests/cpp/test_native_extractors.cpp
  tests/cpp/test_extraction_limits.cpp
  tests/support/legacy_reference.cpp
)
target_include_directories(vector_otel_tests PRIVATE tests/support)
target_link_libraries(vector_otel_tests PRIVATE
  vector_otel_core
  ${LUA_LIBRARIES}
  GTest::gtest
)
add_test(NAME vector_otel_tests COMMAND vector_otel_tests)
target_compile_definitions(vector_otel_tests PRIVATE
  VECTOR_OTEL_TEST_EXPECTED_MAX_PAYLOAD_BYTES=${VECTOR_OTEL_MAX_PAYLOAD_BYTES}
  VECTOR_OTEL_UNCHECKED_READ_KEYS  # tests probe the readers with arbitrary keys (see read_keys.hpp)
)

# Configure-only tests for the VECTOR_OTEL_MAX_PAYLOAD_BYTES contract. The end-to-end cases
# re-run configure with the parent build's toolchain/Lua settings ('|' separated pass-through).
set(_pt "")
foreach(_v CMAKE_CXX_COMPILER CMAKE_C_COMPILER CMAKE_CXX_FLAGS CMAKE_EXE_LINKER_FLAGS
           CMAKE_SHARED_LINKER_FLAGS CMAKE_MODULE_LINKER_FLAGS LUA_INCLUDE_DIR LUA_LIBRARY)
  if(DEFINED ${_v} AND NOT "${${_v}}" STREQUAL "")
    list(APPEND _pt "-D${_v}=${${_v}}")
  endif()
endforeach()
string(REPLACE ";" "|" _pt "${_pt}")
add_test(NAME payload_limits
  COMMAND ${CMAKE_COMMAND} -DPROJECT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
          -DTEST_BINARY_ROOT=${CMAKE_BINARY_DIR}/payload_limit_configure "-DPASS_THROUGH=${_pt}"
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_payload_limits.cmake)

if(NOT ENABLE_SANITIZERS AND NOT ENABLE_TSAN)
  # Fault-injection tests. This executable REPLACES the global operator new/delete (see
  # tests/support/fault_new.cpp) so it must never be combined with sanitizers' allocator
  # interception, so it is not built at all when ENABLE_SANITIZERS / ENABLE_TSAN is on.
  add_executable(vector_otel_fault_tests
    tests/cpp/test_main.cpp
    tests/cpp/test_fault_harness.cpp
    tests/cpp/test_lua_fault_harness.cpp
    tests/cpp/test_contract_fixtures.cpp
    tests/cpp/test_parser_workspace.cpp
    tests/cpp/test_operation_context.cpp
    tests/cpp/test_admission.cpp
    tests/cpp/test_staged_row.cpp
    tests/cpp/test_table_staging.cpp
    tests/cpp/test_failure_safety.cpp
    src/module.cpp
    src/native_module.cpp
    tests/support/fault_new.cpp
    tests/support/fault_allocator.cpp
    tests/support/contract_fixtures.cpp
    tests/support/legacy_reference.cpp
  )
  target_include_directories(vector_otel_fault_tests PRIVATE tests/support)
  target_link_libraries(vector_otel_fault_tests PRIVATE
    vector_otel_core
    ${LUA_LIBRARIES}
    GTest::gtest
  )
  target_compile_definitions(vector_otel_fault_tests PRIVATE VECTOR_OTEL_UNCHECKED_READ_KEYS)
  add_test(NAME vector_otel_fault_tests COMMAND vector_otel_fault_tests)
endif()

# Worker-isolation tests: one Lua state per thread, no replaced global operator new, so this
# target is also the one built and run under ThreadSanitizer (-DENABLE_TSAN=ON).
find_package(Threads REQUIRED)
add_executable(vector_otel_worker_tests
  tests/cpp/test_main.cpp
  tests/cpp/test_worker_isolation.cpp
  src/module.cpp
  src/native_module.cpp
  tests/support/fault_allocator.cpp
)
target_include_directories(vector_otel_worker_tests PRIVATE tests/support)
target_link_libraries(vector_otel_worker_tests PRIVATE
  vector_otel_core
  ${LUA_LIBRARIES}
  GTest::gtest
  Threads::Threads
)
target_compile_definitions(vector_otel_worker_tests PRIVATE VECTOR_OTEL_UNCHECKED_READ_KEYS)
add_test(NAME vector_otel_worker_tests COMMAND vector_otel_worker_tests)

if(BUILD_EXTRACTION_BENCHMARKS)
  add_executable(vector_otel_latency_bench
    bench/extraction_latency.cpp
    src/module.cpp
    src/native_module.cpp
  )
  target_include_directories(vector_otel_latency_bench PRIVATE tests/support)
  target_link_libraries(vector_otel_latency_bench PRIVATE vector_otel_core ${LUA_LIBRARIES})
endif()
