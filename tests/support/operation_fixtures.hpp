#pragma once

// Shared harness for fault-target tests that exercise operation-level code: a raw Lua state with
// the fault allocator, and scenarios that run inside a real lua_pcall'd C closure whose upvalue 1
// is the module anchor (exactly like a production process() closure). Observations are plain POD
// copied out of the closure so that no gtest object lives across a Lua longjmp.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <lua.hpp>
#include <string>
#include <string_view>

#include "fault_allocator.hpp"
#include "vector_otel/core/module_state.hpp"
#include "vector_otel/core/operation_context.hpp"
#include "vector_otel/core/protected_lua.hpp"

namespace vector_otel::core::fixtures {

using test::LuaFaultAllocator;
using test::LuaFaultMode;

class RawState {
 public:
  RawState() : L_(alloc_.NewState()) {}
  ~RawState() {
    if (L_ != nullptr) {
      lua_close(L_);
    }
  }
  RawState(const RawState&) = delete;
  RawState& operator=(const RawState&) = delete;
  lua_State* L() const { return L_; }
  LuaFaultAllocator& alloc() { return alloc_; }

 private:
  LuaFaultAllocator alloc_;
  lua_State* L_;
};

// ---- scenario plumbing -----------------------------------------------------------------------

struct Scenario {
  void (*body)(OperationContext&, Scenario&) = nullptr;
  ModuleKind kind = ModuleKind::Otel;
  // observations (POD only)
  bool leaf_ok = false;
  ErrorCode code = ErrorCode::None;
  char message[64] = {};
  LuaFaultAllocator* alloc = nullptr;  // for scenarios that arm denial from inside the call
  // generic inputs for admission-style scenarios (borrowed, outlive the call)
  Signal signal = Signal::Logs;
  const char* text = nullptr;
  std::size_t text_len = 0;
  const char* chunk = nullptr;    // Lua chunk that returns the table input
  std::size_t text_capacity = 0;  // padded capacity when `text` already carries padding
  bool flag = false;
  bool flag2 = false;
  bool fixture_error = false;
  std::size_t number = 0;
  std::size_t skip = 0;
  std::size_t admitted_size = 0;
  const char* admitted_data = nullptr;
  std::size_t denied = 0;
  int pending_type = -1;
  int pending_after_clear = -1;
  int row_type_at_failure = -1;
  int attrs_type_at_failure = -1;
};

inline int HarnessClosure(lua_State* L) {
  auto* sc = static_cast<Scenario*>(lua_touserdata(L, 1));
  OperationContext ctx;
  InitOperationContext(ctx, L, sc->kind, sc->signal, lua_upvalueindex(1));
  sc->body(ctx, *sc);
  ClearInvocationRoots(ctx);
  return ReturnOperationResult(ctx);
}

inline void CopyMessage(Scenario& sc, const OperationContext& ctx) {
  const std::string_view m = ctx.error.view();
  const std::size_t n = m.size() < sizeof(sc.message) - 1 ? m.size() : sizeof(sc.message) - 1;
  std::memcpy(sc.message, m.data(), n);
  sc.message[n] = '\0';
}

struct Observed {
  int status = LUA_OK;
  int nresults = 0;
  bool first_nil = false;
  bool second_nil = false;
  std::string text;       // second result when a string
  lua_Integer last = -1;  // last result when an integer
};

// Builds the closure (no fault active), arms `plan`, runs the scenario under lua_pcall.
template <typename ArmFn>
inline Observed RunScenario(RawState& s, Scenario& sc, ArmFn arm) {
  lua_State* L = s.L();
  Observed o;
  const int base = lua_gettop(L);
  if (!EnsureModuleState(L, sc.kind)) {
    o.status = LUA_ERRMEM;
    lua_settop(L, base);
    return o;
  }
  lua_pushcclosure(L, &HarnessClosure, 1);
  lua_pushlightuserdata(L, &sc);
  arm();
  o.status = lua_pcall(L, 1, LUA_MULTRET, 0);
  s.alloc().ClearPlan();
  o.nresults = lua_gettop(L) - base;
  if (o.status == LUA_OK && o.nresults >= 2) {
    o.first_nil = lua_isnil(L, base + 1);
    o.second_nil = lua_isnil(L, base + 2);
    if (lua_type(L, base + 2) == LUA_TSTRING) {
      o.text = lua_tostring(L, base + 2);
    }
    if (lua_isinteger(L, lua_gettop(L))) {
      o.last = lua_tointeger(L, lua_gettop(L));
    }
  }
  lua_settop(L, base);
  return o;
}

inline int AnchorSlotType(lua_State* L, ModuleKind kind, AnchorSlot slot) {
  EXPECT_TRUE(EnsureModuleState(L, kind));
  const int anchor = lua_gettop(L);
  const int type = AnchorType(L, anchor, slot);
  lua_pop(L, 1);
  return type;
}

}  // namespace vector_otel::core::fixtures
