#!/usr/bin/env bash
# Throughput harness: stdin source -> <transform variant> -> blackhole sink.
#
#   bench/run.sh [-v vector_bin] [-b batches] [-r records_per_batch] [-n reps] <pipeline.yaml.tmpl> <transform-file>
#
# <transform-file> is substituted for @VRL@ in the template (a .vrl file today;
# a Lua variant can reuse the same harness with its own template).
# Reports median wall-clock records/s and MB/s, net of a measured startup cost.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
VECTOR="${VECTOR:-vector}"
BATCHES=1000
RECORDS=100
REPS=5

while getopts "v:b:r:n:" o; do
  case $o in
    v) VECTOR="$OPTARG" ;;
    b) BATCHES="$OPTARG" ;;
    r) RECORDS="$OPTARG" ;;
    n) REPS="$OPTARG" ;;
    *) exit 2 ;;
  esac
done
shift $((OPTIND - 1))
TMPL="${1:?pipeline template}"; XFORM="$(realpath "${2:?transform file}")"

DATA="$HERE/.data"; mkdir -p "$DATA"
FIX="$DATA/perf_${BATCHES}x${RECORDS}.ndjson"
WARM="$DATA/perf_1x1.ndjson"
[[ -f "$FIX" ]]  || python3 "$HERE/gen_fixtures.py" perf --batches "$BATCHES" --records "$RECORDS" > "$FIX"
[[ -f "$WARM" ]] || python3 "$HERE/gen_fixtures.py" perf --batches 1 --records 1 > "$WARM"

CFG="$DATA/run.yaml"
SINK=$'    type: blackhole\n    inputs: [extract]\n    print_interval_secs: 3600'
LIBDIR="$(dirname "$XFORM")"
sed -e "s|@VRL@|$XFORM|" -e "s|@XFORM@|$XFORM|" -e "s|@LIBDIR@|$LIBDIR|" -e "s|@SINK@|${SINK//$'\n'/\\n}|" "$TMPL" > "$CFG"

TIME_TMP="$(mktemp)"
trap 'rm -f "$TIME_TMP"' EXIT

now() { date +%s.%N; }
run_once() { # $1 = input file -> outputs: seconds rss_kb page_faults
  local s e duration rss faults
  s=$(now)
  /usr/bin/time -v "$VECTOR" --quiet --config "$CFG" < "$1" > /dev/null 2> "$TIME_TMP"
  e=$(now)
  duration=$(echo "$e - $s" | bc -l)
  rss=$(awk -F': ' '/Maximum resident set size/ {print $2}' "$TIME_TMP")
  faults=$(awk -F': ' '/Minor \(reclaiming a frame\) page faults/ {print $2}' "$TIME_TMP")
  echo "$duration $rss $faults"
}

median() { sort -n | awk '{a[NR]=$1} END{print (NR%2)?a[(NR+1)/2]:(a[NR/2]+a[NR/2+1])/2}'; }

startup=$(for _ in 1 2 3; do run_once "$WARM" | awk '{print $1}'; done | median)
raw_runs=$(for _ in $(seq "$REPS"); do run_once "$FIX"; done)
med=$(echo "$raw_runs" | awk '{print $1}' | median)
med_rss_kb=$(echo "$raw_runs" | awk '{print $2}' | median)
med_faults=$(echo "$raw_runs" | awk '{print $3}' | median)
med_rss_mb=$(echo "$med_rss_kb / 1024" | bc -l)

total=$((BATCHES * RECORDS))
mb=$(echo "$(stat -c %s "$FIX") / 1048576" | bc -l)
net=$(echo "$med - $startup" | bc -l)
printf 'variant=%s\n' "$(basename "$XFORM")"
printf 'records=%d  input=%.1fMB  reps=%d  cores=%s\n' "$total" "$mb" "$REPS" "$(nproc)"
printf 'startup=%.3fs  median_wall=%.3fs  net=%.3fs\n' "$startup" "$med" "$net"
printf 'throughput: %.0f records/s   %.1f MB/s (net)\n' \
  "$(echo "$total / $net" | bc -l)" "$(echo "$mb / $net" | bc -l)"
printf 'memory: %.2f MB max_rss   %.0f page_faults\n' \
  "$med_rss_mb" "$med_faults"

