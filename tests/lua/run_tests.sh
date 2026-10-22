#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

BUILD_DIR="${BUILD_DIR:-${PROJECT_ROOT}/build}"
export LUA_CPATH="${BUILD_DIR}/?.so;;"

lua5.4 "${SCRIPT_DIR}/test_vector_otel.lua"
lua5.4 "${SCRIPT_DIR}/test_vector_native.lua"
