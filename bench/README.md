# Benchmark harness

Baseline for the C++/Lua module: OTLP/JSON logs -> flat, column-per-field events.

```bash
# needs: vector (>= 0.59), python3, bc
VECTOR=/path/to/vector bench/run.sh -b 1000 -r 100 -n 5 \
  bench/pipelines/vrl_logs.yaml.tmpl bench/vrl/logs.vrl
```

- `gen_fixtures.py perf|edge` – deterministic OTLP/JSON NDJSON (one batch per line).
- `vrl/logs.vrl` – VRL reference implementation (also the semantic reference for column
  types: ISO-8601 nanosecond timestamp strings, int64 preserved, attributes as objects).
- `run.sh` – stdin -> transform -> blackhole; median wall-clock net of startup.
  A Lua variant plugs in by supplying its own pipeline template.
- Correctness: run the `edge` fixture through a `file` sink and diff against golden output
  (ignore `processed_at`). Expected: 8 events from the 6 edge batches.

## Gotchas found
- VRL closures clone their enclosing scope per invocation; capturing the whole batch inside
  nested `map_values`/`for_each` made cost quadratic in records-per-batch. Hoist what you need
  and null the large variable first.
- `blackhole` with `print_interval_secs: 0` busy-loops after stdin EOF; use a large value.

## Per-call extraction latency (`vector_otel_latency_bench`)

Opt-in target that drives `<module>.<signal>.process(payload, emit)` in one standard Lua 5.4 state
with a counting, non-buffering callback and reports latency quantiles for the `lua_pcall` only.

```bash
cmake -B build-bench -DCMAKE_BUILD_TYPE=Release -DBUILD_EXTRACTION_BENCHMARKS=ON
cmake --build build-bench --target vector_otel_latency_bench
./build-bench/vector_otel_latency_bench --help
./build-bench/vector_otel_latency_bench --module native --signal metrics --mode table \
    --records 64 --iterations 3000 --warmup 300 --cold --seed 1 --sha "$(git rev-parse --short HEAD)"
```

- Options: `--module otel|native`, `--signal logs|traces|metrics`, `--mode json|table`,
  `--records 1..2048`, `--bytes B` (pads the fixture to about B bytes without changing the record
  count), `--iterations`, `--warmup`, `--cold` (reports the first call separately as
  `cold_first_call_ns`), `--seed`, `--sha`, `--format json|csv`, `--allow-reject`.
- Fixtures are the deterministic builders in `tests/support/signal_payloads.hpp`. A fixture above the
  configured payload limit is **rejected explicitly** (exit 2) instead of silently shrunk; pass
  `--allow-reject` to time the rejection path.
- Output includes the fixture seed, git SHA, compiler, architecture, configured limit `C`, emit
  batch size `N`, error and callback counts, mean/p50/p95/p99/p99.9/max latency, rows/s and payload
  MiB/s. Exit status is non-zero when any call errored or the delivered row count is wrong.
- Allocation counts are **not** measured here (the field is `null`): allocator instrumentation
  perturbs CPU timing. Use the allocator fault harness for allocation domains.
- The same source also builds against a pre-limit checkout (no `process_runner.hpp`; `C`/`N`
  report as 0), so baseline and final run the identical fixture/driver contract:
  compile `bench/extraction_latency.cpp` with `-I tests/support` (from the branch under test) plus
  the baseline's `src/module.cpp`, `src/native_module.cpp`, core library and simdjson.

Reference numbers (macOS arm64, Homebrew clang 23 / libc++, Release, no-op counting callback,
`N=1`, `C=20 MiB`; baseline `c1e1b05` vs this branch, p50 per call). The callback never touches the
emitted table, so these are the *module's own* costs; a callback that reads the event (as Vector's
does) amortises the difference:

| records | otel logs JSON | otel traces JSON | native metrics table |
|:---|:---|:---|:---|
| 1 | 1.1 µs → 3.5 µs | 1.5 µs → 2.4 µs | 1.8 µs → 2.3 µs |
| 64 | 58.7 µs → 71.2 µs (+21%) | 92.7 µs → 103.3 µs (+11%) | 126.9 µs → 139.0 µs (+9.5%) |
| 2048 | 1.75 ms → 2.05 ms (+17.5%) | 2.98 ms → 3.25 ms (+9%) | 3.75 ms → 4.39 ms (+17%) |

Across the full 36-cell matrix (2 modules × 3 signals × json/table × 1/64/2048 records) p50 was
**+3% to +25% slower at 64–2048 records** and +17% to +215% (an absolute +0.3 to +2.4 µs) at one
record. This is the price of the failure-safe design: a protected frame per call and per row, a
preflight pass, and staging each row before materialising it. It is a measured regression that
the deployment owner needs to approve; it is not a latency SLO.
