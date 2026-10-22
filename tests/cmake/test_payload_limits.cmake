# Tests the build-time VECTOR_OTEL_MAX_PAYLOAD_BYTES contract. Configure-only: nothing is built and
# no parser workspace is ever allocated.
#
# Usage: cmake -DPROJECT_SOURCE_DIR=<repo> -DTEST_BINARY_ROOT=<scratch dir>
#              [-DPASS_THROUGH="-DA=b|-DC=d"]  -P tests/cmake/test_payload_limits.cmake
# PASS_THROUGH ('|' separated) carries the parent build's compiler / Lua settings to the real
# end-to-end configure cases.
cmake_minimum_required(VERSION 3.20)

if(NOT PROJECT_SOURCE_DIR OR NOT TEST_BINARY_ROOT)
  message(FATAL_ERROR "PROJECT_SOURCE_DIR and TEST_BINARY_ROOT are required")
endif()
string(REPLACE "|" ";" pass_through "${PASS_THROUGH}")
set_property(GLOBAL PROPERTY PL_FAILURES 0)
set(var "VECTOR_OTEL_MAX_PAYLOAD_BYTES")
set(range "1..536870912")

# A global property is used because the helpers below run in nested function scopes, where a
# PARENT_SCOPE variable would only reach the calling helper, never the top level.
function(record_failure text)
  message(STATUS "FAIL: ${text}")
  get_property(n GLOBAL PROPERTY PL_FAILURES)
  math(EXPR n "${n} + 1")
  set_property(GLOBAL PROPERTY PL_FAILURES ${n})
endfunction()

# ---- unit cases through the production validation function -------------------------------------
function(expect_valid value)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "VOTEL_CASE_VALUE=${value}"
            "${CMAKE_COMMAND}" "-DPROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
            -P "${PROJECT_SOURCE_DIR}/tests/cmake/validate_one.cmake"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  # cmake prints message() to stderr in script mode. Match the whole line literally (no regex, so
  # a value can never act as a pattern, and "1" cannot be satisfied by "10").
  string(FIND "${err}${out}" "VALIDATED=${value}\n" p_line)
  if(NOT rc EQUAL 0 OR p_line EQUAL -1)
    record_failure("expected '${value}' accepted; rc=${rc} output=${out}${err}")
  else()
    message(STATUS "ok: accepted '${value}'")
  endif()
endfunction()

# `reason` is a single word that must appear in the diagnostic (CMake wraps long message lines, so
# multi-word fragments could be split across lines).
function(expect_invalid value reason)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "VOTEL_CASE_VALUE=${value}"
            "${CMAKE_COMMAND}" "-DPROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
            -P "${PROJECT_SOURCE_DIR}/tests/cmake/validate_one.cmake"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(text "${out}${err}")
  set(ok TRUE)
  if(rc EQUAL 0)
    set(ok FALSE)
  endif()
  # The diagnostic must name the variable, echo the supplied spelling, the unit and the range.
  string(FIND "${text}" "${var}" p_var)
  string(FIND "${text}" "'${value}'" p_val)
  string(FIND "${text}" "bytes" p_unit)
  string(FIND "${text}" "${range}" p_range)
  string(FIND "${text}" "${reason}" p_reason)
  foreach(p IN ITEMS p_var p_val p_unit p_range p_reason)
    if(${p} EQUAL -1)
      set(ok FALSE)
      set(missing "${missing} ${p}")
    endif()
  endforeach()
  if(NOT ok)
    record_failure("expected '${value}' rejected with full diagnostic; rc=${rc} missing:${missing}")
  else()
    message(STATUS "ok: rejected '${value}'")
  endif()
endfunction()

foreach(v IN ITEMS 1 8388608 20971520 33554432 536870912)
  expect_valid("${v}")
endforeach()

string(REPEAT "9" 100 hundred_nines)
foreach(v IN ITEMS "" "0" "-1" "+5" "1.5" "20M" "2e7" "0123" " 5" "5 " "0x10")
  expect_invalid("${v}" "canonical")
endforeach()
# Length is checked before the number is evaluated, so these are rejected for their length;
# a 9-digit value above the maximum is rejected by the range check.
expect_invalid("9999999999" "longer")
expect_invalid("${hundred_nines}" "longer")
expect_invalid("536870913" "above")

# ---- end-to-end: the real CMakeLists must validate and generate the header ---------------------
function(configure_case name value out_rc out_text out_dir)
  set(dir "${TEST_BINARY_ROOT}/${name}")
  file(REMOVE_RECURSE "${dir}")
  set(args "")
  if(NOT value STREQUAL "<omit>")
    list(APPEND args "-D${var}=${value}")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${PROJECT_SOURCE_DIR}" -B "${dir}" ${pass_through} ${args}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(${out_rc} ${rc} PARENT_SCOPE)
  set(${out_text} "${out}${err}" PARENT_SCOPE)
  set(${out_dir} "${dir}" PARENT_SCOPE)
endfunction()

function(expect_configure_header name value expected)
  configure_case(${name} "${value}" rc text dir)
  set(header "${dir}/generated/vector_otel/core/config.hpp")
  if(NOT rc EQUAL 0 OR NOT EXISTS "${header}")
    record_failure("configure '${value}' should succeed and generate ${header}; rc=${rc}")
    return()
  endif()
  file(READ "${header}" content)
  if(NOT content MATCHES "kMaxPayloadBytes = ${expected};")
    record_failure("generated header for '${value}' lacks 'kMaxPayloadBytes = ${expected};'")
  elseif(NOT content MATCHES "kMaxIncomingRecords = 2048;")
    record_failure("generated header lacks kMaxIncomingRecords = 2048")
  else()
    message(STATUS "ok: configure '${value}' -> kMaxPayloadBytes = ${expected}")
  endif()
endfunction()

function(expect_configure_rejected name value)
  configure_case(${name} "${value}" rc text dir)
  string(FIND "${text}" "${var}" p_var)
  string(FIND "${text}" "'${value}'" p_val)
  if(rc EQUAL 0 OR p_var EQUAL -1 OR p_val EQUAL -1)
    record_failure("configure '${value}' should fail naming ${var} and the value; rc=${rc}")
  else()
    message(STATUS "ok: configure rejected '${value}'")
  endif()
endfunction()

expect_configure_header(default "<omit>" 20971520)
expect_configure_header(minimum 1 1)
expect_configure_header(maximum 536870912 536870912)
expect_configure_header(custom 33554432 33554432)
expect_configure_rejected(empty "")
expect_configure_rejected(zero 0)
expect_configure_rejected(too_large 536870913)

file(REMOVE_RECURSE "${TEST_BINARY_ROOT}")
get_property(failures GLOBAL PROPERTY PL_FAILURES)
if(failures GREATER 0)
  message(FATAL_ERROR "${failures} payload-limit case(s) failed")
endif()
message(STATUS "all payload-limit cases passed")
