# Security Policy

`vector-lua-otel` is a high-performance C++20 native dynamic library engineered for DataDog Vector's Lua engine. Because this library operates as an in-process native shared object (`vector_otel.so`) handling untrusted network payloads (OpenTelemetry OTLP JSON), security, memory safety, and operational resilience are paramount.

This document details our security policy, supported releases, coordinated vulnerability disclosure procedures, threat model, and hardening guidelines.

---

## 1. Supported Versions

Security patches and vulnerability remediation are prioritised for the active development branch (`main`) and the latest tagged release.

| Version / Branch | Supported | Security Maintenance Status |
| :--- | :--- | :--- |
| `main` | :white_check_mark: Yes | Active development; all security patches merged here first |
| Latest Tagged Release | :white_check_mark: Yes | Supported with backported critical patches |
| Historical / Untagged Commits | :x: No | Unsupported; please upgrade to `main` or latest release |

---

## 2. Reporting a Vulnerability

We practice **Coordinated Vulnerability Disclosure (CVD)**. If you discover a security vulnerability, memory corruption flaw, or denial-of-service vector, please do **not** disclose it publicly via GitHub issues, pull requests, or public forums until it has been triaged, addressed, and patched.

### Preferred Method: GitHub Private Vulnerability Reporting

The most secure and direct reporting channel is GitHub's Private Vulnerability Reporting interface:
1. Navigate to the repository's **Security** tab: [https://github.com/jimmystewpot/vector-lua-otel/security](https://github.com/jimmystewpot/vector-lua-otel/security).
2. Click **Advisories** and select **Report a vulnerability**.
3. Complete the advisory form with comprehensive reproduction details.

### Alternative Method: Encrypted Email Disclosure

If you cannot utilise GitHub Private Vulnerability Reporting, you may submit your finding via GPG-encrypted email directly to the lead maintainer:

* **Contact**: James Lamb
* **Email**: [`admin@oranged.to`](mailto:admin@oranged.to)
* **GPG Key ID**: `6FC5AA978340401C`
* **GPG Fingerprint**: `B1C1 B81F 673D 8DC7 B38D  109C 6FC5 AA97 8340 401C`

You may retrieve the public key from standard keyservers (such as `keys.openpgp.org` or `keyserver.ubuntu.com`) or import it directly:
```bash
gpg --keyserver keys.openpgp.org --recv-keys B1C1B81F673D8DC7B38D109C6FC5AA978340401C
```

### Information to Include in Your Report

To facilitate rapid triage and validation, please provide:
1. **Description & Vulnerability Class**: (e.g. out-of-bounds read/write, buffer over-read, use-after-free, memory leak, unhandled C++ exception across FFI boundary, undefined behaviour).
2. **Component Affected**: Specific extractor (`logs_extractor`, `traces_extractor`, `metrics_extractor`) or core module (`parser`, `fast_json_writer`, `process_runner`, `table_reader`).
3. **Proof of Concept (PoC)**: Minimal JSON payload, Lua reproduction script, or unit test demonstrating the flaw.
4. **Environment**: Compiler (GCC/Clang), architecture (`x86_64`, `aarch64`), operating system, and Lua 5.4 version.
5. **Impact Assessment**: Evaluation of potential consequences (e.g. process crash/DoS, host Vector memory disclosure, remote code execution).
6. **Remediation Guidance**: If you have identified a potential fix, patch proposals are highly welcomed.

---

## 3. Vulnerability Response Lifecycle & SLAs

We commit to the following response timeline for all legitimate security reports:

| Phase | Target SLA | Description |
| :--- | :--- | :--- |
| **Initial Acknowledgement** | $\le$ 48 hours | Confirmation of receipt and assignment of a tracking reference |
| **Triage & Assessment** | $\le$ 5 business days | Reproduction, validation, and CVSS v3.1 / v4.0 severity scoring |
| **Fix Formulation & Review** | $\le$ 14 calendar days | Development of remediation patch in a private security fork |
| **Reporter Verification** | $\le$ 7 calendar days | Private sharing of candidate patch with reporter for validation |
| **Public Release & Advisory** | Coordinated | Release of patched version, publication of GitHub Security Advisory, and CVE publication |

Standard disclosure embargo is **90 days** from initial report, or sooner upon mutual agreement once patches are deployed.

---

## 4. Safe Harbour Policy

We consider security research undertaken in good faith to be an invaluable contribution to software safety. We will not pursue legal action or initiate law enforcement complaints against researchers who:
* Adhere to Coordinated Vulnerability Disclosure principles.
* Make a genuine effort to avoid privacy violations, data destruction, and interruption or degradation of production services.
* Do not access or modify customer, telemetry, or user data without explicit authorisation.
* Give us reasonable time to remedy the vulnerability prior to public disclosure.

---

## 5. Threat Model & Security Architecture

`vector-lua-otel` operates within a high-throughput data processing pipeline. Understanding our threat boundaries assists both security researchers and system operators.

