#!/usr/bin/env bash
# Build-time payload-limit matrix (offline: nothing here fetches anything).
#
#   scripts/test_limit_builds.sh            run the full matrix
#   scripts/test_limit_builds.sh --self-test  check the script's own matrix/validation logic only
#
# Matrix (C = VECTOR_OTEL_MAX_PAYLOAD_BYTES, R = VECTOR_OTEL_MAX_BATCH_RECORDS):
#   built and executed : C = default (20 MiB), 8 MiB, 32 MiB
#                        -> focused C++ tests (admission, parser, extraction limits, module
#                           exports) + the Lua payload-limit test against BOTH real modules
#                        R = 4096
#                        -> full C++ tests (vector_otel_tests, vector_otel_fault_tests)
#                           + Lua record-limit and payload-limit tests against BOTH real modules
#   configure-only     : C=1 and C=536870912 (512 MiB); R=1 and R=1048576 (1 Mi records)
#   must fail to configure: invalid values for C and R
#
# Environment:
#   VECTOR_OTEL_CMAKE_ARGS   extra cmake arguments (toolchain / Lua location), word-split
#   LUA_BIN                  Lua 5.4 interpreter (default lua5.4)
#   JOBS                     parallel build jobs (default 4)
#   KEEP_BUILD_DIRS=1        keep the scratch build directories
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
LUA_BIN="${LUA_BIN:-lua5.4}"
JOBS="${JOBS:-4}"
# shellcheck disable=SC2206
EXTRA_ARGS=(${VECTOR_OTEL_CMAKE_ARGS:-})

DEFAULT_C=20971520
DEFAULT_R=2048
RUN_LIMITS=("" 8388608 33554432)           # "" = leave the option unset (the default)
CONFIGURE_ONLY_LIMITS=(1 536870912)
INVALID_LIMITS=(0 536870913 01 +5 -1 1e3 0x10 "" 5MiB)
RUN_RECORD_LIMITS=(4096)
CONFIGURE_ONLY_RECORD_LIMITS=(1 1048576)
INVALID_RECORD_LIMITS=(0 1048577 01 +5 -1 1e3 0x10 "" 5k)
FOCUSED_TESTS='*Admission*:*Parser*:*ExtractionLimits*:*ModuleExports*:*PayloadLimit*'

SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/votel-limits.XXXXXX")"
cleanup() {
  if [[ "${KEEP_BUILD_DIRS:-0}" != "1" ]]; then
    rm -rf "${SCRATCH}"
  else
    echo "kept build directories in ${SCRATCH}"
  fi
}
trap cleanup EXIT

failures=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; failures=$((failures + 1)); }

# configure <dir> [cmake -D args...]; returns cmake's status, output in <dir>.log
configure() {
  local dir="$1"
  shift
  mkdir -p "${dir}"
  cmake -S "${ROOT}" -B "${dir}" -DCMAKE_BUILD_TYPE=Release ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"} "$@" \
    >"${dir}.log" 2>&1
}

