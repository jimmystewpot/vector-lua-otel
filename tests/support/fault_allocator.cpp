#include "fault_allocator.hpp"

#include <cstdlib>

namespace vector_otel::test {

void LuaFaultAllocator::SetPlan(LuaFaultMode mode, std::size_t skip, std::size_t count) noexcept {
  mode_ = mode;
  skip_ = skip;
  count_ = count;
  attempts_ = 0;
  denied_ = 0;
}

bool LuaFaultAllocator::ShouldDeny() noexcept {
  if (mode_ == LuaFaultMode::kDisabled) {
    return false;
  }
  const std::size_t index = attempts_++;
  const bool deny = (mode_ == LuaFaultMode::kPersistent)
                        ? (index >= skip_)
                        : (index >= skip_ && index - skip_ < count_);
  if (deny) {
    ++denied_;
  }
  return deny;
}

void* LuaFaultAllocator::Alloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
  auto* self = static_cast<LuaFaultAllocator*>(ud);
  // For a brand-new block Lua passes a type tag in osize; the old size is really zero.
  const std::size_t old_size = (ptr == nullptr) ? 0 : osize;
  if (nsize == 0) {
    std::free(ptr);
    self->live_bytes_ -= old_size;
    return nullptr;
  }
  if (nsize > old_size && self->ShouldDeny()) {
    return nullptr;  // growth refused; the old block (if any) is untouched and still counted
  }
  void* fresh = std::realloc(ptr, nsize);
  if (fresh == nullptr) {
    return nullptr;
  }
  self->live_bytes_ = self->live_bytes_ - old_size + nsize;
  return fresh;
}

}  // namespace vector_otel::test
