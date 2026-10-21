#!/bin/bash
# Offline build verification harness for Ubuntu 22.04
# Usage: bash scripts/verify_offline_build.sh [--allow-network]
#
# Note: On macOS, /tmp is not shared with Docker. By default, this script uses
# $HOME/tmp. To override, set OFFLINE_TMPDIR before running:
#   OFFLINE_TMPDIR=/path/to/shared/dir bash scripts/verify_offline_build.sh
#
# Exit codes:
#   0 = all checks passed
#   1 = check failed
#   2 = usage error

set -euo pipefail

# Resolve repo root from script location
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"
SCRIPTS_OFFLINE="$REPO_ROOT/scripts/offline"

# Parse flags
ALLOW_NETWORK=false
while [[ $# -gt 0 ]]; do
  case "$1" in
    --allow-network)
      ALLOW_NETWORK=true
      shift
      ;;
    *)
      echo "Usage: $0 [--allow-network]" >&2
      exit 2
      ;;
  esac
done

# ============================================================================
# Helper functions
# ============================================================================

build_image() {
  echo "=== Building Docker image ==="
  docker build -t vector-otel-offline "$SCRIPTS_OFFLINE"
}

export_tree() {
  local export_dir="$1"
  
  echo "=== Exporting clean tree ===" >&2
  echo "Export directory: $export_dir" >&2
  
  # Copy tracked and untracked files (excluding ignored)
  while IFS= read -r file; do
    mkdir -p "$export_dir/$(dirname "$file")"
    cp "$REPO_ROOT/$file" "$export_dir/$file"
  done < <(git -C "$REPO_ROOT" ls-files -co --exclude-standard)
}

run_container() {
  local tmp_dir="$1"
  
  echo "=== Running container ==="
  
  # Container command script (run once, shared between both branches)
  local container_script
  read -r -d '' container_script << 'CONTAINER_SCRIPT_EOF' || true
set -euo pipefail
echo "=== CMake Configure ==="
cmake -B build -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_QUIET=OFF --no-warn-unused-cli -GNinja 2>&1 | tee configure.log
echo "=== CMake Configure (BUILD_SHARED_LIBS=ON) ==="
cmake -B build-shared -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DFETCHCONTENT_QUIET=OFF --no-warn-unused-cli -GNinja 2>&1 | tee -a configure.log
test -f build-shared/third_party/simdjson/simdjson.pc
rm -rf build-shared
echo "=== Building ==="
cmake --build build
echo "=== Running CTest ==="
ctest --test-dir build --output-on-failure
echo "=== Running Lua Tests ==="
bash tests/lua/run_tests.sh 2>&1 | tee lua_test.log
if ! grep -q "Memory Delta: 0.00 KB" lua_test.log; then
  echo "ERROR: Memory leaks detected" >&2
  exit 1
fi
echo "=== Checking Formatting ==="
find include src tests/cpp -type f \( -name "*.h" -o -name "*.hpp" -o -name "*.c" -o -name "*.cpp" \) | xargs -r clang-format --dry-run --Werror
echo "=== Checking Symbol Dependencies ==="
if readelf -d build/vector_otel.so | grep -q liblua; then
  echo "ERROR: vector_otel.so incorrectly links to liblua" >&2
  exit 1
fi
echo "=== Checking for Download Activity ==="
if grep -qiE "Populating|Cloning into|git clone|Downloading|FetchContent_" configure.log; then
  echo "ERROR: Download activity detected in configure log" >&2
  exit 1
fi
if [[ -d build/_deps ]]; then
  echo "ERROR: Found build/_deps directory (FetchContent downloaded dependencies)" >&2
  exit 1
fi
echo "OFFLINE BUILD OK"
CONTAINER_SCRIPT_EOF
  
  # Build Docker run args (order matters: flags before image name)
  local -a docker_args=(--rm)
  
  # On Linux CI, set user to avoid permission issues on cleanup
  if [[ "$(uname)" == "Linux" ]]; then
    docker_args+=(--user "$(id -u):$(id -g)" -e HOME=/tmp)
  fi
  
  # Add network restriction if not allowed
  if [[ "$ALLOW_NETWORK" != "true" ]]; then
    docker_args+=(--network none)
  fi
  
  # Add mount and working directory (must come after flags, before image)
  docker_args+=(-v "$tmp_dir:/src" -w /src vector-otel-offline)
  
  # Run container with script
  docker run "${docker_args[@]}" bash -euo pipefail -c "$container_script"
}

# ============================================================================
# Host-side pre-checks
# ============================================================================

# Verify vendored dependencies before building in container
echo "=== Verifying vendored dependencies ==="
if ! bash "$REPO_ROOT/scripts/vendor_deps.sh" --verify; then
  echo "ERROR: Vendored dependency verification failed" >&2
  exit 1
fi

# ============================================================================
# Main flow
# ============================================================================

# Create temp directory using portable mktemp
# On macOS, /tmp is not shared with Docker, so default to $HOME/tmp
# Can override with OFFLINE_TMPDIR environment variable
tmpdir="${OFFLINE_TMPDIR:-$HOME/tmp}"
mkdir -p "$tmpdir"
TEMP_DIR=$(mktemp -d "$tmpdir/tmp_rovo_offline.XXXXXX")

# Install cleanup trap immediately after creating the directory
trap "rm -rf '$TEMP_DIR'" EXIT

# Export the clean tree
export_tree "$TEMP_DIR"

# Build Docker image
build_image

# Run container
run_container "$TEMP_DIR"