self_test() {
  local ok=1
  [[ "${#RUN_LIMITS[@]}" -eq 3 ]] || ok=0
  [[ "${RUN_LIMITS[0]}" == "" ]] || ok=0
  for c in "${RUN_LIMITS[@]}" "${CONFIGURE_ONLY_LIMITS[@]}"; do
    [[ -z "${c}" || "${c}" =~ ^[1-9][0-9]*$ ]] || ok=0
    [[ -z "${c}" ]] || [[ "${c}" -le 536870912 ]] || ok=0
  done
  for c in "${INVALID_LIMITS[@]}"; do
    if [[ "${c}" =~ ^[1-9][0-9]*$ && "${c}" -le 536870912 ]]; then ok=0; fi
  done
  # The 512 MiB limit must never be in the executed set.
  for c in "${RUN_LIMITS[@]}"; do
    [[ -z "${c}" || "${c}" =~ ^[1-9][0-9]*$ ]] || ok=0
    [[ -z "${c}" || "${c}" -le 33554432 ]] || ok=0
  done
  [[ "${RUN_RECORD_LIMITS[@]+"${#RUN_RECORD_LIMITS[@]}"}" -ge 1 ]] 2>/dev/null || ok=0
  for r in ${RUN_RECORD_LIMITS[@]+"${RUN_RECORD_LIMITS[@]}"} "${CONFIGURE_ONLY_RECORD_LIMITS[@]}"; do
    [[ -z "${r}" || "${r}" =~ ^[1-9][0-9]*$ ]] || ok=0
    [[ -z "${r}" ]] || [[ "${r}" -le 1048576 ]] || ok=0
  done
  for r in "${INVALID_RECORD_LIMITS[@]}"; do
    if [[ "${r}" =~ ^[1-9][0-9]*$ && "${r}" -le 1048576 ]]; then ok=0; fi
  done
  # The 1048576 limit must never be in the executed set.
  for r in ${RUN_RECORD_LIMITS[@]+"${RUN_RECORD_LIMITS[@]}"}; do
    [[ -n "${r}" && "${r}" =~ ^[1-9][0-9]*$ && "${r}" -le 65536 ]] || ok=0
  done
  local rec_run_count="${RUN_RECORD_LIMITS[@]+"${#RUN_RECORD_LIMITS[@]}"}"
  rec_run_count="${rec_run_count:-0}"
  if [[ "${ok}" == 1 ]]; then
    echo "self-test OK (${#RUN_LIMITS[@]} payload executed, ${rec_run_count} record executed," \
      "${#CONFIGURE_ONLY_LIMITS[@]} payload configure-only, ${#CONFIGURE_ONLY_RECORD_LIMITS[@]} record configure-only," \
      "$(( ${#INVALID_LIMITS[@]} + ${#INVALID_RECORD_LIMITS[@]} )) invalid)"
  else
    echo "self-test FAILED: matrix definition is inconsistent" >&2
    exit 1
  fi
}

if [[ "${1:-}" == "--self-test" ]]; then
  self_test
  exit 0
fi
self_test

for limit in "${RUN_LIMITS[@]}"; do
  effective="${limit:-${DEFAULT_C}}"
  dir="${SCRATCH}/run-${effective}"
  args=()
  [[ -n "${limit}" ]] && args+=("-DVECTOR_OTEL_MAX_PAYLOAD_BYTES=${limit}")
  echo "== C=${effective} (build + execute) =="
  if ! configure "${dir}" ${args[@]+"${args[@]}"}; then
    fail "C=${effective} configure (see ${dir}.log)"
    tail -5 "${dir}.log" || true
    continue
  fi
  if ! cmake --build "${dir}" -j "${JOBS}" --target vector_otel_tests vector_otel vector_native \
    >"${dir}.build.log" 2>&1; then
    fail "C=${effective} build"
    tail -20 "${dir}.build.log" || true
    continue
  fi
  if "${dir}/vector_otel_tests" --gtest_filter="${FOCUSED_TESTS}" >"${dir}.test.log" 2>&1; then
    pass "C=${effective} focused C++ tests"
  else
    fail "C=${effective} focused C++ tests"
    tail -20 "${dir}.test.log" || true
  fi
  if LUA_CPATH="${dir}/?.so;;" "${LUA_BIN}" "${ROOT}/tests/lua/test_payload_limit.lua" \
    "${effective}"; then
    pass "C=${effective} admission text + record behaviour in both real modules"
  else
    fail "C=${effective} Lua payload-limit test"
  fi
  RECORD_LIMIT="$(sed -n 's/.*kMaxIncomingRecords = \([0-9][0-9]*\);.*/\1/p' "${dir}/generated/vector_otel/core/config.hpp")"
  if LUA_CPATH="${dir}/?.so;;" "${LUA_BIN}" "${ROOT}/tests/lua/test_record_limit.lua" \
    "${RECORD_LIMIT}"; then
    pass "C=${effective} Lua record-limit test (${RECORD_LIMIT} records)"
  else
    fail "C=${effective} Lua record-limit test"
  fi
done

for limit in ${RUN_RECORD_LIMITS[@]+"${RUN_RECORD_LIMITS[@]}"}; do
  effective="${limit:-${DEFAULT_R}}"
  dir="${SCRATCH}/run-rec-${effective}"
  args=()
  [[ -n "${limit}" ]] && args+=("-DVECTOR_OTEL_MAX_BATCH_RECORDS=${limit}")
  echo "== R=${effective} (build + execute) =="
  if ! configure "${dir}" ${args[@]+"${args[@]}"}; then
    fail "R=${effective} configure (see ${dir}.log)"
    tail -5 "${dir}.log" || true
    continue
  fi
  if ! cmake --build "${dir}" -j "${JOBS}" \
    --target vector_otel_tests vector_otel_fault_tests vector_otel vector_native \
    >"${dir}.build.log" 2>&1; then
    fail "R=${effective} build"
    tail -20 "${dir}.build.log" || true
    continue
  fi
  if "${dir}/vector_otel_tests" >"${dir}.test.log" 2>&1; then
    pass "R=${effective} C++ unit tests"
  else
    fail "R=${effective} C++ unit tests"
    tail -20 "${dir}.test.log" || true
  fi
  if "${dir}/vector_otel_fault_tests" >"${dir}.fault_test.log" 2>&1; then
    pass "R=${effective} C++ fault tests"
  else
    fail "R=${effective} C++ fault tests"
    tail -20 "${dir}.fault_test.log" || true
  fi
  if LUA_CPATH="${dir}/?.so;;" "${LUA_BIN}" "${ROOT}/tests/lua/test_record_limit.lua" \
    "${effective}"; then
    pass "R=${effective} Lua record-limit test (${effective} records)"
  else
    fail "R=${effective} Lua record-limit test"
  fi
  PAYLOAD_LIMIT="$(sed -n 's/.*kMaxPayloadBytes = \([0-9][0-9]*\);.*/\1/p' "${dir}/generated/vector_otel/core/config.hpp")"
  if LUA_CPATH="${dir}/?.so;;" "${LUA_BIN}" "${ROOT}/tests/lua/test_payload_limit.lua" \
    "${PAYLOAD_LIMIT:-${DEFAULT_C}}"; then
    pass "R=${effective} admission text + record behaviour in both real modules"
  else
    fail "R=${effective} Lua payload-limit test"
  fi
done

for limit in "${CONFIGURE_ONLY_RECORD_LIMITS[@]}"; do
  dir="${SCRATCH}/cfg-rec-${limit}"
  echo "== R=${limit} (configure only) =="
  if configure "${dir}" "-DVECTOR_OTEL_MAX_BATCH_RECORDS=${limit}" &&
    grep -q "kMaxIncomingRecords = ${limit};" "${dir}/generated/vector_otel/core/config.hpp"; then
    pass "R=${limit} configures and is baked into the generated header"
  else
    fail "R=${limit} configure"
  fi
done

for limit in "${INVALID_RECORD_LIMITS[@]}"; do
  dir="${SCRATCH}/bad-rec-${limit:-empty}"
  echo "== R='${limit}' (must be rejected) =="
  if configure "${dir}" "-DVECTOR_OTEL_MAX_BATCH_RECORDS=${limit}"; then
    fail "R='${limit}' was accepted"
  elif grep -q "VECTOR_OTEL_MAX_BATCH_RECORDS" "${dir}.log"; then
    pass "R='${limit}' rejected with a diagnostic"
  else
    fail "R='${limit}' failed without a VECTOR_OTEL_MAX_BATCH_RECORDS diagnostic"
  fi
done

for limit in "${CONFIGURE_ONLY_LIMITS[@]}"; do
  dir="${SCRATCH}/cfg-${limit}"
  echo "== C=${limit} (configure only) =="
  if configure "${dir}" "-DVECTOR_OTEL_MAX_PAYLOAD_BYTES=${limit}" &&
    grep -q "kMaxPayloadBytes = ${limit};" "${dir}/generated/vector_otel/core/config.hpp"; then
    pass "C=${limit} configures and is baked into the generated header"
  else
    fail "C=${limit} configure"
  fi
done

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
  echo "${failures} check(s) FAILED" >&2
  exit 1
fi
echo "LIMIT MATRIX OK"
