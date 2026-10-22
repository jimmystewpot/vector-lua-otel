#pragma once

// Test-only C++ allocation fault injection.
//
// fault_new.cpp replaces the global operator new/delete family. It must be compiled into the
// fault-test executable ONLY, never into a production library or module.
//
// Semantics:
//  * Faults exist only while a CppFaultScope is alive on the current thread.
//  * kOneShot(skip): the attempt with zero-based index `skip` is denied; all others succeed.
//  * kPersistent(skip): attempts with index >= `skip` are denied.
//  * A nested scope starts with fresh counters; on destruction the enclosing scope's complete
//    state (plan, attempts, denied, one-shot progress) is restored unchanged.
//  * Bookkeeping never allocates.

#include <cstddef>

namespace vector_otel::test {

enum class CppFaultMode : int { kDisabled = 0, kOneShot, kPersistent };

struct CppFaultStats {
  std::size_t attempts = 0;
  std::size_t denied = 0;
};

struct CppFaultState {
  CppFaultMode mode = CppFaultMode::kDisabled;
  std::size_t skip = 0;
  std::size_t attempts = 0;
  std::size_t denied = 0;
};

class CppFaultScope {
 public:
  CppFaultScope(CppFaultMode mode, std::size_t skip) noexcept;
  ~CppFaultScope();
  CppFaultScope(const CppFaultScope&) = delete;
  CppFaultScope& operator=(const CppFaultScope&) = delete;

  CppFaultStats stats() const noexcept;

 private:
  CppFaultState saved_;
};

}  // namespace vector_otel::test
