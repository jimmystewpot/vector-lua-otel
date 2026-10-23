#!/usr/bin/env bash
# Comparative Throughput & Memory Benchmark:
# 1. Vector VRL Baseline
# 2. vector_otel Lua Flat (Emit Batch Size N = 1)
# 3. vector_otel Lua Matched (Emit Batch Size N = Input Batch Size)
#
# Evaluates OTLP JSON logs transformation throughput & process memory across batch sizes N in {1, 16, 128}.
#
# Usage:
#   bench/compare.sh [-v vector_bin] [-t total_records] [-n reps]
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

# Discover Vector binary
if [[ -z "${VECTOR:-}" ]]; then
  if [[ -x "/tmp/vector-bench/vector-x86_64-unknown-linux-gnu/bin/vector" ]]; then
    VECTOR="/tmp/vector-bench/vector-x86_64-unknown-linux-gnu/bin/vector"
  elif command -v vector >/dev/null 2>&1; then
    VECTOR="$(command -v vector)"
  else
    echo "ERROR: vector binary not found. Set VECTOR=/path/to/vector or install vector." >&2
    exit 1
  fi
fi

TOTAL_RECORDS=100000
REPS=2

usage() {
  cat <<EOF
Usage: $0 [-v vector_bin] [-t total_records] [-n reps]

Options:
  -v VECTOR       Path to Vector binary (default: $VECTOR)
  -t RECORDS      Target total records to benchmark (default: $TOTAL_RECORDS)
  -n REPS         Number of repetitions per test (default: $REPS)
  -h              Show this help message
EOF
  exit 1
}

while getopts "v:t:n:h" opt; do
  case "$opt" in
    v) VECTOR="$OPTARG" ;;
    t) TOTAL_RECORDS="$OPTARG" ;;
    n) REPS="$OPTARG" ;;
    h) usage ;;
    *) usage ;;
  esac
done

if [[ ! -x "$VECTOR" ]]; then
  echo "ERROR: Vector binary '$VECTOR' does not exist or is not executable." >&2
  exit 1
fi

# Ensure vector_otel is compiled
SO="$ROOT/build_bench/vector_otel.so"
if [[ ! -f "$SO" ]]; then
  echo "Compiling vector_otel..."
  cmake -B "$ROOT/build_bench" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$ROOT/build_bench" --target vector_otel -j$(nproc)
fi

VRL_TMPL="$HERE/pipelines/vrl_logs.yaml.tmpl"
VRL_XFORM="$HERE/vrl/logs.vrl"
LUA_TMPL="$HERE/pipelines/lua_logs.yaml.tmpl"

BATCH_SIZES=(1 16 128)

echo "========================================================================================================================"
echo " Comparative Benchmark: Vector VRL vs vector_otel Lua Flat Streaming"
echo " Vector:  $($VECTOR --version | head -n1)"
echo " Target:  $TOTAL_RECORDS records across N in {1, 16, 128} (Reps: $REPS, Cores: $(nproc))"
echo " Profiling: Throughput (rec/s, MB/s), Peak Max RSS (MB), Minor Page Faults"
echo "========================================================================================================================"
printf "\n"

# Arrays to store results for final summary tables
declare -A r_vrl_rec r_vrl_mb r_vrl_rss r_vrl_faults
declare -A r_lua_rec r_lua_mb r_lua_rss r_lua_faults
declare -A r_fixture_mb r_actual_total

