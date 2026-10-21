# Vendored Third-Party Dependencies

This directory contains vendored (pinned, checked-in) copies of third-party libraries required to build `vector-lua-otel` offline on Ubuntu 22.04. Vendoring ensures the build is deterministic and does not require network access.

## What is vendored

- **simdjson** v5.0.5 (Apache-2.0/MIT License)
  - High-performance JSON parsing library
  - Vendored subdirectories: `CMakeLists.txt`, `simdjson.pc.in`, `cmake/`, `include/`, `src/`, `LICENSE`, `LICENSE-MIT`
  - Used by: OTLP JSON extraction in C++20 code

- **googletest** v1.14.0 (BSD-3-Clause License)
  - C++ testing framework (gtest)
  - Vendored subdirectories: `CMakeLists.txt`, `googletest/CMakeLists.txt`, `googletest/cmake/`, `googletest/include/`, `googletest/src/`, `LICENSE`
  - Excludes: `googletest/test/` (unit tests), `googletest/docs/`, `googletest/samples/` (not needed for building)
  - Used by: `tests/cpp/` GoogleTest test suite

Each dependency has a `VENDOR.md` file documenting upstream source, tag, commit SHA, vendored paths, and a tree-wide SHA-256 checksum for integrity verification.

## Why vendored

1. **Offline builds**: Ubuntu 22.04 environments without network access can build without requiring internet.
2. **Deterministic builds**: Fixed versions and tree checksums ensure reproducible builds across machines.
3. **No surprise updates**: Dependencies do not auto-update via `FetchContent`; updates are manual and tracked via git commits.

## Trade-off

**Dependabot cannot track these dependencies.** Manual updates are required:
1. Run `bash scripts/vendor_deps.sh <dep> <tag>` to fetch and vendor a new version
2. Verify changes: `git diff --stat third_party/`
3. Commit the regenerated `third_party/<dep>` tree; the script itself changes in a separate commit
4. Run the offline build test: `bash scripts/verify_offline_build.sh`

## Updating a dependency

To update simdjson to a newer tag:

```bash
bash scripts/vendor_deps.sh simdjson v5.1.0
git add third_party/simdjson
git commit -m "build: vendor simdjson 5.1.0"
```

To update googletest:

```bash
bash scripts/vendor_deps.sh googletest v1.15.0
git add third_party/googletest
git commit -m "build: vendor googletest 1.15.0"
```

## Verification

To verify that vendored trees have not been tampered with:

```bash
bash scripts/vendor_deps.sh --verify
```

This command (usable offline) recomputes the tree-wide SHA-256 checksum for each dependency and compares it against the value recorded in `VENDOR.md`. Exit code 0 means all trees are intact; non-zero means at least one tree is corrupted or missing.

## License compliance

- **simdjson**: Apache License 2.0 / MIT (dual-licensed) — see `simdjson/LICENSE` and `simdjson/LICENSE-MIT`
- **googletest**: BSD 3-Clause License — see `googletest/LICENSE`

Both licenses permit vendoring and distribution; no license headers were modified.
