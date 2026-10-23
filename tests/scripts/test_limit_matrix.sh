#!/usr/bin/env bash
# Regression harness: validates that the build matrix script properly rejects
# explicit empty payload limits. Uses stubbed cmake/lua to avoid compilation.
# Invokes the ACTUAL production script (scripts/test_limit_builds.sh) with a fake PATH.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
HARNESS_SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/votel-matrix-harness.XXXXXX")"

cleanup() {
  rm -rf "${HARNESS_SCRATCH}"
}
trap cleanup EXIT

echo "=== Limit Matrix Script Harness ==="

# Create stub binaries in a temporary directory
STUB_BIN="${HARNESS_SCRATCH}/stubs"
mkdir -p "${STUB_BIN}"

# Helper function to create a cmake stub with specified behavior
# $1 = strict|broken|wrongmsg
# strict: rejects empty with proper diagnostic
# broken: accepts empty silently (regression)
# wrongmsg: rejects empty but without the expected diagnostic
make_cmake_stub() {
  local mode="$1"
  local outfile="$2"
  
  cat > "${outfile}" << 'STUB_CMAKE_BASE'
#!/usr/bin/env bash
binary_dir=""
limit=""
has_limit=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    -S) shift ;;
    -B) shift; binary_dir="$1" ;;
    -DVECTOR_OTEL_MAX_PAYLOAD_BYTES=*)
      has_limit=1
      limit="${1#-DVECTOR_OTEL_MAX_PAYLOAD_BYTES=}"
      ;;
    *) ;;
  esac
  shift
done

mkdir -p "${binary_dir}/generated/vector_otel/core"

# Create stub test binary
cat > "${binary_dir}/vector_otel_tests" << 'TEST_BIN_EOF'
#!/bin/bash
exit 0
TEST_BIN_EOF
chmod +x "${binary_dir}/vector_otel_tests"

STUB_CMAKE_BASE

  if [[ "$mode" == "strict" ]]; then
    cat >> "${outfile}" << 'STUB_CMAKE_STRICT'
# STRICT MODE: Reject empty with proper diagnostic
if [[ ${has_limit} -eq 1 ]]; then
  if [[ -z "${limit}" ]]; then
    echo "CMake Error: VECTOR_OTEL_MAX_PAYLOAD_BYTES cannot be empty" >&2
    exit 1
  fi
  
  if ! [[ "${limit}" =~ ^[1-9][0-9]*$ ]] || [[ "${limit}" -gt 536870912 ]]; then
    echo "CMake Error: VECTOR_OTEL_MAX_PAYLOAD_BYTES must be a positive integer <= 536870912, got '${limit}'" >&2
    exit 1
  fi
fi

echo "constexpr int64_t kMaxPayloadBytes = ${limit:-20971520};" > \
  "${binary_dir}/generated/vector_otel/core/config.hpp"
exit 0
STUB_CMAKE_STRICT

  elif [[ "$mode" == "broken" ]]; then
    cat >> "${outfile}" << 'STUB_CMAKE_BROKEN'
# BROKEN MODE: Accept empty (regression)
if [[ ${has_limit} -eq 1 ]]; then
  # Missing the empty check - uses default if empty
  if [[ -z "${limit}" ]]; then
    limit="20971520"
  fi
  
  if ! [[ "${limit}" =~ ^[1-9][0-9]*$ ]] || [[ "${limit}" -gt 536870912 ]]; then
    echo "CMake Error: invalid" >&2
    exit 1
  fi
fi

echo "constexpr int64_t kMaxPayloadBytes = ${limit:-20971520};" > \
  "${binary_dir}/generated/vector_otel/core/config.hpp"
exit 0
STUB_CMAKE_BROKEN

  elif [[ "$mode" == "wrongmsg" ]]; then
    cat >> "${outfile}" << 'STUB_CMAKE_WRONGMSG'
