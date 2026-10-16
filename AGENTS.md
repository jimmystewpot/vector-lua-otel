# AGENTS.md — Developer & AI Agent Guidelines

This document provides architectural rules, code invariants, formatting requirements, and verification protocols for AI coding agents and human engineers contributing to `vector-lua-otel`.

---

## 1. Project Mission & Core Architecture

`vector-lua-otel` is a high-performance C++20 dynamic library exposing a native Lua 5.4 module interface (`vector_otel.so`) for DataDog Vector's Lua transforms. It processes raw OpenTelemetry (OTLP) JSON batch payloads (`ExportLogsServiceRequest`, `ExportTraceServiceRequest`, `ExportMetricsServiceRequest`), flattens nested records into ClickHouse-aligned table columns, and streams extracted events directly into Vector's pipeline via the `emit` callback.

### Non-Negotiable Architectural Invariants

Every modification to this repository **must** strictly adhere to the following invariants:

1. **$O(1)$ Streaming Memory Model**:
   - **Never buffer entire batches** in C++ memory or Lua tables before emitting.
   - For $N = 1$ (default), extract and emit each record immediately via `lua_call(emit, 1, 0)`.
   - For $N > 1$, accumulate only up to $N$ rows in a pre-allocated reusable buffer before flushing.
   - Memory usage must remain bounded and independent of batch size ($M$).

2. **Zero Dynamic Heap Allocations in Hot Extraction Loops**:
   - Do **not** allocate `std::string`, `std::vector`, or raw heap memory (`new` / `malloc`) inside per-record extraction loops.
   - Reuse `thread_local` scratch buffers (`FastJsonWriter`, parser buffers, scratch strings) with pre-reserved capacities.
   - Call `.clear()` rather than deallocating or reallocating buffers between records.

3. **Non-Throwing C API / FFI Boundary Contract**:
   - **Never allow C++ exceptions or Lua panics to escape** across the C API boundary.
   - All `lua_CFunction` entry points (`l_logs_process`, `l_traces_process`, `l_metrics_process`) must wrap processing in `try ... catch` blocks.
   - **Success return**: `(integer count, nil)` (pushes count, then `nil`).
   - **Failure return**: `(nil, string error_message, integer partial_count)` (pushes `nil`, error string, then partial count).
   - **Why `nil` first?** In Lua, `0` is truthy (`not 0 == false`). A return of `0` on error would bypass `if not count then` guards. Returning `nil` ensures Lua error checks work correctly.
   - Empty or sparse payloads (`resourceLogs: []`, missing scopes, 0 records) are **not** errors; they must safely return `(0, nil)`.

4. **Exact 64-bit Integer & Nanosecond Timestamp Preservation**:
   - Never cast string-encoded 64-bit numbers (`timeUnixNano`, `intValue`, `duration`) to standard `double` (which truncates at 53 bits).
   - Use native Lua 5.4 64-bit integers (`lua_pushinteger`) for integer columns.
   - Format all timestamps as ISO-8601 UTC strings with exactly 9 fractional digits (nanoseconds), e.g. `2026-10-16T12:00:00.123456789Z`.

5. **Shared Module Symbol Resolution (`MODULE` vs `EXECUTABLE`)**:
   - The shared library `vector_otel.so` (`MODULE` library) **MUST NOT link `liblua` or `-llua5.4`**.
   - All Lua C API symbols (`lua_*`, `luaL_*`) must resolve dynamically from the host Vector executable at runtime.
   - Standalone unit test binaries (`vector_otel_tests`) link `${LUA_LIBRARIES}` to enable standalone execution without Vector.

6. **Buffer Over-read Safety (`SIMDJSON_PADDING`)**:
   - `simdjson::ondemand` requires 64 bytes (`simdjson::SIMDJSON_PADDING`) beyond the input string to prevent buffer over-reads during SIMD vector loads.
   - Input strings passed from Lua (`lua_tolstring`) must be inspected and safely copied to a padded `thread_local` buffer if not already padded.

7. **Top-Level `source:` Initialisation in Vector Transforms**:
   - Modifying `package.cpath` and calling `require("vector_otel")` must occur strictly in the top-level `source:` block.
   - **Never** call `require()` or alter `package.cpath` inside `hooks.process`. This would trigger repetitive module table initialisation and cause memory leaks.

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
│   ├── simdjson.cmake                  # FetchContent for simdjson 5.0.5
│   └── googletest.cmake                # FetchContent for GoogleTest
├── include/vector_otel/
│   ├── core/
│   │   ├── fast_json_writer.hpp        # Allocation-free JSON serialiser
│   │   ├── lua_bridge.hpp              # Lua stack management & batch emitter
│   │   ├── otlp_common.hpp             # Shared AnyValue decoding & attributes
│   │   ├── parser.hpp                  # simdjson ondemand & padding manager
│   │   └── timestamp.hpp               # ISO-8601 nanosecond UTC formatter
│   └── extractors/
│       ├── logs_extractor.hpp          # OTLP logs -> ClickHouse columns
│       ├── traces_extractor.hpp        # OTLP traces -> ClickHouse columns
│       └── metrics_extractor.hpp       # OTLP metrics -> ClickHouse columns
├── src/
│   ├── core/                           # Core component implementations
│   ├── extractors/                     # Extractor implementations
│   └── module.cpp                      # luaopen_vector_otel & submodule bindings
├── tests/
│   ├── cpp/                            # GoogleTest C++ test suite
│   ├── lua/                            # Standalone Lua 5.4 test suite & GC checks
│   │   ├── run_tests.sh                # Test runner script
│   │   └── test_vector_otel.lua        # Functional & 50k-loop leak tests
│   └── vector/
│       └── vector_test.yaml            # Vector end-to-end pipeline config
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
- Module loading passes.
- Error contracts pass.
- Logs, Traces, and Metrics schemas validate against golden fixtures.
- 50,000-event stress loop completes with `Memory Delta: 0.00 KB` (`0 leaks`).

### Step 3: Vector Pipeline Validation
```bash
vector validate --no-environment tests/vector/vector_test.yaml
```
*(If `vector` is installed or available in `/tmp/vector-bench/`)*
**Expected**: `Validated` (exit code 0).

### Step 4: Formatting Verification
```bash
clang-format --dry-run --Werror -style=Google \
    include/vector_otel/**/*.hpp \
    src/**/*.cpp \
    tests/cpp/**/*.cpp
```
**Expected**: Clean format with 0 errors.

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
