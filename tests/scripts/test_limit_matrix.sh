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
# Only a configure call's -B argument identifies the default build directory.
# A later --build invocation must never satisfy or hide this assertion.
assert_default_configure_omits_limit() {
  awk '/^CALL$/{configure=0; default_dir=0; option=0; expect_dir=0; next}
       /^END$/{if(configure && default_dir) {seen++; if(option) bad=1}; next}
       expect_dir {default_dir=($0 ~ /\/run-20971520$/); expect_dir=0; next}
       /^-B$/{configure=1; expect_dir=1; next}
       /^-DVECTOR_OTEL_MAX_PAYLOAD_BYTES=/{option=1}
       END{exit !(seen == 1 && !bad)}' "$1"
}

# Pin the previous blind spot: a clean --build call cannot excuse an explicit
# limit in the configure call, or substitute for a missing configure call.
cat > "${SCRATCH}/explicit-default.argv" <<'ARGV'
CALL
-S
/source
-B
/build/run-20971520
-DVECTOR_OTEL_MAX_PAYLOAD_BYTES=20971520
END
CALL
--build
/build/run-20971520
END
ARGV
cat > "${SCRATCH}/build-only.argv" <<'ARGV'
CALL
--build
/build/run-20971520
END
ARGV
for fixture in explicit-default build-only; do
  if assert_default_configure_omits_limit "${SCRATCH}/${fixture}.argv"; then
    echo "FAIL default configure assertion accepted ${fixture}" >&2
    exit 1
  fi
done

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
    # Verify the real configure argv, never the subsequent build invocation.
    assert_default_configure_omits_limit "${argv}"
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
