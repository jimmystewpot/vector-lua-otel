# Build-time extraction limits.
#
# VECTOR_OTEL_MAX_PAYLOAD_BYTES (CACHE STRING) is the maximum uncompressed JSON payload, in bytes,
# accepted by BOTH modules. It is validated here and emitted into one generated header that core,
# both modules and the tests share. Override example:
#   cmake -B build -DVECTOR_OTEL_MAX_PAYLOAD_BYTES=33554432   # 32 MiB
# Parser/input memory is roughly (20/3) x this value per prepared workspace.
#
# VECTOR_OTEL_MAX_BATCH_RECORDS (CACHE STRING) is the maximum incoming records per call/batch
# (log records / spans / metric data points) accepted by BOTH modules. Override example:
#   cmake -B build -DVECTOR_OTEL_MAX_BATCH_RECORDS=4096

set(VECTOR_OTEL_DEFAULT_MAX_PAYLOAD_BYTES 20971520)
set(VECTOR_OTEL_ABSOLUTE_MAX_PAYLOAD_BYTES 536870912)

set(VECTOR_OTEL_DEFAULT_MAX_BATCH_RECORDS 2048)
set(VECTOR_OTEL_ABSOLUTE_MAX_BATCH_RECORDS 1048576)

# Validates `value` as a canonical decimal byte count in 1..536870912 and stores it in `out_var`.
# The text is checked (digits only, no sign/leading zero/whitespace, at most 9 digits) BEFORE any
# numeric comparison so an arbitrarily long or malformed value is never evaluated as a number.
# Invalid values are fatal errors: nothing is clamped or silently replaced by the default.
function(vector_otel_validate_payload_limit value out_var)
  set(var "VECTOR_OTEL_MAX_PAYLOAD_BYTES")
  set(range "1..${VECTOR_OTEL_ABSOLUTE_MAX_PAYLOAD_BYTES}")
  set(problem "")
  string(LENGTH "${value}" length)
  if(NOT "${value}" MATCHES "^[1-9][0-9]*$")
    set(problem "must be a canonical decimal integer (digits only, no sign, no leading zero)")
  elseif(length GREATER 9)
    set(problem "is longer than the largest legal value")
  elseif("${value}" GREATER ${VECTOR_OTEL_ABSOLUTE_MAX_PAYLOAD_BYTES})
    set(problem "is above the legal maximum")
  endif()
  if(problem)
    message(FATAL_ERROR
      "${var}='${value}' is invalid: ${problem}. "
      "The value is a size in bytes and must be in the range ${range} bytes "
      "(default ${VECTOR_OTEL_DEFAULT_MAX_PAYLOAD_BYTES}).")
  endif()
  set(${out_var} "${value}" PARENT_SCOPE)
endfunction()

# Validates `value` as a canonical decimal record count in 1..1048576 and stores it in `out_var`.
# Invalid values are fatal errors: nothing is clamped or silently replaced by the default.
function(vector_otel_validate_record_limit value out_var)
  set(var "VECTOR_OTEL_MAX_BATCH_RECORDS")
  set(range "1..${VECTOR_OTEL_ABSOLUTE_MAX_BATCH_RECORDS}")
  set(problem "")
  string(LENGTH "${value}" length)
  if(NOT "${value}" MATCHES "^[1-9][0-9]*$")
    set(problem "must be a canonical decimal integer (digits only, no sign, no leading zero)")
  elseif(length GREATER 7)
    set(problem "is longer than the largest legal value")
  elseif("${value}" GREATER ${VECTOR_OTEL_ABSOLUTE_MAX_BATCH_RECORDS})
    set(problem "is above the legal maximum")
  endif()
  if(problem)
    message(FATAL_ERROR
      "${var}='${value}' is invalid: ${problem}. "
      "The value is a record count and must be in the range ${range} records "
      "(default ${VECTOR_OTEL_DEFAULT_MAX_BATCH_RECORDS}).")
  endif()
  set(${out_var} "${value}" PARENT_SCOPE)
endfunction()

# Declares the cache variables, validates them and generates
# ${CMAKE_BINARY_DIR}/generated/vector_otel/core/config.hpp.
function(vector_otel_configure_extraction_limits)
  set(VECTOR_OTEL_MAX_PAYLOAD_BYTES ${VECTOR_OTEL_DEFAULT_MAX_PAYLOAD_BYTES} CACHE STRING
      "Maximum uncompressed JSON payload in bytes for both modules (1..536870912)")
  set(VECTOR_OTEL_MAX_BATCH_RECORDS ${VECTOR_OTEL_DEFAULT_MAX_BATCH_RECORDS} CACHE STRING
      "Maximum incoming records per call/batch for both modules (1..1048576)")
  # Fatal on any invalid value, so past this point the cache value is the validated canonical
  # decimal and configure_file() substitutes it unchanged.
  vector_otel_validate_payload_limit("${VECTOR_OTEL_MAX_PAYLOAD_BYTES}" validated_payload_unused)
  vector_otel_validate_record_limit("${VECTOR_OTEL_MAX_BATCH_RECORDS}" validated_record_unused)
  configure_file(
    "${PROJECT_SOURCE_DIR}/include/vector_otel/core/config.hpp.in"
    "${CMAKE_BINARY_DIR}/generated/vector_otel/core/config.hpp" @ONLY)
endfunction()
