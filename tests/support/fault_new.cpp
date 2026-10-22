#include "fault_new.hpp"

#include <cstdlib>
#include <new>

namespace vector_otel::test {
namespace {

// Trivially constructible/destructible, so access never allocates.
constinit thread_local CppFaultState g_state{};

// The single decision point used by every replaced allocation function.
bool ShouldDeny() noexcept {
  CppFaultState& s = g_state;
  if (s.mode == CppFaultMode::kDisabled) {
    return false;
  }
  const std::size_t index = s.attempts++;
  const bool deny = (s.mode == CppFaultMode::kPersistent) ? (index >= s.skip) : (index == s.skip);
  if (deny) {
    ++s.denied;
  }
  return deny;
}

void* AllocateOrNull(std::size_t size, std::size_t alignment) noexcept {
  if (ShouldDeny()) {
    return nullptr;
  }
  if (size == 0) {
    size = 1;
  }
  if (alignment <= __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
    return std::malloc(size);
  }
  void* p = nullptr;
  if (alignment < sizeof(void*)) {
    alignment = sizeof(void*);
  }
  return posix_memalign(&p, alignment, size) == 0 ? p : nullptr;
}

void* AllocateOrThrow(std::size_t size, std::size_t alignment) {
  void* p = AllocateOrNull(size, alignment);
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  return p;
}

constexpr std::size_t kDefaultAlign = __STDCPP_DEFAULT_NEW_ALIGNMENT__;

}  // namespace

CppFaultScope::CppFaultScope(CppFaultMode mode, std::size_t skip) noexcept : saved_(g_state) {
  g_state = CppFaultState{mode, skip, 0, 0};
}

CppFaultScope::~CppFaultScope() { g_state = saved_; }

CppFaultStats CppFaultScope::stats() const noexcept {
  return CppFaultStats{g_state.attempts, g_state.denied};
}

}  // namespace vector_otel::test

namespace vt = vector_otel::test;

// ---- operator new family: every variant funnels into AllocateOrNull/AllocateOrThrow ----
void* operator new(std::size_t n) { return vt::AllocateOrThrow(n, vt::kDefaultAlign); }
void* operator new[](std::size_t n) { return vt::AllocateOrThrow(n, vt::kDefaultAlign); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  return vt::AllocateOrNull(n, vt::kDefaultAlign);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  return vt::AllocateOrNull(n, vt::kDefaultAlign);
}
void* operator new(std::size_t n, std::align_val_t a) {
  return vt::AllocateOrThrow(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a) {
  return vt::AllocateOrThrow(n, static_cast<std::size_t>(a));
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return vt::AllocateOrNull(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return vt::AllocateOrNull(n, static_cast<std::size_t>(a));
}

// ---- operator delete family: all memory comes from malloc/posix_memalign, so free() pairs ----
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