```
                           [ Untrusted Ingestion Boundary ]
                                         │
                                         ▼
                      ┌──────────────────────────────────────┐
                      │    Vector Ingest Source (OTLP/HTTP)  │
                      └──────────────────┬───────────────────┘
                                         │ Raw JSON string
                                         ▼
                      ┌──────────────────────────────────────┐
                      │       Vector Lua 5.4 Engine          │
                      │  (Process address space of Vector)   │
                      └──────────────────┬───────────────────┘
                                         │ FFI / C API boundary
                                         ▼
┌────────────────────────────────────────────────────────────────────────────┐
│ vector_otel.so (Native C++20 Module)                                       │
│                                                                            │
│  1. SIMD Padding Verification:                                             │
│     Safely copies unpadded strings to thread_local buffers with            │
│     simdjson::SIMDJSON_PADDING (64 bytes) to eliminate vector over-reads.  │
│                                                                            │
│  2. Memory Isolation:                                                      │
│     O(1) streaming model. Thread-local scratch buffers reused with         │
│     capacity caps. Zero dynamic heap allocations in hot loops.             │
│                                                                            │
│  3. Non-Throwing FFI Guard:                                                │
│     Enclosing try/catch blocks catch std::exception and unknown errors.    │
│     Protected lua_pcall wraps emit callbacks to prevent longjmp crashes.   │
│                                                                            │
│  4. Deterministic Schema Sanitisation:                                     │
│     Nested attributes flattened into sanitised ClickHouse-aligned types    │
│     without schema injection or arbitrary code execution risks.            │
└────────────────────────────────────────────────────────────────────────────┘
```

### Key Security Invariants

1. **SIMD Buffer Over-Read Prevention (`SIMDJSON_PADDING`)**:
   `simdjson::ondemand` requires 64 bytes of padding beyond the string payload to allow AVX-512 / AVX2 / NEON vector loads without triggering segmentation faults across page boundaries. `vector-lua-otel` checks whether the input string has sufficient capacity or safely copies it to an aligned, padded `thread_local` buffer.

2. **FFI Boundary & Exception Containment**:
   Crossing the C++/Lua boundary with uncaught C++ exceptions triggers undefined behaviour (`std::terminate`). Conversely, Lua panics or uncaught `lua_error` calls invoke `longjmp`, bypassing C++ non-trivial destructors. `vector-lua-otel` implements strict two-way protection:
   - Outer `try ... catch` blocks around all `lua_CFunction` entry points convert exceptions to Lua error return tuples `(nil, "error message", count)`.
   - Lua callback emissions are executed under `lua_pcall` with clean stack balancing.

3. **Denial-of-Service (DoS) and Resource Exhaustion**:
   Malicious or malformed OTLP batches containing millions of sparse nodes or deeply nested JSON structures could trigger memory exhaustion in naive parsers. `vector-lua-otel` enforces an $O(1)$ streaming memory model: records are emitted immediately or in small fixed batches ($N \le 100$), and parser capacity is recycled rather than reallocated.

4. **Integer Precision & Overflow Protection**:
   Nanosecond timestamps and 64-bit metric counters are handled using exact 64-bit integer arithmetic (`uint64_t` / `int64_t`) and pushed as native Lua 5.4 integers (`lua_pushinteger`). Floating-point truncation (IEEE-754 double precision limit at 53 bits) is strictly avoided.

---

## 6. Operational Hardening Guidelines for Deployers

When compiling and deploying `vector-lua-otel` in production environments, operators should apply the following defensive controls:

### Binary Hardening Flags (CMake / Compiler)

Ensure build pipelines enforce standard compiler and linker exploit mitigations:
```bash
# Relocation Read-Only (RELRO) and Immediate Binding (BIND_NOW)
-Wl,-z,relro,-z,now

# Position Independent Executable / Code
-fPIC -fPIE

# Stack Smashing Protection
-fstack-protector-strong

# Standard Library Buffer Hardening
-D_FORTIFY_SOURCE=3 -O2
```

### Host Isolation & Vector Sandboxing

1. **Unprivileged Execution**: Run the host Vector daemon under a dedicated, unprivileged system account (e.g. `vector:vector`). Never execute Vector as `root`.
2. **Resource Constraints**: Enforce cgroup or Kubernetes memory and CPU quotas (`limits.memory`, `limits.cpu`) on Vector containers to neutralise downstream ingestion surges.
3. **Module Path Confinement**: Secure the filesystem directory containing `vector_otel.so` with strict ownership (`chown root:root`) and read-only permissions (`chmod 0555`) to prevent unauthorised library replacement or injection.

---

## 7. Out of Scope

The following scenarios are considered out of scope for security vulnerabilities unless they lead to native memory corruption or code execution:
* Malformed JSON payloads that are rejected gracefully with `(nil, err)` returns.
* Vector configuration errors (e.g. placing `require("vector_otel")` inside `hooks.process` instead of top-level `source:`).
* Network-level volumetric denial-of-service attacks targeting Vector's listening sockets prior to payload decoding.
