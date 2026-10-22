// Tests for the test-only Lua allocator fault harness (tests/support/fault_allocator.*).
#include <gtest/gtest.h>

#include <cstring>
#include <lua.hpp>

#include "fault_allocator.hpp"

namespace vector_otel::test {
namespace {

int AllocateUserdata(lua_State* L) {
  lua_newuserdatauv(L, 64, 0);
  return 1;
}

int CallAllocateUserdata(lua_State* L) {
  lua_pushcfunction(L, AllocateUserdata);
  return lua_pcall(L, 0, 1, 0);
}

TEST(LuaFaultHarness, StateClosedAfterUseLeavesZeroLiveBytes) {
  LuaFaultAllocator alloc;
  lua_State* L = alloc.NewState();
  ASSERT_NE(L, nullptr);
  luaL_openlibs(L);
  ASSERT_EQ(luaL_dostring(L, "local t = {} for i = 1, 1000 do t[i] = tostring(i) .. 'x' end"),
            LUA_OK);
  EXPECT_GT(alloc.live_bytes(), 0u);
  lua_close(L);
  EXPECT_EQ(alloc.live_bytes(), 0u);
}

TEST(LuaFaultHarness, PlanThresholdsCountFromTheMomentThePlanIsSet) {
  LuaFaultAllocator alloc;
  lua_State* L = alloc.NewState();
  ASSERT_NE(L, nullptr);
  // Plenty of allocation activity BEFORE the plan exists.
  luaL_openlibs(L);
  ASSERT_EQ(luaL_dostring(L, "local t = {} for i = 1, 500 do t[i] = {i} end"), LUA_OK);
  ASSERT_GT(alloc.live_bytes(), 16u * 1024u);
  ASSERT_EQ(alloc.stats().attempts, 0u);  // none of it counts toward a later plan

  // Two consecutive denials defeat Lua's emergency-GC retry: the very next request fails.
  alloc.SetPlan(LuaFaultMode::kWindow, 0, 2);
  EXPECT_EQ(CallAllocateUserdata(L), LUA_ERRMEM);
  const LuaFaultStats first = alloc.stats();
  lua_settop(L, 0);
  EXPECT_EQ(first.attempts, 2u);
  EXPECT_EQ(first.denied, 2u);

  // The window is consumed: the same call now succeeds.
  EXPECT_EQ(CallAllocateUserdata(L), LUA_OK);
  EXPECT_EQ(alloc.stats().denied, 2u);
  lua_settop(L, 0);

  // A skip is also relative to SetPlan: deny requests #4 and #5 from now (zero-based 3 and 4).
  alloc.SetPlan(LuaFaultMode::kWindow, 3, 2);
  int status = LUA_OK;
  int calls = 0;
  for (; calls < 64 && status == LUA_OK; ++calls) {
    status = CallAllocateUserdata(L);
    lua_settop(L, 0);
  }
  EXPECT_EQ(status, LUA_ERRMEM);
  EXPECT_EQ(calls, 4);
  EXPECT_EQ(alloc.stats().attempts, 5u);
  EXPECT_EQ(alloc.stats().denied, 2u);
  lua_close(L);
  EXPECT_EQ(alloc.live_bytes(), 0u);
}

// Pins the Lua behaviour the failure-safety tests depend on: one denied request is retried after
// an emergency GC and succeeds, so single-denial plans cannot be used to force LUA_ERRMEM.
TEST(LuaFaultHarness, SingleDenialIsAbsorbedByLuaEmergencyCollectionRetry) {
  LuaFaultAllocator alloc;
  lua_State* L = alloc.NewState();
  ASSERT_NE(L, nullptr);
  luaL_openlibs(L);
  alloc.SetPlan(LuaFaultMode::kWindow, 0, 1);
  EXPECT_EQ(CallAllocateUserdata(L), LUA_OK);
  EXPECT_EQ(alloc.stats().attempts, 2u);  // the denied request plus Lua's retry
  EXPECT_EQ(alloc.stats().denied, 1u);
  lua_close(L);
  EXPECT_EQ(alloc.live_bytes(), 0u);
}

TEST(LuaFaultHarness, PersistentDenialStillAcceptsFrees) {
  LuaFaultAllocator alloc;
  lua_State* L = alloc.NewState();
  ASSERT_NE(L, nullptr);
  luaL_openlibs(L);
  ASSERT_EQ(luaL_dostring(L, "big = {} for i = 1, 2000 do big[i] = {i} end"), LUA_OK);

  alloc.SetPlan(LuaFaultMode::kPersistent, 0);
  EXPECT_EQ(CallAllocateUserdata(L), LUA_ERRMEM);
  lua_settop(L, 0);
  EXPECT_EQ(CallAllocateUserdata(L), LUA_ERRMEM);
  lua_settop(L, 0);
  const std::size_t before = alloc.live_bytes();
  lua_gc(L, LUA_GCCOLLECT);  // frees/shrinks must never be denied
  EXPECT_LE(alloc.live_bytes(), before);
  EXPECT_GE(alloc.stats().denied, 2u);

  alloc.ClearPlan();
  lua_close(L);
  EXPECT_EQ(alloc.live_bytes(), 0u);
}

TEST(LuaFaultHarness, FailedReallocLeavesTheOldBlockValidAndCounted) {
  LuaFaultAllocator alloc;
  void* ud = alloc.userdata();
  void* p = LuaFaultAllocator::Alloc(ud, nullptr, LUA_TSTRING, 16);  // osize is a type tag here
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(alloc.live_bytes(), 16u);  // the tag must not be counted as a size
  std::memset(p, 0xAB, 16);

  alloc.SetPlan(LuaFaultMode::kPersistent, 0);
  ASSERT_EQ(LuaFaultAllocator::Alloc(ud, p, 16, 64), nullptr);  // growth denied
  EXPECT_EQ(alloc.live_bytes(), 16u);
  unsigned char expected[16];
  std::memset(expected, 0xAB, sizeof expected);
  EXPECT_EQ(std::memcmp(p, expected, sizeof expected), 0);

  void* shrunk = LuaFaultAllocator::Alloc(ud, p, 16, 8);  // Lua requires shrinking never fail
  ASSERT_NE(shrunk, nullptr);
  EXPECT_EQ(alloc.live_bytes(), 8u);
  EXPECT_EQ(LuaFaultAllocator::Alloc(ud, shrunk, 8, 0), nullptr);  // free
  EXPECT_EQ(alloc.live_bytes(), 0u);
  EXPECT_EQ(alloc.stats().denied, 1u);
}

}  // namespace
}  // namespace vector_otel::test