# WRONGMSG MODE: Reject empty but without expected diagnostic
if [[ ${has_limit} -eq 1 ]]; then
  if [[ -z "${limit}" ]]; then
    echo "CMake Error: configuration failed" >&2
    exit 1
  fi
  
  if ! [[ "${limit}" =~ ^[1-9][0-9]*$ ]] || [[ "${limit}" -gt 536870912 ]]; then
    echo "CMake Error: VECTOR_OTEL_MAX_PAYLOAD_BYTES invalid" >&2
    exit 1
  fi
fi

echo "constexpr int64_t kMaxPayloadBytes = ${limit:-20971520};" > \
  "${binary_dir}/generated/vector_otel/core/config.hpp"
exit 0
STUB_CMAKE_WRONGMSG
  fi

  chmod +x "${outfile}"
}

# Create stub lua5.4: just exit cleanly
cat > "${STUB_BIN}/lua5.4" << 'STUB_LUA'
#!/bin/bash
exit 0
STUB_LUA
chmod +x "${STUB_BIN}/lua5.4"

# Mode 1: STRICT (correct behavior)
echo ""
echo "=== Mode 1: STRICT (real script, proper cmake) ==="
MODE1_DIR="${HARNESS_SCRATCH}/mode1"
mkdir -p "${MODE1_DIR}"

make_cmake_stub "strict" "${STUB_BIN}/cmake"

if PATH="${STUB_BIN}:${PATH}" \
   KEEP_BUILD_DIRS=0 \
   bash "${ROOT}/scripts/test_limit_builds.sh" --self-test >/dev/null 2>&1; then
  echo "PASS: Matrix self-test passes"
else
  echo "FAIL: Matrix self-test failed"
  exit 1
fi

if PATH="${STUB_BIN}:${PATH}" \
   KEEP_BUILD_DIRS=0 \
   bash "${ROOT}/scripts/test_limit_builds.sh" >"${MODE1_DIR}/output.log" 2>&1; then
  echo "PASS: Real script completed successfully with strict cmake"
else
  echo "FAIL: Real script failed with strict cmake"
  tail -30 "${MODE1_DIR}/output.log"
  exit 1
fi

# Mode 2: DELIBERATE REGRESSION (broken cmake accepts empty)
echo ""
echo "=== Mode 2: DELIBERATE REGRESSION (fake cmake accepts empty) ==="
MODE2_DIR="${HARNESS_SCRATCH}/mode2"
mkdir -p "${MODE2_DIR}"

make_cmake_stub "broken" "${STUB_BIN}/cmake"

if PATH="${STUB_BIN}:${PATH}" \
   KEEP_BUILD_DIRS=0 \
   bash "${ROOT}/scripts/test_limit_builds.sh" >"${MODE2_DIR}/output.log" 2>&1; then
  echo "FAIL: Real script succeeded with broken cmake (regression NOT caught)"
  exit 1
else
  if grep -q "FAIL" "${MODE2_DIR}/output.log"; then
    echo "PASS: Real script correctly detected regression (broken cmake accepted empty)"
  else
    echo "FAIL: Real script failed but without detecting the regression"
    tail -10 "${MODE2_DIR}/output.log"
    exit 1
  fi
fi

# Mode 3: WRONG DIAGNOSTIC (cmake fails without expected message)
echo ""
echo "=== Mode 3: WRONG DIAGNOSTIC (cmake fails without proper message) ==="
MODE3_DIR="${HARNESS_SCRATCH}/mode3"
mkdir -p "${MODE3_DIR}"

make_cmake_stub "wrongmsg" "${STUB_BIN}/cmake"

if PATH="${STUB_BIN}:${PATH}" \
   KEEP_BUILD_DIRS=0 \
   bash "${ROOT}/scripts/test_limit_builds.sh" >"${MODE3_DIR}/output.log" 2>&1; then
  echo "FAIL: Real script succeeded with wrong-msg cmake"
  exit 1
else
  if grep -q "FAIL" "${MODE3_DIR}/output.log"; then
    echo "PASS: Real script rejected wrong-message cmake failure"
  else
    echo "FAIL: Real script failed but without detecting the diagnostic issue"
    tail -10 "${MODE3_DIR}/output.log"
    exit 1
  fi
fi

echo ""
echo "=========================================="
echo "HARNESS COMPLETE: All three modes verified"
echo "=========================================="
exit 0
