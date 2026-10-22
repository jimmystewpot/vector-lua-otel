#pragma once

// Shared test fixtures: a real Lua 5.4 state (tracked allocator) with both production modules
// loaded, registry-root counting, and an emit callback that counts only normal returns.

#include <cstddef>
#include <limits>
#include <lua.hpp>
#include <string>

#include "fault_allocator.hpp"

namespace vector_otel::test {

// Owns the allocator and the state; the allocator outlives lua_close().
class ModuleState {
 public:
  ModuleState();
  ~ModuleState();
  ModuleState(const ModuleState&) = delete;
  ModuleState& operator=(const ModuleState&) = delete;

  lua_State* L() const { return L_; }
  LuaFaultAllocator& allocator() { return alloc_; }

 private:
  LuaFaultAllocator alloc_;
  lua_State* L_ = nullptr;
};

// Number of registry entries that are real GC roots held through luaL_ref-style integer keys:
// integer key > LUA_RIDX_LAST whose value is not a number (free-list links are numbers).
// Call with no fault plan active.
std::size_t CountRegistryRoots(lua_State* L);

void FullCollect(lua_State* L);

struct StateSnapshot {
  std::size_t registry_roots = 0;
  std::size_t live_bytes = 0;
};
// Full-collects (twice), then reads the registry root count and tracked live bytes.
StateSnapshot TakeSnapshot(ModuleState& state);

// Emit callback. `completed()` counts calls that RETURNED NORMALLY; a call that raises after being
// entered is counted only by `entered()`.
class RowCounter {
 public:
  void FailAfter(std::size_t completed_rows) { fail_after_ = completed_rows; }
  void PushEmit(lua_State* L);  // pushes a C closure bound to this counter

  std::size_t entered() const { return entered_; }
  std::size_t completed() const { return completed_; }

 private:
  static int Emit(lua_State* L);

  std::size_t entered_ = 0;
  std::size_t completed_ = 0;
  std::size_t fail_after_ = std::numeric_limits<std::size_t>::max();
};

struct ProcessResult {
  int status = LUA_OK;  // lua_pcall status of the process call
  int nresults = 0;
  bool first_is_nil = false;
  bool second_is_nil = false;
  lua_Integer count = -1;    // first result when it is an integer
  std::string error;         // second result when it is a string
  lua_Integer partial = -1;  // third result when it is an integer
};

// Calls <module>.<signal>.process(payload, emit) under lua_pcall and restores the stack.
ProcessResult CallProcess(lua_State* L, const char* module, const char* signal,
                          const std::string& payload, RowCounter& counter);

// Minimal valid OTLP logs JSON with `records` log records in one resource/scope.
std::string MakeOtlpLogsJson(std::size_t records);

}  // namespace vector_otel::test
