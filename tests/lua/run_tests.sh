#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

export LUA_CPATH="${PROJECT_ROOT}/build/?.so;;"

lua5.4 "${SCRIPT_DIR}/test_vector_otel.lua"
