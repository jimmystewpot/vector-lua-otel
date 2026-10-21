#!/bin/bash
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
  echo "=== Exporting clean tree ===" >&2
  # Create temp directory in /Users for Docker compatibility on macOS
  local EXPORT_DIR
  EXPORT_DIR="/Users/jlamb/.docker_temp_$$_tmp_rovo_$(date +%s%N)"
  mkdir -p "$EXPORT_DIR"
  echo "Export directory: $EXPORT_DIR" >&2
  
  # Copy tracked and untracked files (excluding ignored)
  while IFS= read -r file; do
    mkdir -p "$EXPORT_DIR/$(dirname "$file")"
    cp "$REPO_ROOT/$file" "$EXPORT_DIR/$file"
  done < <(git -C "$REPO_ROOT" ls-files -co --exclude-standard)
  
  printf "%s\n" "$EXPORT_DIR"
}

run_container() {
  local tmp_dir="$1"
  
  echo "=== Running container ==="
  
  if [[ "$ALLOW_NETWORK" == "true" ]]; then
    docker run --rm \
      -v "$tmp_dir:/src" \
      -w /src \
      vector-otel-offline \
      bash -euo pipefail -c 'set -euo pipefail
echo "=== CMake Configure ==="
cmake -B build -DCMAKE_BUILD_TYPE=Release -GNinja 2>&1 | tee configure.log
echo "=== Building ==="
cmake --build build
echo "=== Running CTest ==="
ctest --test-dir build --output-on-failure
echo "=== Running Lua Tests ==="
bash tests/lua/run_tests.sh
echo "=== Checking Memory Delta ==="
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
echo "=== Checking for Network Activity in Configure ==="
if grep -qiE "git clone|Downloading|FetchContent" configure.log; then
  echo "ERROR: Network activity detected in configure log" >&2
  exit 1
fi
echo "OFFLINE BUILD OK"'
  else
    docker run --rm \
      --network none \
      -v "$tmp_dir:/src" \
      -w /src \
      vector-otel-offline \
      bash -euo pipefail -c 'set -euo pipefail
echo "=== CMake Configure ==="
cmake -B build -DCMAKE_BUILD_TYPE=Release -GNinja 2>&1 | tee configure.log
echo "=== Building ==="
cmake --build build
echo "=== Running CTest ==="
ctest --test-dir build --output-on-failure
echo "=== Running Lua Tests ==="
bash tests/lua/run_tests.sh
echo "=== Checking Memory Delta ==="
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
echo "=== Checking for Network Activity in Configure ==="
if grep -qiE "git clone|Downloading|FetchContent" configure.log; then
  echo "ERROR: Network activity detected in configure log" >&2
  exit 1
fi
echo "OFFLINE BUILD OK"'
  fi
}

# ============================================================================
# Host-side pre-checks
# ============================================================================

# (Placeholder for Task 4: scripts/vendor_deps.sh --verify will go here)

# ============================================================================
# Main flow
# ============================================================================

# Create temp directory
TEMP_DIR=$(export_tree)
trap "rm -rf '$TEMP_DIR'" EXIT

# Build Docker image
build_image

# Run container
run_container "$TEMP_DIR"
