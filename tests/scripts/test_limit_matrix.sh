#!/usr/bin/env bash
# Exercise the real matrix control flow; no configuration or compilation occurs.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/votel-matrix-harness.XXXXXX")"
trap 'rm -rf "${SCRATCH}"' EXIT
mkdir -p "${SCRATCH}/bin" "${SCRATCH}/tmp"
cat > "${SCRATCH}/bin/cmake" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' 'CALL' "$@" 'END' >> "${MATRIX_ARGV}"
if [[ "${1:-}" == --build ]]; then exit 0; fi
binary_dir=""; limit=""; explicit=0
while (($#)); do
  case "$1" in
    -B) shift; binary_dir="$1" ;;
    -DVECTOR_OTEL_MAX_PAYLOAD_BYTES=*) explicit=1; limit="${1#*=}" ;;
  esac
  shift
done
[[ -n "${binary_dir}" ]]
if ((explicit)); then
  if [[ -z "${limit}" ]]; then
    case "${MATRIX_MODE}" in
      accepted) : ;;
      wrong) echo 'unrelated configure failure' >&2; exit 1 ;;
      strict) echo 'VECTOR_OTEL_MAX_PAYLOAD_BYTES cannot be empty' >&2; exit 1 ;;
    esac
  elif ! [[ "${limit}" =~ ^[1-9][0-9]*$ ]] || ((${#limit} > 9)) || ((limit > 536870912)); then
    echo 'VECTOR_OTEL_MAX_PAYLOAD_BYTES invalid' >&2; exit 1
  fi
fi
mkdir -p "${binary_dir}/generated/vector_otel/core"
printf 'constexpr auto kMaxPayloadBytes = %s;\n' "${limit:-20971520}" > "${binary_dir}/generated/vector_otel/core/config.hpp"
printf '#!/bin/sh\nexit 0\n' > "${binary_dir}/vector_otel_tests"
chmod +x "${binary_dir}/vector_otel_tests"
STUB
printf '#!/bin/sh\nexit 0\n' > "${SCRATCH}/bin/lua5.4"
chmod +x "${SCRATCH}/bin/"*
for mode in strict accepted wrong; do
  log="${SCRATCH}/${mode}.log"
  argv="${SCRATCH}/${mode}.argv"
  status=0
  PATH="${SCRATCH}/bin:${PATH}" LUA_BIN="${SCRATCH}/bin/lua5.4" \
    MATRIX_MODE="${mode}" MATRIX_ARGV="${argv}" TMPDIR="${SCRATCH}/tmp" KEEP_BUILD_DIRS=0 \
    bash "${ROOT}/scripts/test_limit_builds.sh" > "${log}" 2>&1 || status=$?
  if [[ "${mode}" == strict ]]; then
    [[ ${status} == 0 ]] || { cat "${log}"; exit 1; }
    grep -q 'PASS  C=20971520 focused C++ tests' "${log}"
    # Verify exact argv, not a similar substring in script source.
    awk '/^CALL$/{in_call=1; is_default=0; option=0; next}
         /^END$/{if(in_call && is_default && !option) found=1; in_call=0; next}
         /run-20971520$/{is_default=1}
         /^-DVECTOR_OTEL_MAX_PAYLOAD_BYTES=/{option=1}
         END{exit !found}' "${argv}"
    grep -qx -- '-DVECTOR_OTEL_MAX_PAYLOAD_BYTES=' "${argv}"
  else
    [[ ${status} != 0 ]] || { echo "${mode}: invalid configure was accepted"; exit 1; }
    if [[ "${mode}" == accepted ]]; then
      grep -q "FAIL  C='' was accepted" "${log}"
    else
      grep -q "FAIL  C='' failed without a VECTOR_OTEL_MAX_PAYLOAD_BYTES diagnostic" "${log}"
    fi
  fi
  echo "PASS matrix behavior: ${mode}"
done
