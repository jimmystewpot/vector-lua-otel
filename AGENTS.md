# AGENTS.md — Developer & AI Agent Guidelines

This document provides architectural rules, code invariants, formatting requirements, and verification protocols for AI coding agents and human engineers contributing to `vector-lua-otel`.

---

## 1. Project Mission & Core Architecture

`vector-lua-otel` is a high-performance C++20 dynamic library suite exposing two native Lua 5.4 module interfaces for DataDog Vector's Lua transforms:
- `vector_otel.so` (`require("vector_otel")`): Processes raw OpenTelemetry (OTLP) JSON batch payloads (`ExportLogsServiceRequest`, `ExportTraceServiceRequest`, `ExportMetricsServiceRequest`) or OTLP-decoded Lua tables (`use_otlp_decoding: true`).
- `vector_native.so` (`require("vector_native")`): Processes Vector's native internal Log, Trace (via `trace_to_log` with `log_namespace: false`), and Metric event models (`use_otlp_decoding: false` or JSON-encoded native events).

Both modules flatten records into identical ClickHouse/Parquet-aligned table columns and stream extracted events directly into Vector's pipeline via the `emit` callback.

### Non-Negotiable Architectural Invariants

Every modification to this repository **must** strictly adhere to the following invariants:

1. **$O(1)$ Streaming Memory Model**:
   - **Never buffer entire batches** in C++ memory or Lua tables before emitting.
   - For $N = 1$ (default), extract and emit each record immediately via `lua_pcall(emit, 1, 0)`.
   - For $N > 1$, accumulate only up to $N$ rows in a pre-allocated reusable buffer before flushing.
   - Memory usage must remain bounded and independent of batch size ($M$).

2. **Zero Dynamic Heap Allocations in Hot Extraction Loops**:
   - Do **not** allocate `std::string`, `std::vector`, or raw heap memory (`new` / `malloc`) inside per-record extraction loops.
   - Reuse `thread_local` scratch buffers (`FastJsonWriter`, parser buffers, scratch strings) with pre-reserved capacities.
   - Call `.clear()` rather than deallocating or reallocating buffers between records.

3. **Non-Throwing C API / FFI Boundary Contract**:
   - **Never allow C++ exceptions or Lua panics to escape** across the C API boundary.
   - All `lua_CFunction` entry points go through `DispatchProcess` (`include/vector_otel/core/process_runner.hpp`), which runs the whole extraction inside one protected region (C++ `try ... catch` plus `lua_pcall`-protected Lua operations) and converts every failure — allocation failure, Lua error, callback error, oversized input — into the error return below. No raising Lua API call may run outside a protected frame.
   - **Success return**: `(integer count, nil)` (pushes count, then `nil`).
   - **Failure return**: `(nil, string error_message, integer partial_count)` (pushes `nil`, error string, then partial count).
   - **Why `nil` first?** In Lua, `0` is truthy (`not 0 == false`). A return of `0` on error would bypass `if not count then` guards. Returning `nil` ensures Lua error checks work correctly.
   - Empty or sparse payloads (`resourceLogs: []`, missing scopes, `{}` or `{ log = {} }`, 0 records) are **not** errors; they must safely return `(0, nil)`.

4. **Exact 64-bit Integer & Nanosecond Timestamp Preservation**:
   - Never cast string-encoded 64-bit numbers (`timeUnixNano`, `intValue`, `duration`) to standard `double` (which truncates at 53 bits).
   - Use native Lua 5.4 64-bit integers (`lua_pushinteger`) for integer columns.
   - Format all timestamps as ISO-8601 UTC strings with exactly 9 fractional digits (nanoseconds), e.g. `2026-10-16T12:00:00.123456789Z`.

5. **Shared Module Symbol Resolution (`MODULE` vs `EXECUTABLE`)**:
   - The shared libraries `vector_otel.so` and `vector_native.so` (`MODULE` libraries) **MUST NOT link `liblua` or `-llua5.4`**.
   - All Lua C API symbols (`lua_*`, `luaL_*`) must resolve dynamically from the host Vector executable at runtime.
   - Standalone unit test binaries (`vector_otel_tests`) link `${LUA_LIBRARIES}` to enable standalone execution without Vector.

6. **Buffer Over-read Safety (`SIMDJSON_PADDING`)**:
   - `simdjson::ondemand` requires 64 bytes (`simdjson::SIMDJSON_PADDING`) beyond the input string to prevent buffer over-reads during SIMD vector loads.
   - Input strings passed from Lua (`lua_tolstring`) must be inspected and safely copied to a padded `thread_local` buffer if not already padded.

7. **Top-Level `source:` Initialisation in Vector Transforms**:
   - Modifying `package.cpath` and calling `require("vector_otel")` or `require("vector_native")` must occur strictly in the top-level `source:` block.
   - **Never** call `require()` or alter `package.cpath` inside `hooks.process`. This would trigger repetitive module table initialisation and cause memory leaks.

