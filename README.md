# vector-lua-otel

[![License: Apache 2.0](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)
[![Standard: C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Lua: 5.4](https://img.shields.io/badge/Lua-5.4-blue.svg)](https://www.lua.org/)
[![simdjson: 5.0.5](https://img.shields.io/badge/simdjson-5.0.5-brightgreen.svg)](https://simdjson.org/)
[![codecov](https://codecov.io/gh/jimmystewpot/vector-lua-otel/graph/badge.svg)](https://codecov.io/gh/jimmystewpot/vector-lua-otel)

**vector-lua-otel** is an ultra-high-performance C++20 dynamic module (`vector_otel.so`) for [DataDog Vector](https://vector.dev/)'s Lua 5.4 transforms (`type = "lua"`, `version = "2"`). It ingests raw OpenTelemetry (OTLP) JSON batch payloads (`ExportLogsServiceRequest`, `ExportTraceServiceRequest`, `ExportMetricsServiceRequest`), parses them using [`simdjson`](https://simdjson.org/) 5.0.5 on-demand SIMD parsing, flattens hierarchical records into ClickHouse-aligned table columns, and streams extracted events directly into Vector's pipeline via the `emit` callback with $O(1)$ streaming memory.

---

## Table of Contents

- [Why vector-lua-otel?](#why-vector-lua-otel)
- [Architecture and Data Flow](#architecture-and-data-flow)
- [Performance and Benchmarks](#performance-and-benchmarks)
  - [Throughput & Speedup Comparison](#table-1-throughput-and-speedup-comparison)
  - [Memory Utilisation & Allocation Footprint](#table-2-memory-utilisation--allocation-footprint)
  - [Key Takeaways](#key-takeaways)
- [Prerequisites and Toolchain](#prerequisites-and-toolchain)
- [Build and Installation](#build-and-installation)
  - [Signed Release Tarball](#signed-release-tarball-recommended-for-production)
  - [CMake Build](#cmake-build)
  - [LuaRocks Installation](#luarocks-installation)
  - [Build Options](#build-options)
- [Vector Configuration](#vector-configuration)
  - [YAML Configuration (OTLP Source -> S3 Parquet Sink)](#yaml-configuration-industry-standard-otlp-source---lua-transform---s3-parquet-sink)
  - [TOML Configuration](#toml-configuration)
  - [Configuration Invariants](#configuration-invariants)
- [Lua Module API Reference](#lua-module-api-reference)
  - [Module Structure](#module-structure)
  - [Function Signatures](#function-signatures)
  - [Return and Error Contract](#return-and-error-contract)
- [Telemetry Schema Mappings](#telemetry-schema-mappings)
  - [Parquet Schema Reference](schemas/parquet/README.md)
  - [Logs Schema](#logs-schema)
  - [Traces Schema](#traces-schema)
  - [Metrics Schema](#metrics-schema)
- [Testing and Quality Assurance](#testing-and-quality-assurance)
- [License](#license)

---

## Why vector-lua-otel?

When receiving OTLP JSON batches through Vector's HTTP, Kafka, or file sources, processing deeply nested arrays of Resource $\rightarrow$ Scope $\rightarrow$ Record items in native Vector Remap Language (VRL) suffers from significant performance bottlenecks:

1. **Quadratic Closure Overhead**: Iterating over nested arrays with closures in VRL creates heavy allocation churn and degrades throughput at typical production batch sizes ($N \ge 100$).
2. **Dynamic Allocation & GC Pressure**: Pure Lua implementations create tens of thousands of temporary tables per batch, causing massive garbage collector pauses.
3. **Loss of 64-bit Integer Precision**: Standard JSON parsers often cast 64-bit numbers (`timeUnixNano`, `duration`, `intValue`) to 64-bit floating-point doubles, corrupting high-precision nanosecond timestamps beyond 53 bits.

**vector-lua-otel solves this by:**
- Utilising **simdjson 5.0.5 On-Demand** parsing with AVX-512, AVX2, SSE4.2, and ARM NEON hardware acceleration.
- Maintaining **$O(1)$ streaming memory**: extracting and emitting records incrementally without buffering the batch in C++ or Lua.
- Reusing thread-local string buffers (strings read from Lua or JSON are copied into reusable scratch buffers, never borrowed across `emit` calls) to achieve **zero heap allocations in hot extraction loops**.
- Preserving **exact 64-bit integers** (via Lua 5.4 native integer support) and formatting all timestamps as **ISO-8601 UTC strings with 9 fractional digits** (nanosecond precision).
- Aligning column schemas directly with canonical OpenTelemetry ClickHouse exporter templates (`logs_json_table.sql`, `traces_json_table.sql`, `metrics_*_table.sql`).

---

## Architecture and Data Flow

The diagram below illustrates the end-to-end data lifecycle from input source through `vector_otel.so` to Vector sinks:

```
┌──────────────────────────────────────────────────────────────────────────┐
│                           Vector Engine (Rust)                           │
│                                                                          │
│  [Source: opentelemetry (gRPC 4317 / HTTP 4318, use_otlp_decoding)]      │
│         │                                                                │
│         ▼ (Lua table in event.log.resourceLogs or raw OTLP JSON string)  │
│  [Transform: lua (v2, Lua 5.4)]                                          │
│         │                                                                │
│         │──> hooks.process(event, emit)                                  │
│         │         │                                                      │
│         │         ▼ require("vector_otel").logs.process(payload, emit)   │
│         │   ┌────────────────────────────────────────────────────────┐   │
│         │   │ vector_otel.so (C++20 Shared Module - Dual Mode)       │   │
│         │   │                                                        │   │
│         │   │  1. Ingestion Dispatcher                               │   │
│         │   │     • String: simdjson::ondemand SIMD vector parser    │   │
│         │   │     • Table: In-place Lua table traversal              │   │
│         │   │                                                        │   │
│         │   │  2. OtlpCommon & Schema Flatteners                     │   │
│         │   │     • Resource -> Scope -> Record hierarchy            │   │
│         │   │     • Injects service.name fallback ("unknown_service")│   │
│         │   │     • Exact 64-bit nanosecond ISO-8601 UTC timestamps  │   │
│         │   │     • FastJsonWriter for structured AnyValues / maps   │   │
│         │   │     • Generates ISO-8601 processed_at timestamp        │   │
│         │   │                                                        │   │
│         │   │  3. LuaBridge Emitter (N = 1 default, N > 1 batching)  │   │
│         │   │     • N = 1: pushes { log = { <column> = <val>, ... } }│   │
│         │   │     • N > 1: pushes { log = { records = [ ... ] } }    │   │
│         │   │     • Invokes lua_pcall(emit, 1, 0) per record/batch   │   │
│         │   └────────────────────────────────────────────────────────┘   │
│         │         │                                                      │
│         │<────────┘ returns (emitted_count, nil)                         │
│         │                                                                │
│  [Sink: aws_s3 (Parquet batch encoding) / clickhouse / kafka]            │
└──────────────────────────────────────────────────────────────────────────┘
```

---

## Performance and Benchmarks

Benchmarked on Linux x86-64 (16 cores) using Vector 0.59.0 and a 100,000-record OTLP logs fixture across batch sizes ($N \in \{1, 16, 128\}$), measuring both processing throughput and process memory footprints (Peak Max RSS and Minor Page Faults via `/usr/bin/time -v`):

### Table 1: Throughput and Speedup Comparison

| Batch Size ($N$) | Records | Fixture Size | VRL Baseline (rec/s) | Lua Flat $N=1$ (rec/s) | Speedup $N=1$ | Lua Matched (rec/s) | Speedup Matched |
|:---|:---|:---|:---|:---|:---|:---|:---|
| **1** | 100,000 | 131.7 MB | 111,345 | 54,280 | 0.49x | 56,186 | 0.50x |
| **16** | 100,000 | 60.1 MB | 81,208 | 81,779 | 1.01x | 81,286 | 1.00x |
| **128** | 100,096 | 56.0 MB | 16,750 | 82,051 | **4.90x** | 82,299 | **4.91x** |

### Table 2: Memory Utilisation & Allocation Footprint

| Batch Size ($N$) | VRL Max RSS (MB) | Lua $N=1$ RSS (MB) | Lua Matched RSS (MB) | Memory Reduction | VRL Page Faults | Lua $N=1$ Faults | Lua Matched Faults | Faults Reduction |
|:---|:---|:---|:---|:---|:---|:---|:---|:---|
| **1** | 452.56 MB | 181.94 MB | 188.64 MB | **58% lower** | 109,750 | 40,071 | 41,718 | **62% fewer** |
| **16** | 686.86 MB | 158.17 MB | 164.49 MB | **76% lower** | 177,852 | 33,760 | 35,080 | **80% fewer** |
| **128** | 673.25 MB | 147.38 MB | 190.75 MB | **72% – 78% lower** | 170,076 | 30,570 | 42,194 | **75% – 82% fewer** |

### Key Takeaways:
- **Production Batch Size ($N = 128$)**: VRL performance collapses to **16,750 rec/s** due to nested closure allocations and heavy heap overhead. `vector_otel.so` processes **82,299 rec/s** (46.0 MB/s) — a **4.91x throughput speedup**.
- **Memory Footprint Reduction (72% – 78% lower RSS)**: At production batch sizes ($N=16, 128$), Vector VRL consumes **673 – 687 MB** of peak resident memory due to deep AST and intermediate object allocations. In contrast, `vector_otel.so` operates in bounded streaming memory, consuming only **147 – 191 MB** peak RSS.
- **Dramatic Allocation Overhead Reduction (75% – 82% fewer page faults)**: VRL triggers over **170,000 minor page faults** during a 100k-record run, whereas `vector_otel.so` triggers only **30,570 – 42,194** minor page faults due to thread-local scratch buffer recycling and $O(1)$ streaming extraction.
- **Stable Scaling**: While VRL experiences a 6.6x slowdown when scaling from $N=1$ to $N=128$, `vector_otel.so` delivers consistently high performance (~82,000 rec/s) regardless of batch size.

---

## Prerequisites and Toolchain

| Component | Minimum Version | Description |
|:---|:---|:---|
| **C++ Compiler** | GCC 10+ / Clang 11+ | Must support `-std=c++20` |
| **CMake** | 3.20+ | Build configuration |
| **Lua** | 5.4 (`liblua5.4-dev`) | Header files for Lua 5.4 C API |
| **simdjson** | 5.0.5 | Vendored under `third_party/` (no download, no manual install) |
| **Vector** | 0.38.0+ (0.59.0+ tested) | DataDog Vector with Lua v2 transform support |
| **LuaRocks** | 3.0+ *(optional)* | For rockspec-based automated package installations |

On Debian/Ubuntu systems:
```bash
sudo apt-get update && sudo apt-get install -y \
    build-essential \
    cmake \
    liblua5.4-dev \
    lua5.4
```

---

## Build and Installation

### Signed Release Tarball (Recommended for Production)

Each tagged release on `main` publishes a deterministic source archive (`vector-lua-otel-<version>.tar.gz`) with vendored dependencies included, `SHA256SUMS`, keyless [Sigstore Cosign](https://docs.sigstore.dev/) bundles (`.sigstore.json`), and GitHub SLSA Build Provenance Attestations.

```bash
VERSION="0.1.0"
BASE_URL="https://github.com/jimmystewpot/vector-lua-otel/releases/download/v${VERSION}"

# 1. Download release archive, checksums, and Sigstore bundles
curl -sSLO "${BASE_URL}/vector-lua-otel-${VERSION}.tar.gz"
curl -sSLO "${BASE_URL}/vector-lua-otel-${VERSION}.tar.gz.sigstore.json"
curl -sSLO "${BASE_URL}/vector-otel-${VERSION}-1.rockspec"
curl -sSLO "${BASE_URL}/SHA256SUMS"
curl -sSLO "${BASE_URL}/SHA256SUMS.sigstore.json"

# 2. Verify SHA-256 checksums
sha256sum -c SHA256SUMS

# 3. Verify keyless Sigstore Cosign signature (optional, requires cosign)
cosign verify-blob \
  --bundle "vector-lua-otel-${VERSION}.tar.gz.sigstore.json" \
  --certificate-identity "https://github.com/jimmystewpot/vector-lua-otel/.github/workflows/release.yml@refs/tags/v${VERSION}" \
  --certificate-oidc-issuer "https://token.actions.githubusercontent.com" \
  "vector-lua-otel-${VERSION}.tar.gz"

# 4. Verify GitHub SLSA build provenance (optional, requires gh CLI)
gh attestation verify "vector-lua-otel-${VERSION}.tar.gz" --repo jimmystewpot/vector-lua-otel

# 5. Extract and build (no network access required)
tar -xzf "vector-lua-otel-${VERSION}.tar.gz"
cd "vector-lua-otel-${VERSION}"
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

### CMake Build

Clone the repository and build using CMake:

```bash
git clone https://github.com/jimmystewpot/vector-lua-otel.git
cd vector-lua-otel

# Configure and compile in Release mode
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Run test suite
ctest --test-dir build --output-on-failure
```

The compiled shared library will be placed at:
```
build/vector_otel.so
```

> **Note on Symbol Resolution:** `vector_otel.so` is built as a CMake `MODULE` library and does not link `liblua5.4.so`. When loaded inside Vector, all Lua C API symbols (`lua_*`, `luaL_*`) are resolved dynamically from the host Vector executable. Standalone tests (`vector_otel_tests`) link `liblua5.4.so` for standalone execution.

### Offline Builds

The repository vendors third-party dependencies (`simdjson` 5.0.5 and `googletest` 1.14.0) under `third_party/`, enabling offline builds on Ubuntu 22.04 (CMake 3.22, GCC 11) without any network access after cloning.

**Requirements for offline builds:**
- C++ Compiler (GCC 10+ or Clang 11+)
- CMake ≥ 3.20
- `liblua5.4-dev` and `lua5.4`
- Docker (for verification)

**Verify offline build capability** (requires Docker):
```bash
bash scripts/verify_offline_build.sh
```
This script builds and tests in an isolated Ubuntu 22.04 container with no network access. Expected output: `OFFLINE BUILD OK`.

For custom Docker mount paths (e.g., on Docker Desktop):
```bash
OFFLINE_TMPDIR=/path/to/mount bash scripts/verify_offline_build.sh
```

**Check vendored integrity** (offline, no network required):
```bash
bash scripts/vendor_deps.sh --verify
```

**Update a dependency** (e.g., simdjson to a newer version):
```bash
bash scripts/vendor_deps.sh simdjson v5.1.0
git add third_party/simdjson
git commit -m "build: vendor simdjson 5.1.0"
```

See [`third_party/README.md`](third_party/README.md) for more details on vendored components and updating procedures.

**Vendored components and licenses:**
- **simdjson** v5.0.5: Apache License 2.0 / MIT (dual-licensed)
- **googletest** v1.14.0: BSD 3-Clause License

### LuaRocks Installation

You can install `vector-otel` directly via LuaRocks using the provided rockspec:

```bash
luarocks make vector-otel-scm-1.rockspec
```

### Build Options

Pass these options to `cmake -B build`:

| Flag | Default | Description |
|:---|:---|:---|
| `-DCMAKE_BUILD_TYPE` | `Release` | Build mode (`Release`, `Debug`, `RelWithDebInfo`) |
| `-DENABLE_NATIVE_TUNING=ON` | `OFF` | Enables `-march=native` compiler optimisations |
| `-DVECTOR_OTEL_EMIT_BATCH_SIZE=N`| `1` | Compile-time emit batch size ($N \ge 1$) |

---

## Vector Configuration

Vector's Lua transform version 2 (`version = "2"`) executes in a persistent Lua state. `package.cpath` and module loading **must** occur in the top-level `source:` script, outside the per-event `process()` hook.

`vector_otel.so` supports **dual-mode ingestion**:
- **Decoded Lua Table Mode**: Vector's industry-standard [`opentelemetry` source](https://vector.dev/docs/reference/configuration/sources/opentelemetry/) with [`use_otlp_decoding: true`](https://vector.dev/docs/reference/configuration/sources/opentelemetry/#use_otlp_decoding) parses incoming Protobuf/gRPC or HTTP payloads into native Lua tables (`event.log.resourceLogs`, `resourceSpans`, `resourceMetrics`). `vector_otel.so` traverses these tables directly in C++ with zero intermediate JSON serialisation.
- **Raw JSON String Mode**: When receiving raw JSON / NDJSON strings via HTTP server, Kafka, or stdin (`event.log.message`), `vector_otel.so` executes SIMD-accelerated on-demand parsing via `simdjson`.

### YAML Configuration (Industry Standard: OTLP Source -> Lua Transform -> S3 Parquet Sink)

```yaml
sources:
  otel_in:
    type: opentelemetry
    http:
      address: "0.0.0.0:4318"
    grpc:
      address: "0.0.0.0:4317"
    use_otlp_decoding:
      logs: true
      metrics: true

transforms:
  flatten_otel_logs:
    type: lua
    version: "2"
    inputs:
      - otel_in.logs
    source: |
      -- Prepend path to vector_otel.so
      package.cpath = "/usr/local/lib/lua/5.4/?.so;/opt/vector/lib/?.so;" .. package.cpath

      -- Require the extractor submodule once during initialisation
      local otel_logs = require("vector_otel").logs

      function process(event, emit)
        -- Dual-mode selector: accepts decoded OTLP table or fallback raw string
        local payload = event.log.resourceLogs or event.log.message or event.log
        if not payload then
          return
        end

        -- Flatten batch and stream individual ClickHouse/Parquet-aligned rows
        local count, err = otel_logs.process(payload, emit)
        if not count then
          -- Raising an error triggers Vector's component_errors_total metric
          error("OTLP logs extraction failed: " .. tostring(err))
        end
      end
    hooks:
      process: "process"

sinks:
  s3_parquet_logs:
    type: aws_s3
    inputs:
      - flatten_otel_logs
    bucket: telemetry-warehouse
    region: us-east-1
    key_prefix: logs/year=%Y/month=%m/day=%d/
    compression: none
    encoding:
      codec: text
    batch_encoding:
      codec: parquet
      schema_file: schemas/parquet/logs.schema
      compression:
        algorithm: zstd
        level: 3
    buffer:
      type: memory
      max_events: 10000
    batch:
      max_bytes: 10485760
      timeout_secs: 30
```

> **Parquet Schema Definitions:** Production Parquet schema definitions aligned with OpenTelemetry ClickHouse schemas are located in [`schemas/parquet/`](schemas/parquet/):
> - [`logs.schema`](schemas/parquet/logs.schema) — Canonical log records with exact 64-bit nanosecond timestamps
> - [`traces.schema`](schemas/parquet/traces.schema) — Distributed trace spans and parent-child hierarchy
> - [`metrics.schema`](schemas/parquet/metrics.schema) — Gauges, sums, histograms, exponential histograms, summaries, and exemplars
>
> Sinks such as ClickHouse (`type = "clickhouse"`) and Kafka (`type = "kafka"`) are also fully supported.

### TOML Configuration

```toml
[sources.otel_in]
type = "opentelemetry"
use_otlp_decoding.logs = true
use_otlp_decoding.metrics = true

[sources.otel_in.http]
address = "0.0.0.0:4318"

[sources.otel_in.grpc]
address = "0.0.0.0:4317"

[transforms.flatten_otel_logs]
type = "lua"
inputs = ["otel_in.logs"]
version = "2"

source = '''
package.cpath = "/usr/local/lib/lua/5.4/?.so;/opt/vector/lib/?.so;" .. package.cpath
local otel_logs = require("vector_otel").logs

function process(event, emit)
    local payload = event.log.resourceLogs or event.log.message or event.log
    if not payload then
        return
    end

    local count, err = otel_logs.process(payload, emit)
    if not count then
        error("OTLP logs extraction failed: " .. tostring(err))
    end
end
'''

hooks.process = "process"

[sinks.s3_parquet_logs]
type = "aws_s3"
inputs = ["flatten_otel_logs"]
bucket = "telemetry-warehouse"
region = "us-east-1"
key_prefix = "logs/year=%Y/month=%m/day=%d/"
compression = "none"

[sinks.s3_parquet_logs.encoding]
codec = "text"

[sinks.s3_parquet_logs.batch_encoding]
codec = "parquet"
schema_file = "schemas/parquet/logs.schema"

[sinks.s3_parquet_logs.batch_encoding.compression]
algorithm = "zstd"
level = 3
```

### Configuration Invariants

1. **Top-Level `source:` Initialisation**:
   - `package.cpath` modifications and `require("vector_otel")` calls must happen **once** in the top-level script block.
   - **Never** call `require()` or alter `package.cpath` inside `hooks.process`. Doing so causes severe CPU overhead and memory leaks.
2. **Error Handling (`if not count then`)**:
   - Always check `if not count then`.
   - In Lua, `0` is truthy (`not 0 == false`). A batch that safely emits 0 records returns `(0, nil)`, which evaluates as success.
   - On error, the function returns `(nil, err_string, count_before_failure)`. Checking `if not count then` correctly detects failure.

---

## Lua Module API Reference

### Module Structure

When required via `require("vector_otel")`, the module exposes:

```lua
local vector_otel = require("vector_otel")

vector_otel._VERSION   -- "0.1.0"
vector_otel.logs      -- Logs extractor submodule
vector_otel.traces    -- Traces extractor submodule
vector_otel.metrics   -- Metrics extractor submodule
```

Submodules are also registered in `package.loaded`, allowing direct imports:
```lua
local otel_logs    = require("vector_otel.logs")
local otel_traces  = require("vector_otel.traces")
local otel_metrics = require("vector_otel.metrics")
```

### Function Signatures

All three submodules export an identical dual-mode `process` signature:

```lua
otel_logs.process(payload: string | table, emit: function) -> (integer, nil) | (nil, string, integer)
otel_traces.process(payload: string | table, emit: function) -> (integer, nil) | (nil, string, integer)
otel_metrics.process(payload: string | table, emit: function) -> (integer, nil) | (nil, string, integer)
```

- **`payload`** (`string | table`):
  - **`table`**: Decoded OTLP Lua table (e.g. `event.log.resourceLogs` from Vector's `use_otlp_decoding: true`).
  - **`string`**: Raw OTLP JSON string (e.g. `ExportLogsServiceRequest` from HTTP or Kafka), parsed via `simdjson`.
- **`emit`** (`function`): Vector callback invoked for each emitted record: `emit(event)`.

### Return and Error Contract

- **Success**: Returns `(emitted_count, nil)`
  ```lua
  local count, err = otel_logs.process(payload, emit)
  -- count == 128, err == nil
  ```
- **Failure**: Returns `(nil, error_str, emitted_count_before_failure)`
  ```lua
  local count, err, partial_count = otel_logs.process(invalid_payload, emit)
  -- count == nil, err == "error message", partial_count == 0
  ```
- **Partial counts**: `emitted_count` / `partial_count` is the number of rows whose `emit` call returned successfully. Rows already passed to `emit` cannot be recalled, so a failed call is **not atomic**: log `partial_count` if you need to know how much was delivered. With `VECTOR_OTEL_EMIT_BATCH_SIZE > 1`, rows still queued when a payload fails to parse are discarded and are not counted.
- **Errors raised by `emit`**: `emit` is invoked under `lua_pcall`; an error in your callback stops the call and is returned as `(nil, message, partial_count)`.
- **Acknowledgements in Vector**: events that a Lua transform builds from Lua tables are new events. Per Vector's Lua transform implementation (not verified against every version), they do not carry the input event's source acknowledgement finalizers, so end-to-end acknowledgements do not follow emitted rows past the Lua transform. Treat this module as best-effort with respect to delivery guarantees.
- **Empty / Sparse Payloads**: Payloads with empty records or missing scopes safely return `(0, nil)` without errors.
- **FFI Safety**: C++ exceptions are caught at the FFI boundary and converted into error return values. No unhandled C++ exceptions or Lua panics escape.

### Emitted Tables Are Shared

Within one resource, every emitted record's `ResourceAttributes` points at the **same** Lua table, and likewise `ScopeAttributes` within one scope. This avoids rebuilding the table for every record. Treat both as **read-only**: copy a table before mutating it, because a change made while handling one event is visible to every other event of the same resource/scope (including events your `emit` callback has already stored).

---

## Telemetry Schema Mappings

All extracted columns are aligned with canonical OpenTelemetry ClickHouse exporter SQL schemas and production [Parquet Schema Definitions](schemas/parquet/README.md).

### Logs Schema
*Aligned with ClickHouse `logs_json_table.sql` and Parquet [`schemas/parquet/logs.schema`](schemas/parquet/logs.schema):*

| Column Name | Type | OTLP Field Source | Description |
|:---|:---|:---|:---|
| `Timestamp` | `string` | `timeUnixNano` / `observedTimeUnixNano` | ISO-8601 UTC string with 9 fractional digits |
| `TraceId` | `string` | `traceId` | W3C 16-byte hex string (empty if unset) |
| `SpanId` | `string` | `spanId` | W3C 8-byte hex string (empty if unset) |
| `TraceFlags` | `integer` | `flags` | Unsigned 8-bit integer |
| `SeverityText` | `string` | `severityText` | Severity string (`"INFO"`, `"ERROR"`, etc.) |
| `SeverityNumber` | `integer` | `severityNumber` | Numeric severity (1–24) |
| `ServiceName` | `string` | `resource.attributes["service.name"]` | Service name (defaults to `"unknown_service"`) |
| `Body` | `string` | `body` | String body or stringified JSON if structured |
| `ResourceSchemaUrl` | `string` | `resourceLogs.schemaUrl` | Resource schema URL |
| `ResourceAttributes` | `table` | `resource.attributes` | Key-value table of resource attributes |
| `ScopeSchemaUrl` | `string` | `scopeLogs.schemaUrl` | Scope schema URL |
| `ScopeName` | `string` | `scope.name` | Instrumentation scope name |
| `ScopeVersion` | `string` | `scope.version` | Instrumentation scope version |
| `ScopeAttributes` | `table` | `scope.attributes` | Key-value table of scope attributes |
| `LogAttributes` | `table` | `logRecord.attributes` | Key-value table of record attributes |
| `EventName` | `string` | `eventName` / attributes | Event name (empty if unset) |
| `processed_at` | `string` | `std::chrono::system_clock` | Transform ISO-8601 UTC nanosecond timestamp |

### Traces Schema
*Aligned with ClickHouse `traces_json_table.sql` and Parquet [`schemas/parquet/traces.schema`](schemas/parquet/traces.schema):*

| Column Name | Type | OTLP Field Source | Description |
|:---|:---|:---|:---|
| `Timestamp` | `string` | `startTimeUnixNano` | ISO-8601 UTC string with 9 fractional digits |
| `TraceId` | `string` | `traceId` | W3C 16-byte hex string |
| `SpanId` | `string` | `spanId` | W3C 8-byte hex string |
| `ParentSpanId` | `string` | `parentSpanId` | Parent span hex (empty if root) |
| `TraceState` | `string` | `traceState` | W3C trace state string |
| `SpanName` | `string` | `name` | Operation / span name |
| `SpanKind` | `string` | `kind` | Span kind (`"SPAN_KIND_SERVER"`, etc.) |
| `ServiceName` | `string` | `resource.attributes["service.name"]` | Service name (defaults to `"unknown_service"`) |
| `ResourceAttributes` | `table` | `resource.attributes` | Resource key-value attributes |
| `ScopeName` | `string` | `scope.name` | Instrumentation scope name |
| `ScopeVersion` | `string` | `scope.version` | Instrumentation scope version |
| `SpanAttributes` | `table` | `attributes` | Span key-value attributes |
| `Duration` | `integer` | `endTimeUnixNano - startTimeUnixNano` | Span duration in nanoseconds (preserved 64-bit int) |
| `StatusCode` | `string` | `status.code` | `"STATUS_CODE_UNSET"`, `"OK"`, or `"ERROR"` |
| `StatusMessage` | `string` | `status.message` | Status description message |
| `Events` | `table` | `events` | Array of `{ Timestamp, Name, Attributes }` |
| `Links` | `table` | `links` | Array of `{ TraceId, SpanId, TraceState, Attributes }` |
| `processed_at` | `string` | `std::chrono::system_clock` | Transform ISO-8601 UTC nanosecond timestamp |

### Metrics Schema
*Aligned with ClickHouse `metrics_*_table.sql` and Parquet [`schemas/parquet/metrics.schema`](schemas/parquet/metrics.schema):*

| Column Name | Type | OTLP Field Source | Description |
|:---|:---|:---|:---|
| `MetricName` | `string` | `metric.name` | Metric name |
| `MetricDescription` | `string` | `metric.description` | Metric description |
| `MetricUnit` | `string` | `metric.unit` | Metric unit of measurement |
| `MetricType` | `string` | Data point type | Discriminator: `"gauge"`, `"sum"`, `"histogram"`, `"exponential_histogram"`, `"summary"` |
| `ServiceName` | `string` | `resource.attributes["service.name"]` | Service name (defaults to `"unknown_service"`) |
| `AggregationTemporality` | `integer` | `aggregationTemporality` | Aggregation temporality (`1` = DELTA, `2` = CUMULATIVE, `0` = UNSPECIFIED) for Sum / Histogram / ExponentialHistogram |
| `IsMonotonic` | `boolean` | `sum.isMonotonic` | Monotonicity flag emitted for `"sum"` metrics when present |
| `ResourceAttributes` | `table` | `resource.attributes` | Resource key-value attributes |
| `ScopeName` | `string` | `scope.name` | Instrumentation scope name |
| `ScopeVersion` | `string` | `scope.version` | Instrumentation scope version |
| `ScopeAttributes` | `table` | `scope.attributes` | Scope key-value attributes |
| `Attributes` | `table` | `dataPoint.attributes` | Data point label dimensions |
| `StartTimeUnix` | `string` | `dataPoint.startTimeUnixNano` | Metric start time (ISO-8601 UTC string) |
| `TimeUnix` | `string` | `dataPoint.timeUnixNano` | Data point time (ISO-8601 UTC string) |
| `Value` | `number` | `asDouble` / `asInt` / `value` | Metric value for Gauge / Sum (omitted / `nil` for Histogram, ExponentialHistogram, and Summary) |
| `Histogram` | `table` | `dataPoint` (Histogram) | `{ aggregationTemporality, count, sum, min, max, bucketCounts, explicitBounds }` |
| `ExponentialHistogram` | `table` | `dataPoint` (ExponentialHistogram) | `{ aggregationTemporality, count, sum, min, max, scale, zeroCount, zeroThreshold, positive = { offset, bucketCounts }, negative = { offset, bucketCounts } }` |
| `Summary` | `table` | `dataPoint` (Summary) | `{ count, sum, quantileValues = [ { quantile, value }, ... ] }` |
| `Flags` | `integer` | `flags` | Unsigned 32-bit flags |
| `Exemplars` | `table` | `exemplars` | Array of `{ FilteredAttributes, TimeUnix, Value, SpanId, TraceId }` |
| `processed_at` | `string` | `std::chrono::system_clock` | Transform ISO-8601 UTC nanosecond timestamp |

---

## Testing and Quality Assurance

The project enforces strict automated verification protocols across multiple test harnesses:

1. **C++ GoogleTest Suite** (`ctest`):
   ```bash
   ctest --test-dir build --output-on-failure
   ```
   Validates timestamp ISO-8601 nanosecond formatting, `simdjson` padding safety, `FastJsonWriter` escaping, int64 preservation, and extractor schema adherence.

2. **Standalone Lua Integration & GC Stability** (`tests/lua/run_tests.sh`):
   ```bash
   bash tests/lua/run_tests.sh
   ```
   Tests module exports, error contracts, ClickHouse schemas against golden fixtures, and executes a **50,000-event stress loop** asserting $0.00\text{ KB}$ memory delta across runs.

3. **End-to-End Vector Pipeline Validation**:
   ```bash
   vector validate --no-environment tests/vector/vector_test.yaml
   ```
   Validates syntax and schema compatibility against official Vector release binaries.

4. **Comparative Benchmark Matrix** (`bench/compare.sh`):
   ```bash
   bash bench/compare.sh
   ```
   Generates comparative throughput benchmarks against baseline VRL transforms across batch sizes $N \in \{1, 16, 128\}$.

---

## License

`vector-lua-otel` is licensed under the [Apache License, Version 2.0](LICENSE).
