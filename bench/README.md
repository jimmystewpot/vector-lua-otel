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