8. **No Network Access in Build or Test**:
   - **No build or test step may require network access.** All third-party dependencies are vendored under `third_party/` and must only be changed via `scripts/vendor_deps.sh`.
   - Do not hand-edit or manually patch vendored files; use `scripts/vendor_deps.sh` to refresh a dependency.
   - Changes must pass `bash scripts/vendor_deps.sh --verify` (verify checksums match).
   - Do not reintroduce `FetchContent_Declare`, `FetchContent_MakeAvailable`, or `ExternalProject` commands that fetch from the network.

9. **Vector Native Event Invariants (`vector_native.so`)**:
   - **Traces via `trace_to_log` (`log_namespace: false`)**: Vector's Lua v2 transform only accepts `Log` and `Metric` events, so native `Trace` events from `opentelemetry` (`use_otlp_decoding: false`) must pass through `trace_to_log` (`log_namespace: false`) before `vector_native.traces.process`.
   - **Timestamp Fallback to `processed_at` (`now()`)**: When primary and secondary timestamps are absent in native events, `Timestamp` (or `TimeUnix` for metrics) falls back to the record's `processed_at` (`now()`) timestamp.
   - **Non-OTLP Metric Variant Exclusion**: Vector-only metric variants (`distribution`, `set`, `sketch`) are intentionally unsupported in `vector_native.metrics.process` and must return `(nil, "Unsupported non-OTLP metric variant '<variant>': vector_native only supports OpenTelemetry metric types (counter, gauge, aggregated_histogram, aggregated_summary)", partial_count)`.

---

## 2. Coding & Formatting Standards

### C++ Code Guidelines

- **Standard**: C++20 (`-std=c++20`).
- **Style Guide**: Strictly follow Google C++ Style Guide with a 100-character column limit.
- **Formatting Command**:
  ```bash
  clang-format -i include/vector_otel/**/*.hpp src/**/*.cpp tests/cpp/**/*.cpp
  ```
- **Naming Conventions**:
  - Types / Classes / Structs: `PascalCase` (e.g. `FastJsonWriter`, `LogsExtractor`)
  - Methods / Functions: `CamelCase` or `snake_case` matching existing module conventions
  - Member variables: `trailing_underscore_` (e.g. `buffer_`, `capacity_`)
  - Local variables: `snake_case` (e.g. `raw_payload`, `emitted_count`)
  - Constants / Macros: `ALL_CAPS` (e.g. `VECTOR_OTEL_EMIT_BATCH_SIZE`)
- **Headers & Includes**:
  - Prefer project includes in the order: own header, standard library headers, third-party libraries (`simdjson`, `lua.hpp`), internal headers.
  - All public headers reside in `include/vector_otel/`.

### Lua Code Guidelines

- Target Lua 5.4.
- 2-space indentation.
- Always use `local` variables; never pollute the global environment.
- Error checking pattern:
  ```lua
  local count, err = otel_logs.process(payload, emit)
  if not count then
    error("Extraction failed: " .. tostring(err))
  end
  ```

---

## 3. Directory Structure & Key Files