for N in "${BATCH_SIZES[@]}"; do
  BATCHES=$(( (TOTAL_RECORDS + N - 1) / N ))
  ACTUAL_TOTAL=$(( BATCHES * N ))
  r_actual_total[$N]=$ACTUAL_TOTAL

  echo ">>> Testing Batch Size N = $N ($ACTUAL_TOTAL records, $BATCHES batches) ..."

  # 1. Run VRL baseline
  VRL_OUT="$(VECTOR="$VECTOR" bash "$HERE/run.sh" -b "$BATCHES" -r "$N" -n "$REPS" "$VRL_TMPL" "$VRL_XFORM")"
  VRL_REC="$(echo "$VRL_OUT" | awk '/throughput:/ {print $2}')"
  VRL_MB="$(echo "$VRL_OUT" | awk '/throughput:/ {print $4}')"
  VRL_RSS="$(echo "$VRL_OUT" | awk '/memory:/ {print $2}')"
  VRL_FAULTS="$(echo "$VRL_OUT" | awk '/memory:/ {print $5}')"
  MB_INPUT="$(echo "$VRL_OUT" | awk -F'[ =]+' '/input=/ {for(i=1;i<=NF;i++) if($i=="input") print $(i+1)}')"
  r_vrl_rec[$N]=$VRL_REC
  r_vrl_mb[$N]=$VRL_MB
  r_vrl_rss[$N]=$VRL_RSS
  r_vrl_faults[$N]=$VRL_FAULTS
  r_fixture_mb[$N]=$MB_INPUT

  # 2. Run Lua Flat Streaming
  LUA_OUT="$(VECTOR="$VECTOR" bash "$HERE/run.sh" -b "$BATCHES" -r "$N" -n "$REPS" "$LUA_TMPL" "$SO")"
  LUA_REC="$(echo "$LUA_OUT" | awk '/throughput:/ {print $2}')"
  LUA_MB="$(echo "$LUA_OUT" | awk '/throughput:/ {print $4}')"
  LUA_RSS="$(echo "$LUA_OUT" | awk '/memory:/ {print $2}')"
  LUA_FAULTS="$(echo "$LUA_OUT" | awk '/memory:/ {print $5}')"
  r_lua_rec[$N]=$LUA_REC
  r_lua_mb[$N]=$LUA_MB
  r_lua_rss[$N]=$LUA_RSS
  r_lua_faults[$N]=$LUA_FAULTS
done

echo ""
echo "========================================================================================================================"
echo " Table 1: Throughput & Speedup Comparison"
echo "========================================================================================================================"
printf "%-10s | %-10s | %-10s | %-12s | %-12s | %-10s\n" \
  "Batch (N)" "Records" "Size (MB)" "VRL (rec/s)" "Lua Flat" "Speedup"
printf "%-10s-+-%-10s-+-%-10s-+-%-12s-+-%-12s-+-%-10s\n" \
  "----------" "----------" "----------" "------------" "------------" "----------"

for N in "${BATCH_SIZES[@]}"; do
  spd="$(awk -v l="${r_lua_rec[$N]}" -v v="${r_vrl_rec[$N]}" 'BEGIN { if (v > 0) printf "%.2fx", l / v; else print "N/A" }')"
  printf "%-10d | %-10d | %-10s | %-12s | %-12s | %-10s\n" \
    "$N" "${r_actual_total[$N]}" "${r_fixture_mb[$N]}" "${r_vrl_rec[$N]}" "${r_lua_rec[$N]}" "$spd"
done

echo ""
echo "========================================================================================================================"
echo " Table 2: Memory Utilization & Minor Page Faults Comparison"
echo "========================================================================================================================"
printf "%-10s | %-14s | %-14s | %-16s | %-12s | %-12s | %-16s\n" \
  "Batch (N)" "VRL RSS (MB)" "Lua RSS (MB)" "RSS Reduction" "VRL Faults" "Lua Faults" "Faults Reduction"
printf "%-10s-+-%-14s-+-%-14s-+-%-16s-+-%-12s-+-%-12s-+-%-16s\n" \
  "----------" "--------------" "--------------" "----------------" "------------" "------------" "----------------"

for N in "${BATCH_SIZES[@]}"; do
  red_rss="$(awk -v l="${r_lua_rss[$N]}" -v v="${r_vrl_rss[$N]}" 'BEGIN { if (v > 0) printf "-%.1f%%", (1.0 - l / v) * 100; else print "N/A" }')"
  red_flt="$(awk -v l="${r_lua_faults[$N]}" -v v="${r_vrl_faults[$N]}" 'BEGIN { if (v > 0) printf "-%.1f%%", (1.0 - l / v) * 100; else print "N/A" }')"
  printf "%-10d | %-14s | %-14s | %-16s | %-12s | %-12s | %-16s\n" \
    "$N" "${r_vrl_rss[$N]}" "${r_lua_rss[$N]}" "$red_rss" "${r_vrl_faults[$N]}" "${r_lua_faults[$N]}" "$red_flt"
done

echo "========================================================================================================================"
echo "Benchmark completed successfully."

