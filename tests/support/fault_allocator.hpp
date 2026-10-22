#pragma once

// Test-only Lua allocator with accounting and fault injection.
//
// Create the allocator BEFORE the lua_State and destroy it AFTER lua_close(): Lua keeps a raw
// pointer to it. Lua's contract: the allocator may fail only when growing (nsize > osize, or a
// new block); shrinking and freeing never fail, and when ptr == nullptr `osize` is a type tag, not
// a size.
//
// Plans count allocation requests from the moment SetPlan() is called, never from state creation.
//
// IMPORTANT (Lua 5.4 behaviour): when the allocator returns NULL for a growth request, Lua runs an
// emergency full GC and retries the SAME request once. A single denial (kWindow count == 1) is
// therefore absorbed and the operation succeeds; two consecutive denials (count >= 2) or
// kPersistent surface as LUA_ERRMEM / a raised memory error.

#include <cstddef>
#include <lua.hpp>

namespace vector_otel::test {

enum class LuaFaultMode : int { kDisabled = 0, kWindow, kPersistent };

struct LuaFaultStats {
  std::size_t attempts = 0;  // growth requests seen since the plan was set
  std::size_t denied = 0;    // growth requests refused since the plan was set
};

class LuaFaultAllocator {
 public:
  LuaFaultAllocator() = default;
  LuaFaultAllocator(const LuaFaultAllocator&) = delete;
  LuaFaultAllocator& operator=(const LuaFaultAllocator&) = delete;

  lua_State* NewState() { return lua_newstate(&LuaFaultAllocator::Alloc, userdata()); }
  void* userdata() { return this; }

  // kWindow: deny `count` consecutive growth requests starting at zero-based index `skip`
  //          (counted from now). kPersistent: deny every request with index >= skip.
  // Resets the attempt/denied counters.
  void SetPlan(LuaFaultMode mode, std::size_t skip, std::size_t count = 1) noexcept;
  void ClearPlan() noexcept { SetPlan(LuaFaultMode::kDisabled, 0, 0); }

  LuaFaultStats stats() const noexcept { return {attempts_, denied_}; }
  std::size_t live_bytes() const noexcept { return live_bytes_; }

  static void* Alloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize);

 private:
  bool ShouldDeny() noexcept;

  LuaFaultMode mode_ = LuaFaultMode::kDisabled;
  std::size_t skip_ = 0;
  std::size_t count_ = 0;
  std::size_t attempts_ = 0;
  std::size_t denied_ = 0;
  std::size_t live_bytes_ = 0;
};

}  // namespace vector_otel::test