```
.
├── CMakeLists.txt                      # Top-level build configuration
├── cmake/
│   ├── simdjson.cmake                  # add_subdirectory(third_party/simdjson)
│   └── googletest.cmake                # add_subdirectory(third_party/googletest)
├── third_party/                        # Vendored dependencies (no network required)
│   ├── simdjson/                       # simdjson 5.0.5 with VENDOR.md
│   ├── googletest/                     # googletest 1.14.0 with VENDOR.md
│   └── README.md                       # Vendoring guide and license compliance
├── scripts/
│   ├── vendor_deps.sh                  # Fetch and vendor a dependency; verify checksums
│   ├── verify_offline_build.sh         # Test offline build in Docker container
│   └── offline/Dockerfile              # Ubuntu 22.04 verification environment
├── .gitattributes                      # Binary/text attributes for vendored trees
├── include/vector_otel/
│   ├── core/
│   │   ├── admission.hpp               # Input admission (2048-record / byte limits, preflight)
│   │   ├── fast_json_writer.hpp        # Allocation-free JSON serialiser
│   │   ├── module_runner.hpp           # Active-extraction guard & package.loaded helper
│   │   ├── module_state.hpp            # Per-Lua-state module anchor (registry slots)
│   │   ├── operation_context.hpp       # Per-call context: state, limits, error code
│   │   ├── otlp_common.hpp             # Shared AnyValue decoding & attributes
│   │   ├── parser.hpp                  # simdjson ondemand & padding manager
│   │   ├── process_runner.hpp          # DispatchProcess: the single protected entry path
│   │   ├── protected_lua.hpp           # lua_pcall-wrapped (non-raising) Lua operations
│   │   ├── read_keys.hpp               # Compile-time table-read keys
│   │   ├── row_emitter.hpp             # Row delivery to the emit callback (N=1 / N>1)
│   │   ├── scratch.hpp                 # Reusable thread_local scratch strings
│   │   ├── staged_row.hpp              # Allocation-free staged row -> Lua table materialiser
│   │   ├── table_reader.hpp            # Non-raising Lua table reads & native timestamp parser
│   │   └── timestamp.hpp               # ISO-8601 nanosecond UTC formatter & RFC-3339 parser
│   ├── extractors/
│   │   ├── logs_extractor.hpp          # OTLP logs -> ClickHouse columns
│   │   ├── traces_extractor.hpp        # OTLP traces -> ClickHouse columns
│   │   └── metrics_extractor.hpp       # OTLP metrics -> ClickHouse columns
│   └── native_extractors/
│       ├── native_logs_extractor.hpp   # Vector native logs -> ClickHouse columns
│       ├── native_traces_extractor.hpp # Vector native traces (trace_to_log) -> ClickHouse columns
│       └── native_metrics_extractor.hpp# Vector native metrics -> ClickHouse columns
├── src/
│   ├── core/                           # Core component implementations
│   ├── extractors/                     # OTLP extractor implementations
│   ├── native_extractors/              # Vector native extractor implementations
│   ├── module.cpp                      # luaopen_vector_otel & submodule bindings
│   └── native_module.cpp               # luaopen_vector_native & submodule bindings
├── tests/
│   ├── cpp/                            # GoogleTest C++ test suites (unit, fault, worker isolation)
│   ├── support/                        # Fault allocators, process fixtures, legacy reference oracle
│   ├── lua/                            # Standalone Lua 5.4 test suite & GC checks
│   │   ├── run_tests.sh                # Test runner script
│   │   ├── test_vector_otel.lua        # OTLP functional & 50k-loop leak tests
│   │   └── test_vector_native.lua      # Native event functional & 50k-loop leak tests
│   └── vector/
│       ├── vector_test.yaml            # Vector OTLP-decoded pipeline config
│       └── vector_native_test.yaml     # Vector native event + trace_to_log pipeline config
├── bench/                              # Benchmark harness & comparative scripts
│   ├── compare.sh                      # Automated VRL vs Lua comparison runner
│   ├── gen_fixtures.py                 # OTLP test fixture generator
│   └── run.sh                          # Single-configuration benchmark harness
├── README.md                           # Public user documentation & quickstart
└── AGENTS.md                           # Contributor & agent engineering invariants
```

---

## 4. Verification Protocols (Pre-Commit Checklist)

Before claiming any task complete or committing changes, run and verify all four levels of test validation:

### Step 1: C++ Unit Tests (CTest)
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```
**Expected**: 100% tests passing (`vector_otel_tests`).

### Step 2: Standalone Lua 5.4 Integration & GC Leak Check
```bash
bash tests/lua/run_tests.sh
```
**Expected**:
- Module loading passes for both `vector_otel` and `vector_native`.
- Error contracts and non-OTLP metric variant exclusions pass.
- Logs, Traces, and Metrics schemas validate against golden fixtures and schema parity assertions.
- 50,000-event stress loops complete with `Memory Delta: 0.00 KB` (`0 leaks`).

### Step 3: Vector Pipeline Validation
```bash
vector validate --no-environment tests/vector/vector_test.yaml tests/vector/vector_native_test.yaml
```
*(If `vector` is installed or available in `/tmp/vector-bench/`)*
**Expected**: `Validated` (exit code 0).

### Step 4: Formatting Verification
```bash
find include src tests/cpp -type f \( -name "*.h" -o -name "*.hpp" -o -name "*.c" -o -name "*.cpp" \) | xargs -r clang-format --dry-run --Werror
```
**Expected**: Clean format with 0 errors.
*(Note: `third_party/` is excluded from formatting as vendored code should not be modified.)*

### Step 5: Offline Build Verification (Optional)
```bash
bash scripts/verify_offline_build.sh
```
**Expected**: `OFFLINE BUILD OK` (requires Docker).
This step verifies that the build works with no network access in an isolated Ubuntu 22.04 container.

### Step 6: Sanitiser Run (Optional)
```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
cmake --build build-asan -j$(nproc)
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir build-asan --output-on-failure
```
**Expected**: 100% tests passing (`vector_otel_tests`) with 0 ASAN/UBSAN errors.
*(Note: `tests/lua/run_tests.sh` is not run under ASAN because loading an ASAN-instrumented `vector_otel.so` or `vector_native.so` into an uninstrumented system `lua5.4` binary requires `LD_PRELOAD` of the ASAN runtime.)*

---

## 5. Git Commit & PR Conventions

- Follow the [Conventional Commits](https://www.conventionalcommits.org/) specification:
  - `feat: ...` for new user-facing functionality
  - `fix: ...` for bug fixes
  - `test: ...` for adding or refactoring tests
  - `docs: ...` for documentation additions or corrections
  - `bench: ...` for benchmark harness updates
  - `refactor: ...` for code refactoring without behaviour changes
  - `build: ...` for CMake or dependency updates
- Keep commits focused and atomic.
- Superpowers artifacts in `.superpowers/` and plan notes in `docs/superpowers/` are local working artifacts and should never be committed to repository history unless explicitly instructed.
