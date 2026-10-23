# Child process for test_payload_limits.cmake: validates $ENV{VOTEL_CASE_VALUE} with the production
# function. The value travels in the environment because `cmake -D` trims trailing whitespace.
# Validation failures are message(FATAL_ERROR), so each case needs its own cmake process.
cmake_minimum_required(VERSION 3.20)
include("${PROJECT_SOURCE_DIR}/cmake/extraction_limits.cmake")
if("$ENV{VOTEL_VALIDATOR}" STREQUAL "record")
  vector_otel_validate_record_limit("$ENV{VOTEL_CASE_VALUE}" validated)
else()
  vector_otel_validate_payload_limit("$ENV{VOTEL_CASE_VALUE}" validated)
endif()
message("VALIDATED=${validated}")
