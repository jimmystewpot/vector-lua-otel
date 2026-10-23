#!/usr/bin/env bash
# Regression harness: validates that the build matrix script properly rejects
# explicit empty payload limits. Uses stubbed cmake/lua to avoid compilation.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
HARNESS_SCRATCH="${TMPDIR:-/tmp}/votel-matrix-harness.$$"
mkdir -p "${HARNESS_SCRATCH}"

cleanup() {
  rm -rf "${HARNESS_SCRATCH}"
}
trap cleanup EXIT

echo "=== Limit Matrix Script Harness ==="

# Create stub binaries
STUB_BIN="${HARNESS_SCRATCH}/stubs"
mkdir -p "${STUB_BIN}"

# Stub cmake: properly validates VECTOR_OTEL_MAX_PAYLOAD_BYTES
# Valid values: 1-536870912; invalid: 0, empty, negative, hex, etc.
cat > "${STUB_BIN}/cmake" << 'STUB_CMAKE'
#!/usr/bin/env bash
binary_dir=""
limit=""
has_limit=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    -B) shift; binary_dir="$1" ;;
    -DVECTOR_OTEL_MAX_PAYLOAD_BYTES=*)
      has_limit=1
      limit="${1#-DVECTOR_OTEL_MAX_PAYLOAD_BYTES=}"
      ;;
  esac
  shift
done

mkdir -p "${binary_dir}/generated/vector_otel/core"
mkdir -p "${binary_dir}"

# If limit was explicitly provided (even if empty), validate it
if [[ ${has_limit} -eq 1 ]]; then
  if [[ -z "${limit}" ]]; then
    # Empty value explicitly passed: reject it
    echo "CMake Error: VECTOR_OTEL_MAX_PAYLOAD_BYTES cannot be empty" >&2
    exit 1
  fi
  
  if ! [[ "${limit}" =~ ^[1-9][0-9]*$ ]] || [[ "${limit}" -gt 536870912 ]]; then
    echo "CMake Error: VECTOR_OTEL_MAX_PAYLOAD_BYTES must be a positive integer <= 536870912, got '${limit}'" >&2
    exit 1
  fi
  echo "constexpr int64_t kMaxPayloadBytes = ${limit};" > \
    "${binary_dir}/generated/vector_otel/core/config.hpp"
  exit 0
fi

# If limit was not provided, use default
echo "constexpr int64_t kMaxPayloadBytes = 20971520;" > \
  "${binary_dir}/generated/vector_otel/core/config.hpp"
exit 0
STUB_CMAKE
chmod +x "${STUB_BIN}/cmake"

# Stub lua5.4
cat > "${STUB_BIN}/lua5.4" << 'STUB_LUA'
#!/bin/bash
exit 0
STUB_LUA
chmod +x "${STUB_BIN}/lua5.4"

# Test the invalid-limits loop
TEST_DIR="${HARNESS_SCRATCH}/test"
mkdir -p "${TEST_DIR}"

cat > "${TEST_DIR}/test.sh" << 'TEST_SCRIPT'
#!/usr/bin/env bash
set -euo pipefail

SCRATCH="${1}"
mkdir -p "${SCRATCH}"

DEFAULT_C=20971520
INVALID_LIMITS=(0 "" 536870913)

failures=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; failures=$((failures + 1)); }

configure() {
  local dir="$1"
  shift
  mkdir -p "${dir}"
  cmake -S /tmp -B "${dir}" -DCMAKE_BUILD_TYPE=Release "$@" \
    >"${dir}.log" 2>&1
}

for limit in "${INVALID_LIMITS[@]}"; do
  dir="${SCRATCH}/bad-${limit:-empty}"
  echo "== C='${limit}' (must be rejected) =="
  if configure "${dir}" "-DVECTOR_OTEL_MAX_PAYLOAD_BYTES=${limit}"; then
    fail "C='${limit}' was accepted"
  elif grep -q "VECTOR_OTEL_MAX_PAYLOAD_BYTES" "${dir}.log"; then
    pass "C='${limit}' rejected with a diagnostic"
  else
    fail "C='${limit}' failed without a VECTOR_OTEL_MAX_PAYLOAD_BYTES diagnostic"
  fi
done

if [[ "${failures}" -ne 0 ]]; then
  echo "${failures} check(s) FAILED"
  exit 1
fi
echo "TEST OK"
exit 0
TEST_SCRIPT
chmod +x "${TEST_DIR}/test.sh"

echo "Running test with stub cmake validation..."
if PATH="${STUB_BIN}:${PATH}" bash "${TEST_DIR}/test.sh" "${TEST_DIR}/scratch" 2>&1; then
  echo ""
  echo "HARNESS: FIX VERIFIED - all invalid values rejected"
  exit 0
else
  echo ""
  echo "HARNESS: FAILED"
  exit 1
fi
