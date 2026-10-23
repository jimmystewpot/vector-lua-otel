#!/usr/bin/env bash
# Automated test suite for scripts/package_release.sh and .gitattributes
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"
PACKAGE_SCRIPT="$REPO_ROOT/scripts/package_release.sh"

SKIP_TARBALL_BUILD=false
while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-tarball-build)
      SKIP_TARBALL_BUILD=true
      shift
      ;;
    *)
      echo "Usage: $0 [--skip-tarball-build]" >&2
      exit 2
      ;;
  esac
done

TEMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/test_package_release.XXXXXX")"
trap 'rm -rf "$TEMP_ROOT"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

assert_exit_code() {
  local expected="$1"
  shift
  local actual=0
  "$@" >/dev/null 2>&1 || actual=$?
  if [[ "$actual" -ne "$expected" ]]; then
    fail "Expected exit code $expected from '$*', got $actual"
  fi
}

echo "=== Running Release Packaging Test Suite ==="

# 1. Verify scripts/package_release.sh exists and is executable
if [[ ! -x "$PACKAGE_SCRIPT" ]]; then
  fail "Expected executable script at $PACKAGE_SCRIPT"
fi

# 2. CLI usage error checks
echo ">> Testing CLI usage validation..."
assert_exit_code 2 bash "$PACKAGE_SCRIPT"
assert_exit_code 2 bash "$PACKAGE_SCRIPT" v0.1.0 "$TEMP_ROOT/out" extra_arg
echo "   CLI usage validation passed."

# 3. Invalid tag format checks
echo ">> Testing SemVer tag format validation..."
for bad_tag in "0.1.0" "v0.1" "v0.1.0-" "v0.1.0--bad" "v0.1.0.1" "v0.1.0-rc_1" "v0.1.0+build1"; do
  assert_exit_code 1 bash "$PACKAGE_SCRIPT" "$bad_tag" "$TEMP_ROOT/bad"
done
echo "   SemVer tag format validation passed."

# 4. Version mismatch checks against CMakeLists.txt & src/module.cpp
echo ">> Testing version alignment enforcement..."
assert_exit_code 1 bash "$PACKAGE_SCRIPT" "v9.9.9" "$TEMP_ROOT/mismatch"
assert_exit_code 1 bash "$PACKAGE_SCRIPT" "v0.9.0-rc.1" "$TEMP_ROOT/mismatch_rc"
echo "   Version alignment enforcement passed."

# 5. Valid stable tag packaging (v0.3.0)
echo ">> Testing valid stable release packaging (v0.3.0)..."
OUT_STABLE="$TEMP_ROOT/stable"
bash "$PACKAGE_SCRIPT" "v0.3.0" "$OUT_STABLE"

TARBALL_STABLE="$OUT_STABLE/vector-lua-otel-0.3.0.tar.gz"
ROCKSPEC_STABLE="$OUT_STABLE/vector-otel-0.3.0-1.rockspec"
SUMS_STABLE="$OUT_STABLE/SHA256SUMS"
META_STABLE="$OUT_STABLE/release-metadata.env"

[[ -f "$TARBALL_STABLE" ]] || fail "Missing $TARBALL_STABLE"
[[ -f "$ROCKSPEC_STABLE" ]] || fail "Missing $ROCKSPEC_STABLE"
[[ -f "$SUMS_STABLE" ]] || fail "Missing $SUMS_STABLE"
[[ -f "$META_STABLE" ]] || fail "Missing $META_STABLE"

# Check metadata contents
grep -q '^TAG=v0.3.0$' "$META_STABLE" || fail "Expected TAG=v0.3.0 in metadata"
grep -q '^BASE_VERSION=0.3.0$' "$META_STABLE" || fail "Expected BASE_VERSION=0.3.0 in metadata"
grep -q '^FULL_VERSION=0.3.0$' "$META_STABLE" || fail "Expected FULL_VERSION=0.3.0 in metadata"
grep -q '^ROCKSPEC_VERSION=0.3.0-1$' "$META_STABLE" || fail "Expected ROCKSPEC_VERSION=0.3.0-1 in metadata"
grep -q '^IS_PRERELEASE=false$' "$META_STABLE" || fail "Expected IS_PRERELEASE=false in metadata"

# Check archive exclusions (.github, .superpowers, docs/superpowers, etc.)
TAR_LIST="$(tar -tzf "$TARBALL_STABLE")"
if grep -qE '(^|/)(\.github|\.gitattributes|\.gitignore|\.clang-format|\.vscode|\.superpowers|docs/superpowers|sonar-project\.properties)(/|$)' <<< "$TAR_LIST"; then
  fail "Archive contains excluded development/CI paths"
fi

# Check required files in archive
for req in \
  "vector-lua-otel-0.3.0/CMakeLists.txt" \
  "vector-lua-otel-0.3.0/src/module.cpp" \
  "vector-lua-otel-0.3.0/third_party/simdjson/src/simdjson.cpp" \
  "vector-lua-otel-0.3.0/third_party/googletest/CMakeLists.txt"; do
  grep -Fxq "$req" <<< "$TAR_LIST" || fail "Archive missing required file: $req"
done

# Verify SHA256SUMS
(cd "$OUT_STABLE" && sha256sum -c SHA256SUMS >/dev/null) || fail "SHA256SUMS verification failed"

# Verify rockspec with luarocks lint if available
if command -v luarocks >/dev/null 2>&1; then
  luarocks lint "$ROCKSPEC_STABLE" || fail "luarocks lint failed on $ROCKSPEC_STABLE"
fi
grep -q 'version = "0.3.0-1"' "$ROCKSPEC_STABLE" || fail "Rockspec missing version 0.3.0-1"
grep -q 'tag = "v0.3.0"' "$ROCKSPEC_STABLE" || fail "Rockspec missing tag v0.3.0"
echo "   Valid stable release packaging passed."

