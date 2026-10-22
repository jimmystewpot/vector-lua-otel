#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

FAIL_UNDER=90.0
BUILD_DIR="${PROJECT_ROOT}/build-cov"

for arg in "$@"; do
  case $arg in
    --fail-under=*)
      FAIL_UNDER="${arg#*=}"
      shift
      ;;
    *)
      ;;
  esac
done

cd "${PROJECT_ROOT}"

echo "=== Building vector_lua_otel with coverage instrumentation ==="
# Remove stale gcda files to prevent counter mismatch errors across builds
find "${BUILD_DIR}" -name "*.gcda" -delete 2>/dev/null || true

cmake -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON
NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
cmake --build "${BUILD_DIR}" -j"${NPROC}"

echo "=== Running CTest Suite ==="
ctest --test-dir "${BUILD_DIR}" --output-on-failure

echo "=== Running Lua Integration Test Suite ==="
BUILD_DIR="${BUILD_DIR}" bash "${PROJECT_ROOT}/tests/lua/run_tests.sh"

echo "=== Generating Code Coverage Summary ==="
python3 - "${BUILD_DIR}" "${FAIL_UNDER}" << 'EOF'
import sys, os, glob, re, subprocess

build_dir = sys.argv[1]
fail_under = float(sys.argv[2])
project_root = os.path.dirname(os.path.abspath(build_dir)) if not os.path.isabs(build_dir) else build_dir

# Detect gcov tool
gcov_cmd = None
if sys.platform == "darwin":
    try:
        proc = subprocess.run(["xcrun", "-f", "llvm-cov"], capture_output=True, text=True)
        if proc.returncode == 0 and proc.stdout.strip():
            gcov_cmd = ["xcrun", "llvm-cov", "gcov"]
    except Exception:
        pass

if not gcov_cmd:
    gcov_cmd = ["gcov"]

gcdas = glob.glob(os.path.join(build_dir, "**", "*.gcda"), recursive=True)
if not gcdas:
    print("Error: No .gcda profile files found in build directory.")
    sys.exit(1)

# Run gcov in build_dir
cmd = gcov_cmd + ["-p"] + gcdas
proc = subprocess.run(cmd, cwd=build_dir, capture_output=True, text=True)

results = {}
cur_file = None
for line in proc.stdout.splitlines():
    if line.startswith("File '"):
        cur_file = line.split("'")[1]
    elif 'Lines executed:' in line and cur_file:
        m = re.search(r'Lines executed:([\d\.]+)% of (\d+)', line)
        if m:
            pct = float(m.group(1))
            total = int(m.group(2))
            # Only track repository source files in src/ and include/vector_otel/
            # Exclude tests/ and third_party/
            if ("/src/" in cur_file or "/include/vector_otel/" in cur_file) and \
               ("third_party" not in cur_file and "tests" not in cur_file):
                # Normalize relative path
                if "vector-lua-otel/" in cur_file:
                    rel = cur_file.split("vector-lua-otel/")[-1]
                    if ".worktrees/" in rel:
                        rel = re.sub(r'^\.worktrees/[^/]+/', '', rel)
                else:
                    rel = os.path.relpath(cur_file, project_root)
                
                if rel not in results or results[rel][1] < total:
                    results[rel] = (pct, total)
        cur_file = None

print(f"{'File':<55} | {'Lines':<8} | {'Covered':<8} | {'Coverage':<8}")
print("-" * 88)
tot_lines = 0
tot_cov_lines = 0
for f in sorted(results.keys()):
    pct, count = results[f]
    cov_count = int(round(pct * count / 100.0))
    tot_lines += count
    tot_cov_lines += cov_count
    print(f"{f:<55} | {count:<8} | {cov_count:<8} | {pct:>6.2f}%")

print("-" * 88)
overall_pct = (tot_cov_lines / tot_lines * 100.0) if tot_lines else 0.0
print(f"{'TOTAL':<55} | {tot_lines:<8} | {tot_cov_lines:<8} | {overall_pct:>6.2f}%")
print("-" * 88)

if overall_pct < fail_under:
    print(f"FAILED: Coverage {overall_pct:.2f}% is below required threshold of {fail_under:.2f}%")
    sys.exit(1)
else:
    print(f"PASSED: Coverage {overall_pct:.2f}% satisfies required threshold of {fail_under:.2f}%")
    sys.exit(0)
EOF
