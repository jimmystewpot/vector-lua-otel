// Tests for the shared contract fixtures (tests/support/contract_fixtures.*): a real Lua 5.4 state
// with both production modules loaded, registry-root counting, and a callback that counts only
// emissions that returned normally.
#include <gtest/gtest.h>

#include <lua.hpp>
#include <string>

#include "contract_fixtures.hpp"
#include "vector_otel/core/process_runner.hpp"

namespace vector_otel::test {
namespace {

TEST(ContractFixtures, BothModulesAreLoadedWithProcessFunctions) {
  ModuleState state;
  lua_State* L = state.L();
  for (const char* module : {"vector_otel", "vector_native"}) {
    for (const char* signal : {"logs", "traces", "metrics"}) {
      lua_getglobal(L, module);
      ASSERT_TRUE(lua_istable(L, -1)) << module;
      lua_getfield(L, -1, signal);
      ASSERT_TRUE(lua_istable(L, -1)) << module << "." << signal;
      lua_getfield(L, -1, "process");
      EXPECT_TRUE(lua_iscfunction(L, -1)) << module << "." << signal;
      lua_pop(L, 3);
    }
  }
}

TEST(ContractFixtures, RegistryRootCountSeesReferencesAndIgnoresFreeListEntries) {
  ModuleState state;
  lua_State* L = state.L();
  const std::size_t base = CountRegistryRoots(L);

  lua_newtable(L);
  const int ref_a = luaL_ref(L, LUA_REGISTRYINDEX);
  lua_pushstring(L, "x");
  const int ref_b = luaL_ref(L, LUA_REGISTRYINDEX);
  EXPECT_EQ(CountRegistryRoots(L), base + 2);

  luaL_unref(L, LUA_REGISTRYINDEX, ref_a);
  EXPECT_EQ(CountRegistryRoots(L), base + 1);  // freed slot holds a free-list link, not a root
  luaL_unref(L, LUA_REGISTRYINDEX, ref_b);
  EXPECT_EQ(CountRegistryRoots(L), base);
}

TEST(ContractFixtures, SnapshotIsStableOnAnIdleStateAcrossFullCollections) {
  ModuleState state;
  const StateSnapshot first = TakeSnapshot(state);
  const StateSnapshot second = TakeSnapshot(state);
  EXPECT_EQ(first.registry_roots, second.registry_roots);
  EXPECT_EQ(first.live_bytes, second.live_bytes);
  EXPECT_GT(first.live_bytes, 0u);
}

TEST(ContractFixtures, RowCounterCountsOnlyCallsThatReturnedNormally) {
  ModuleState state;
  lua_State* L = state.L();
  RowCounter counter;
  counter.FailAfter(2);  // the third call raises a Lua error after being entered

  for (int i = 0; i < 3; ++i) {
    counter.PushEmit(L);
    lua_newtable(L);
    const int status = lua_pcall(L, 1, 0, 0);
    EXPECT_EQ(status, i < 2 ? LUA_OK : LUA_ERRRUN) << "call " << i;
    lua_settop(L, 0);
  }
  EXPECT_EQ(counter.entered(), 3u);
  EXPECT_EQ(counter.completed(), 2u);
}

TEST(ContractFixtures, RealLogsProcessDeliversEveryRecordAndLeavesStateBaselineUnchanged) {
  ModuleState state;
  lua_State* L = state.L();
  RowCounter counter;
  const std::string payload = MakeOtlpLogsJson(3);

  // Warm once so lazily created module state is part of the baseline.
  ProcessResult warm = CallProcess(L, "vector_otel", "logs", payload, counter);
  ASSERT_EQ(warm.status, LUA_OK);
  const StateSnapshot baseline = TakeSnapshot(state);

  const std::size_t before = counter.completed();
  ProcessResult result = CallProcess(L, "vector_otel", "logs", payload, counter);
  EXPECT_EQ(result.status, LUA_OK);
  EXPECT_EQ(result.count, 3);
  EXPECT_TRUE(result.second_is_nil);
  // one callback per N rows (N = compile-time batch size)
  EXPECT_EQ(counter.completed() - before, (3u + core::kEmitBatchSize - 1) / core::kEmitBatchSize);

  const StateSnapshot after = TakeSnapshot(state);
  EXPECT_EQ(after.registry_roots, baseline.registry_roots);
  EXPECT_EQ(after.live_bytes, baseline.live_bytes);
}

TEST(ContractFixtures, EmptyPayloadReturnsZeroAndNilWithoutCallbacks) {
  ModuleState state;
  RowCounter counter;
  ProcessResult result =
      CallProcess(state.L(), "vector_otel", "logs", "{\"resourceLogs\":[]}", counter);
  EXPECT_EQ(result.status, LUA_OK);
  EXPECT_EQ(result.count, 0);
  EXPECT_TRUE(result.second_is_nil);
  EXPECT_EQ(counter.entered(), 0u);
}

TEST(ContractFixtures, UnknownModuleOrSignalReportsAnErrorInsteadOfPanicking) {
  ModuleState state;
  lua_State* L = state.L();
  RowCounter counter;
  const int top = lua_gettop(L);
  ProcessResult bad_module = CallProcess(L, "no_such_module", "logs", "{}", counter);
  ProcessResult bad_signal = CallProcess(L, "vector_otel", "no_such_signal", "{}", counter);
  EXPECT_NE(bad_module.status, LUA_OK);
  EXPECT_FALSE(bad_module.error.empty());
  EXPECT_NE(bad_signal.status, LUA_OK);
  EXPECT_FALSE(bad_signal.error.empty());
  EXPECT_EQ(lua_gettop(L), top);  // stack restored on every path
  EXPECT_EQ(counter.entered(), 0u);
}

}  // namespace
}  // namespace vector_otel::test