# 6. Valid pre-release tag packaging (v0.3.0-rc.1)
echo ">> Testing pre-release packaging (v0.3.0-rc.1)..."
OUT_RC="$TEMP_ROOT/rc"
bash "$PACKAGE_SCRIPT" "v0.3.0-rc.1" "$OUT_RC"

TARBALL_RC="$OUT_RC/vector-lua-otel-0.3.0-rc.1.tar.gz"
ROCKSPEC_RC="$OUT_RC/vector-otel-0.3.0.rc.1-1.rockspec"
META_RC="$OUT_RC/release-metadata.env"

[[ -f "$TARBALL_RC" ]] || fail "Missing $TARBALL_RC"
[[ -f "$ROCKSPEC_RC" ]] || fail "Missing $ROCKSPEC_RC"
grep -q '^IS_PRERELEASE=true$' "$META_RC" || fail "Expected IS_PRERELEASE=true for pre-release tag"
grep -q '^ROCKSPEC_VERSION=0.3.0.rc.1-1$' "$META_RC" || fail "Expected ROCKSPEC_VERSION=0.3.0.rc.1-1"
grep -q 'tag = "v0.3.0-rc.1"' "$ROCKSPEC_RC" || fail "Expected tag = v0.3.0-rc.1 in pre-release rockspec"
if command -v luarocks >/dev/null 2>&1; then
  luarocks lint "$ROCKSPEC_RC" || fail "luarocks lint failed on pre-release rockspec $ROCKSPEC_RC"
fi
(cd "$OUT_RC" && sha256sum -c SHA256SUMS >/dev/null) || fail "Pre-release SHA256SUMS verification failed"
echo "   Pre-release packaging passed."

# 7. Deterministic reproducibility check
echo ">> Testing deterministic tarball reproducibility..."
OUT_REPRO="$TEMP_ROOT/repro"
sleep 1
bash "$PACKAGE_SCRIPT" "v0.3.0" "$OUT_REPRO"
HASH_1="$(sha256sum "$TARBALL_STABLE" | awk '{print $1}')"
HASH_2="$(sha256sum "$OUT_REPRO/vector-lua-otel-0.3.0.tar.gz" | awk '{print $1}')"
if [[ "$HASH_1" != "$HASH_2" ]]; then
  fail "Tarball is not reproducible across runs ($HASH_1 != $HASH_2)"
fi
echo "   Deterministic reproducibility passed ($HASH_1)."

# 8. Unpacked tarball standalone build & test verification
if [[ "$SKIP_TARBALL_BUILD" != "true" ]]; then
  echo ">> Testing standalone build and test from unpacked release tarball..."
  UNPACK_DIR="$TEMP_ROOT/unpacked"
  mkdir -p "$UNPACK_DIR"
  tar -xzf "$TARBALL_STABLE" -C "$UNPACK_DIR"
  SRC_DIR="$UNPACK_DIR/vector-lua-otel-0.3.0"
  cmake -S "$SRC_DIR" -B "$SRC_DIR/build" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$SRC_DIR/build" -j"$(nproc)"
  ctest --test-dir "$SRC_DIR/build" --output-on-failure
  (cd "$SRC_DIR" && bash tests/lua/run_tests.sh)
  if command -v readelf >/dev/null 2>&1; then
    if readelf -d "$SRC_DIR/build/vector_otel.so" | grep -q liblua; then
      fail "vector_otel.so built from tarball links liblua"
    fi
  fi
  echo "   Standalone build and test from unpacked tarball passed."
fi

# 9. GitHub Actions release workflow structure & security invariants
echo ">> Testing .github/workflows/release.yml structure..."
RELEASE_WORKFLOW="$REPO_ROOT/.github/workflows/release.yml"
[[ -f "$RELEASE_WORKFLOW" ]] || fail "Missing $RELEASE_WORKFLOW"

grep -qE "tags:\s*\['v\[0-9\]\+\.\[0-9\]\+\.\[0-9\]\+\*'\]" "$RELEASE_WORKFLOW" || fail "release.yml missing SemVer tag trigger"
grep -q "git merge-base --is-ancestor" "$RELEASE_WORKFLOW" || fail "release.yml missing main branch lineage check"
grep -q "scripts/package_release.sh" "$RELEASE_WORKFLOW" || fail "release.yml does not invoke scripts/package_release.sh"
grep -q "verify-and-package:" "$RELEASE_WORKFLOW" || fail "release.yml missing verify-and-package job"
grep -q "publish-release:" "$RELEASE_WORKFLOW" || fail "release.yml missing publish-release job"
grep -q "id-token: write" "$RELEASE_WORKFLOW" || fail "release.yml missing id-token: write permission for keyless Cosign"
grep -q "attestations: write" "$RELEASE_WORKFLOW" || fail "release.yml missing attestations: write permission for SLSA provenance"
grep -q "cosign sign-blob" "$RELEASE_WORKFLOW" || fail "release.yml missing cosign sign-blob step"
grep -q "cosign verify-blob" "$RELEASE_WORKFLOW" || fail "release.yml missing cosign verify-blob self-check step"
grep -q "actions/attest-build-provenance" "$RELEASE_WORKFLOW" || fail "release.yml missing SLSA build provenance step"
grep -q "gh release create" "$RELEASE_WORKFLOW" || fail "release.yml missing gh release create step"
grep -q -- '--exclude "third_party/\.\*"' "$REPO_ROOT/.github/workflows/sonarcloud.yml" || fail "sonarcloud.yml missing third_party/.* gcovr exclusion"
echo "   .github/workflows/release.yml structure passed."

echo "ALL RELEASE PACKAGING TESTS PASSED"
