#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# VECTOR_OTEL_BUILD_DIR selects an alternate build directory (default: <repo>/build).
BUILD_DIR="${VECTOR_OTEL_BUILD_DIR:-${PROJECT_ROOT}/build}"
export LUA_CPATH="${BUILD_DIR}/?.so;;"

LUA_BIN="${LUA_BIN:-lua5.4}"

"${LUA_BIN}" "${SCRIPT_DIR}/test_vector_otel.lua"
"${LUA_BIN}" "${SCRIPT_DIR}/test_vector_native.lua"

# The configured payload limit is baked into the build; read it back from the generated header.
CONFIG_HEADER="${BUILD_DIR}/generated/vector_otel/core/config.hpp"
if [[ -f "${CONFIG_HEADER}" ]]; then
  LIMIT="$(sed -n 's/.*kMaxPayloadBytes = \([0-9][0-9]*\);.*/\1/p' "${CONFIG_HEADER}")"
  "${LUA_BIN}" "${SCRIPT_DIR}/test_payload_limit.lua" "${LIMIT}"
fi
