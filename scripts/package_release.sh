#!/usr/bin/env bash
# Deterministic release packaging and validation script for vector-lua-otel
# Usage: bash scripts/package_release.sh <tag> [output_dir]
#
# Exit codes:
#   0 = validation and packaging succeeded
#   1 = validation, version alignment, archive hygiene, or lint check failed
#   2 = CLI usage error

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 <tag> [output_dir]" >&2
  exit 2
fi

TAG="$1"
OUTPUT_DIR="${2:-dist}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"

# 1. Validate SemVer tag format: v<MAJOR>.<MINOR>.<PATCH>[-<PRERELEASE>]
SEMVER_REGEX='^v([0-9]+\.[0-9]+\.[0-9]+)(-([0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*))?$'
if [[ ! "$TAG" =~ $SEMVER_REGEX ]]; then
  echo "ERROR: Invalid release tag '$TAG'. Expected v<MAJOR>.<MINOR>.<PATCH>[-<PRERELEASE>]" >&2
  exit 1
fi

BASE_VERSION="${BASH_REMATCH[1]}"
PRERELEASE_SUFFIX="${BASH_REMATCH[2]:-}"
PRERELEASE_ID="${BASH_REMATCH[3]:-}"

if [[ -n "$PRERELEASE_ID" ]]; then
  if [[ "$PRERELEASE_ID" == *--* || "$PRERELEASE_ID" == -* || "$PRERELEASE_ID" == *- ]]; then
    echo "ERROR: Invalid pre-release identifier '$PRERELEASE_ID' in tag '$TAG'" >&2
    exit 1
  fi
  IS_PRERELEASE="true"
else
  IS_PRERELEASE="false"
fi

FULL_VERSION="${BASE_VERSION}${PRERELEASE_SUFFIX}"
ROCKSPEC_VERSION="${FULL_VERSION//-/.}-1"

# 2. Verify BASE_VERSION matches CMakeLists.txt and src/module.cpp
CMAKE_FILE="$REPO_ROOT/CMakeLists.txt"
MODULE_FILE="$REPO_ROOT/src/module.cpp"

CMAKE_VERSION="$(sed -nE 's/^project\(vector_lua_otel[[:space:]]+VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$CMAKE_FILE")"
MODULE_VERSION="$(awk '/lua_setfield\(L, -2, "_VERSION"\)/ { print prev } { prev = $0 }' "$MODULE_FILE" | sed -nE 's/.*lua_pushstring\(L,[[:space:]]*"([^"]+)"\).*/\1/p')"

if [[ -z "$CMAKE_VERSION" ]]; then
  echo "ERROR: Failed to extract PROJECT_VERSION from $CMAKE_FILE" >&2
  exit 1
fi

if [[ -z "$MODULE_VERSION" ]]; then
  echo "ERROR: Failed to extract _VERSION from $MODULE_FILE" >&2
  exit 1
fi

if [[ "$BASE_VERSION" != "$CMAKE_VERSION" ]]; then
  echo "ERROR: Tag base version '$BASE_VERSION' ($TAG) does not match CMakeLists.txt version '$CMAKE_VERSION'" >&2
  exit 1
fi

if [[ "$BASE_VERSION" != "$MODULE_VERSION" ]]; then
  echo "ERROR: Tag base version '$BASE_VERSION' ($TAG) does not match src/module.cpp _VERSION '$MODULE_VERSION'" >&2
  exit 1
fi

# 3. Verify vendored dependencies
echo "=== Verifying vendored dependencies ==="
if ! bash "$REPO_ROOT/scripts/vendor_deps.sh" --verify >/dev/null; then
  echo "ERROR: Vendored dependency verification failed" >&2
  exit 1
fi

# 4. Prepare output directory
mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"

TARBALL_NAME="vector-lua-otel-${FULL_VERSION}.tar.gz"
TARBALL_PATH="$OUTPUT_DIR/$TARBALL_NAME"
ROCKSPEC_NAME="vector-otel-${ROCKSPEC_VERSION}.rockspec"
ROCKSPEC_PATH="$OUTPUT_DIR/$ROCKSPEC_NAME"

# 5. Build deterministic source archive via git archive + gzip -n
echo "=== Creating deterministic source tarball: $TARBALL_NAME ==="
git -C "$REPO_ROOT" archive \
  --worktree-attributes \
  --format=tar \
  --prefix="vector-lua-otel-${FULL_VERSION}/" \
  HEAD | gzip -n -9 > "$TARBALL_PATH"

# 6. Verify archive hygiene (.github and internal paths excluded, core + third_party included)
TAR_LIST="$(tar -tzf "$TARBALL_PATH")"
if grep -qE '(^|/)(\.github|\.gitattributes|\.gitignore|\.clang-format|\.vscode|\.superpowers|docs/superpowers|sonar-project\.properties)(/|$)' <<< "$TAR_LIST"; then
  echo "ERROR: Release tarball '$TARBALL_NAME' contains forbidden internal/CI paths" >&2
  exit 1
fi

for req_path in \
  "vector-lua-otel-${FULL_VERSION}/CMakeLists.txt" \
  "vector-lua-otel-${FULL_VERSION}/src/module.cpp" \
  "vector-lua-otel-${FULL_VERSION}/third_party/simdjson/src/simdjson.cpp" \
  "vector-lua-otel-${FULL_VERSION}/third_party/googletest/CMakeLists.txt"; do
  if ! grep -Fxq "$req_path" <<< "$TAR_LIST"; then
    echo "ERROR: Release tarball '$TARBALL_NAME' is missing required path '$req_path'" >&2
    exit 1
  fi
done

# 7. Generate version-pinned rockspec and lint
echo "=== Generating versioned rockspec: $ROCKSPEC_NAME ==="
awk -v ver="$ROCKSPEC_VERSION" -v tag="$TAG" '
  /^version = / {
    print "version = \"" ver "\""
    next
  }
  /url = "git\+https:\/\/github\.com\/jimmystewpot\/vector-lua-otel\.git"/ {
    print "   url = \"git+https://github.com/jimmystewpot/vector-lua-otel.git\","
    print "   tag = \"" tag "\""
    next
  }
  { print }
' "$REPO_ROOT/vector-otel-scm-1.rockspec" > "$ROCKSPEC_PATH"

if command -v luarocks >/dev/null 2>&1; then
  luarocks lint "$ROCKSPEC_PATH"
fi

# 8. Generate and verify SHA256SUMS
echo "=== Generating SHA256SUMS ==="
(
  cd "$OUTPUT_DIR"
  sha256sum "$TARBALL_NAME" "$ROCKSPEC_NAME" > SHA256SUMS
  sha256sum -c SHA256SUMS >/dev/null
)

# 9. Write release-metadata.env for downstream CI steps
cat > "$OUTPUT_DIR/release-metadata.env" <<EOF
TAG=${TAG}
BASE_VERSION=${BASE_VERSION}
FULL_VERSION=${FULL_VERSION}
ROCKSPEC_VERSION=${ROCKSPEC_VERSION}
IS_PRERELEASE=${IS_PRERELEASE}
TARBALL_NAME=${TARBALL_NAME}
ROCKSPEC_NAME=${ROCKSPEC_NAME}
EOF

echo "=== Release packaging complete in $OUTPUT_DIR ==="
