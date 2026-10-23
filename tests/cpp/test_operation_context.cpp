// Operation context, protected leaves and per-state module anchors (fault target only: it uses
// the Lua allocator fault injector).
//
// Every scenario runs inside a real lua_pcall'd C closure whose upvalue 1 is the module anchor,
// exactly like a production process() closure, and observations are plain POD copied out of the
// closure so that no gtest object lives across a Lua longjmp.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <lua.hpp>
#include <string>
#include <string_view>
#include <type_traits>

#include "contract_fixtures.hpp"
#include "fault_allocator.hpp"
#include "operation_fixtures.hpp"
#include "vector_otel/core/config.hpp"
#include "vector_otel/core/module_runner.hpp"
#include "vector_otel/core/module_state.hpp"
#include "vector_otel/core/operation_context.hpp"
#include "vector_otel/core/protected_lua.hpp"

extern "C" {
int luaopen_vector_otel(lua_State* L);
int luaopen_vector_otel_logs(lua_State* L);
int luaopen_vector_otel_traces(lua_State* L);
int luaopen_vector_native(lua_State* L);
int luaopen_vector_native_metrics(lua_State* L);
}

namespace vector_otel::core {
namespace {

using test::FullCollect;
using test::LuaFaultAllocator;
using test::LuaFaultMode;

static_assert(std::is_trivially_destructible_v<ErrorStatus>);
static_assert(std::is_trivially_destructible_v<OperationContext>);

using namespace fixtures;

// ---- fixed diagnostics and truthful fallback ---------------------------------------------------

TEST(OperationContextTest, ErrorStatusIsFixedSizeAndTruncatesTo511Bytes) {
  ErrorStatus status;
  EXPECT_EQ(status.code, ErrorCode::None);
  EXPECT_TRUE(status.view().empty());
  static_assert(sizeof(ErrorStatus::message) == 512);

  const std::string huge(2000, 'x');
  status.Set(ErrorCode::Cpp, huge);
  EXPECT_EQ(status.code, ErrorCode::Cpp);
  ASSERT_EQ(status.view().size(), 511u);
  EXPECT_EQ(status.view(), std::string_view(huge).substr(0, 511));
  EXPECT_EQ(status.message[511], '\0');

  status.Set(ErrorCode::Json, "short");
  EXPECT_EQ(status.view(), "short");
}

void FallbackBody(OperationContext& ctx, Scenario&) {
  ctx.delivered_count = 1;
  ctx.error.Set(ErrorCode::Memory, "detailed text that needs a Lua string to be returned");
}

TEST(OperationContextTest, FixedDiagnosticsAndTruthfulFallback) {
  RawState s;
  Scenario sc;
  sc.body = FallbackBody;
  // Every Lua growth request is denied from the moment the call starts: the detailed message
  // cannot be materialised, so the pre-interned fallback must be returned with the true count.
  const Observed o = RunScenario(s, sc, [&] { s.alloc().SetPlan(LuaFaultMode::kPersistent, 0); });
  ASSERT_EQ(o.status, LUA_OK);
  ASSERT_EQ(o.nresults, 3);
  EXPECT_TRUE(o.first_nil);
  EXPECT_EQ(o.text, "memory allocation failed");
  EXPECT_EQ(o.last, 1);
}

void JsonErrorBody(OperationContext& ctx, Scenario&) {
  ctx.delivered_count = 4;
  ctx.error.Set(ErrorCode::Json, "unexpected token at offset 12");
}

TEST(OperationContextTest, DetailedMessageIsReturnedWhenLuaCanAllocate) {
  RawState s;
  Scenario sc;
  sc.body = JsonErrorBody;
  const Observed o = RunScenario(s, sc, [] {});
  ASSERT_EQ(o.status, LUA_OK);
  ASSERT_EQ(o.nresults, 3);
  EXPECT_TRUE(o.first_nil);
  EXPECT_EQ(o.text, "unexpected token at offset 12");
  EXPECT_EQ(o.last, 4);

  // With allocation denied, a non-memory error degrades to the generic extraction message.
  // (A fresh state: the message string above is already interned in `s` and would need no
  // allocation to push again.)
  RawState fresh;
  const Observed denied =
      RunScenario(fresh, sc, [&] { fresh.alloc().SetPlan(LuaFaultMode::kPersistent, 0); });
  ASSERT_EQ(denied.status, LUA_OK);
  ASSERT_EQ(denied.nresults, 3);
  EXPECT_EQ(denied.text, "extraction failed");
  EXPECT_EQ(denied.last, 4);
}

void SuccessBody(OperationContext& ctx, Scenario&) { ctx.delivered_count = 7; }

TEST(OperationContextTest, SuccessReturnsCountAndNilWithoutAllocating) {
  RawState s;
  Scenario sc;
  sc.body = SuccessBody;
  const Observed o = RunScenario(s, sc, [&] { s.alloc().SetPlan(LuaFaultMode::kPersistent, 0); });
  ASSERT_EQ(o.status, LUA_OK);
  ASSERT_EQ(o.nresults, 2);
  EXPECT_FALSE(o.first_nil);
  EXPECT_TRUE(o.second_nil);
  EXPECT_EQ(s.alloc().stats().denied, 0u);
}

// ---- anchors and bootstrap ---------------------------------------------------------------------

TEST(ModuleStateTest, ImmutableMessagesAreInternedAtBootstrapWithTheCompiledLimit) {
  RawState s;
  lua_State* L = s.L();
  ASSERT_TRUE(EnsureModuleState(L, ModuleKind::Otel));
  const int anchor = lua_gettop(L);
  struct Case {
    ImmutableSlot slot;
    std::string expected;
  } cases[] = {
      {ImmutableSlot::kPayloadLimitMessage,
       "payload exceeds " + std::to_string(kMaxPayloadBytes) + "-byte uncompressed JSON limit"},
      {ImmutableSlot::kRecordLimitMessage, "event exceeds 2048-record limit"},
      {ImmutableSlot::kMemoryMessage, "memory allocation failed"},
      {ImmutableSlot::kLuaMessage, "Lua operation failed"},
      {ImmutableSlot::kExtractionMessage, "extraction failed"},
      {ImmutableSlot::kPayloadArgMessage, "payload must be a string or table"},
      {ImmutableSlot::kEmitArgMessage, "emit callback must be a function"},
      {ImmutableSlot::kReentrantMessage,
       "re-entrant vector_otel.process() invocation is not permitted"},
      {ImmutableSlot::kStackMessage, "out of lua stack space"},
  };
  // kReadKeys (the rooted read-key table) is the last slot and is not a string.
  static_assert(static_cast<std::size_t>(ImmutableSlot::kReadKeys) + 1 ==
                static_cast<std::size_t>(ImmutableSlot::kCount));
  static_assert(sizeof(cases) / sizeof(cases[0]) ==
                static_cast<std::size_t>(ImmutableSlot::kReadKeys));
  for (const Case& c : cases) {
    PushImmutable(L, anchor, c.slot);
    ASSERT_EQ(lua_type(L, -1), LUA_TSTRING);
    EXPECT_EQ(std::string(lua_tostring(L, -1)), c.expected);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
}

TEST(ModuleStateTest, EveryInvocationSlotIsPrepopulatedFalse) {
  RawState s;
  lua_State* L = s.L();
  for (ModuleKind kind : {ModuleKind::Otel, ModuleKind::Native}) {
    ASSERT_TRUE(EnsureModuleState(L, kind));
    const int anchor = lua_gettop(L);
    for (int i = 0; i < static_cast<int>(AnchorSlot::kCount); ++i) {
      EXPECT_EQ(AnchorType(L, anchor, static_cast<AnchorSlot>(i)), LUA_TBOOLEAN) << "slot " << i;
    }
    lua_pop(L, 1);
  }
}

TEST(ModuleStateTest, EnsureIsIdempotentAndModulesGetDistinctAnchors) {
  RawState s;
  lua_State* L = s.L();
  ASSERT_TRUE(EnsureModuleState(L, ModuleKind::Otel));
  ASSERT_TRUE(EnsureModuleState(L, ModuleKind::Otel));
  EXPECT_TRUE(lua_rawequal(L, -1, -2));
  ASSERT_TRUE(EnsureModuleState(L, ModuleKind::Native));
  EXPECT_FALSE(lua_rawequal(L, -1, -2));
  lua_pop(L, 3);
}

bool PushProcessUpvalue(lua_State* L, const char* field) {
  // expects the module table at the top
  lua_getfield(L, -1, field);
  lua_getfield(L, -1, "process");
  lua_remove(L, -2);
  const char* name = lua_getupvalue(L, -1, 1);
  lua_remove(L, -2);  // function; leaves the upvalue on top
  return name != nullptr;
}

TEST(ModuleStateTest, ClosuresCaptureTheModuleAnchorForMainAndDirectSubmoduleLoads) {
  for (int order = 0; order < 2; ++order) {
    SCOPED_TRACE(order == 0 ? "submodule then aggregate" : "aggregate then submodule");
    RawState s;
    lua_State* L = s.L();
    int main_idx = 0;
    int sub_idx = 0;
    if (order == 0) {
      ASSERT_EQ(luaopen_vector_otel_logs(L), 1);
      sub_idx = lua_gettop(L);
      ASSERT_EQ(luaopen_vector_otel(L), 1);
      main_idx = lua_gettop(L);
    } else {
      ASSERT_EQ(luaopen_vector_otel(L), 1);
      main_idx = lua_gettop(L);
      ASSERT_EQ(luaopen_vector_otel_traces(L), 1);
      sub_idx = lua_gettop(L);
    }
    ASSERT_TRUE(EnsureModuleState(L, ModuleKind::Otel));
    const int anchor = lua_gettop(L);

    for (const char* sig : {"logs", "traces", "metrics"}) {
      lua_pushvalue(L, main_idx);
      ASSERT_TRUE(PushProcessUpvalue(L, sig)) << sig;
      EXPECT_TRUE(lua_rawequal(L, -1, anchor)) << sig;
      lua_settop(L, anchor);
    }
    // the directly opened submodule table has its own process closure
    lua_pushvalue(L, sub_idx);
    lua_getfield(L, -1, "process");
    ASSERT_EQ(lua_type(L, -1), LUA_TFUNCTION);
    ASSERT_NE(lua_getupvalue(L, -1, 1), nullptr);
    EXPECT_TRUE(lua_rawequal(L, -1, anchor));
    lua_settop(L, 0);
  }
}

TEST(ModuleStateTest, BothModulesInOneStateCaptureTheirOwnAnchor) {
  RawState s;
  lua_State* L = s.L();
  ASSERT_EQ(luaopen_vector_otel(L), 1);
  ASSERT_EQ(luaopen_vector_native(L), 1);
  ASSERT_TRUE(EnsureModuleState(L, ModuleKind::Otel));
  ASSERT_TRUE(EnsureModuleState(L, ModuleKind::Native));
  const int otel = lua_gettop(L) - 1;
  const int native = lua_gettop(L);
  EXPECT_FALSE(lua_rawequal(L, otel, native));
  lua_pushvalue(L, 1);
  ASSERT_TRUE(PushProcessUpvalue(L, "logs"));
  EXPECT_TRUE(lua_rawequal(L, -1, otel));
  lua_pop(L, 2);
  lua_pushvalue(L, 2);
  ASSERT_TRUE(PushProcessUpvalue(L, "logs"));
  EXPECT_TRUE(lua_rawequal(L, -1, native));
  lua_settop(L, 0);
}

// Number of lightuserdata-keyed registry entries holding a table (published anchors).
int CountPublishedAnchors(lua_State* L) {
  int n = 0;
  lua_pushnil(L);
  while (lua_next(L, LUA_REGISTRYINDEX) != 0) {
    if (lua_type(L, -2) == LUA_TLIGHTUSERDATA && lua_type(L, -1) == LUA_TTABLE) {
      ++n;
    }
    lua_pop(L, 1);
  }
  return n;
}

TEST(ModuleStateTest, BootstrapOomPublishesNothingAndALaterLoadSucceeds) {
  bool saw_failure = false;
  bool saw_success = false;
  for (std::size_t skip = 0; skip < 400 && !saw_success; ++skip) {
    SCOPED_TRACE("denied growth request " + std::to_string(skip));
    RawState s;
    lua_State* L = s.L();
    const int baseline = CountPublishedAnchors(L);
    // two consecutive denials defeat Lua's emergency-GC retry of the same request
    s.alloc().SetPlan(LuaFaultMode::kWindow, skip, 2);
    const int top = lua_gettop(L);
    const bool ok = EnsureModuleState(L, ModuleKind::Otel);
    const std::size_t denied = s.alloc().stats().denied;
    s.alloc().ClearPlan();
    if (ok) {
      EXPECT_EQ(lua_gettop(L), top + 1);
      EXPECT_EQ(lua_type(L, -1), LUA_TTABLE);
      lua_pop(L, 1);
      saw_success = denied == 0;
      continue;
    }
    saw_failure = true;
    ASSERT_GT(denied, 0u);
    ASSERT_EQ(lua_gettop(L), top + 1) << "a protected Lua error object must be left on top";
    lua_pop(L, 1);
    EXPECT_EQ(CountPublishedAnchors(L), baseline) << "half-built anchor was published";
    ASSERT_TRUE(EnsureModuleState(L, ModuleKind::Otel));
    EXPECT_EQ(lua_type(L, -1), LUA_TTABLE);
    EXPECT_EQ(CountPublishedAnchors(L), baseline + 1);
    lua_pop(L, 1);
  }
  EXPECT_TRUE(saw_failure);
  EXPECT_TRUE(saw_success);
}

// ---- protected leaves ---------------------------------------------------------------------------

int RaiseTableLeaf(lua_State* L) {
  lua_newtable(L);
  return lua_error(L);
}
int RaiseNumberLeaf(lua_State* L) {
  lua_pushinteger(L, 42);
  return lua_error(L);
}
int RaiseStringLeaf(lua_State* L) {
  lua_pushliteral(L, "boom from leaf");
  return lua_error(L);
}
int OkLeaf(lua_State*) { return 0; }

void NonStringErrorBody(OperationContext& ctx, Scenario& sc) {
  sc.leaf_ok = RunLuaLeaf(ctx, RaiseTableLeaf, nullptr);
  sc.code = ctx.error.code;
  CopyMessage(sc, ctx);
  sc.pending_type = AnchorType(ctx.L, ctx.anchor_index, AnchorSlot::kPendingError);
  // Cleanup must keep the diagnostic object rooted until the result tuple has been built.
  ClearInvocationRoots(ctx);
  sc.pending_after_clear = AnchorType(ctx.L, ctx.anchor_index, AnchorSlot::kPendingError);
}

TEST(ProtectedLuaTest, NonStringErrorObjectIsAnchoredAndReportedGenerically) {
  RawState s;
  Scenario sc;
  sc.body = NonStringErrorBody;
  const Observed o = RunScenario(s, sc, [] {});
  ASSERT_EQ(o.status, LUA_OK);
  EXPECT_FALSE(sc.leaf_ok);
  EXPECT_EQ(sc.code, ErrorCode::Lua);
  EXPECT_STREQ(sc.message, "Lua operation failed");
  EXPECT_EQ(sc.pending_type, LUA_TTABLE) << "the error object must be rooted before restoration";
  EXPECT_EQ(sc.pending_after_clear, LUA_TTABLE) << "ClearInvocationRoots must keep it rooted";
  // ReturnOperationResult drops the final slot once the result values are on the stack.
  EXPECT_EQ(AnchorSlotType(s.L(), ModuleKind::Otel, AnchorSlot::kPendingError), LUA_TBOOLEAN);
}

void NumberErrorBody(OperationContext& ctx, Scenario& sc) {
  sc.leaf_ok = RunLuaLeaf(ctx, RaiseNumberLeaf, nullptr);
  sc.code = ctx.error.code;
  CopyMessage(sc, ctx);
  sc.pending_type = AnchorType(ctx.L, ctx.anchor_index, AnchorSlot::kPendingError);
}

TEST(ProtectedLuaTest, NumericErrorObjectIsNotCoercedToAString) {
  RawState s;
  Scenario sc;
  sc.body = NumberErrorBody;
  ASSERT_EQ(RunScenario(s, sc, [] {}).status, LUA_OK);
  EXPECT_FALSE(sc.leaf_ok);
  EXPECT_EQ(sc.code, ErrorCode::Lua);
  EXPECT_STREQ(sc.message, "Lua operation failed");
  EXPECT_EQ(sc.pending_type, LUA_TNUMBER);
}

void StringErrorBody(OperationContext& ctx, Scenario& sc) {
  sc.leaf_ok = RunLuaLeaf(ctx, RaiseStringLeaf, nullptr);
  sc.code = ctx.error.code;
  CopyMessage(sc, ctx);
}

TEST(ProtectedLuaTest, StringErrorTextIsCopiedIntoTheFixedDiagnostic) {
  RawState s;
  Scenario sc;
  sc.body = StringErrorBody;
  const Observed o = RunScenario(s, sc, [] {});
  ASSERT_EQ(o.status, LUA_OK);
  EXPECT_FALSE(sc.leaf_ok);
  EXPECT_EQ(sc.code, ErrorCode::Lua);
  EXPECT_STREQ(sc.message, "boom from leaf");
  EXPECT_EQ(o.text, "boom from leaf");
}

void OkBody(OperationContext& ctx, Scenario& sc) {
  sc.leaf_ok = RunLuaLeaf(ctx, OkLeaf, nullptr);
  sc.code = ctx.error.code;
}

TEST(ProtectedLuaTest, SuccessfulLeafLeavesNoErrorAndTheStackBalanced) {
  RawState s;
  Scenario sc;
  sc.body = OkBody;
  const Observed o = RunScenario(s, sc, [] {});
  ASSERT_EQ(o.status, LUA_OK);
  EXPECT_TRUE(sc.leaf_ok);
  EXPECT_EQ(sc.code, ErrorCode::None);
  EXPECT_EQ(o.nresults, 2);
}

void StackExhaustedBody(OperationContext& ctx, Scenario& sc) {
  lua_State* L = ctx.L;
  // Deny every allocation, then consume the free slots of the current frame. A refused
  // lua_checkstack is a failed stack reallocation, which behaves identically across Lua 5.4.x
  // releases (unlike probing the hard LUAI_MAXSTACK limit).
  sc.alloc->SetPlan(LuaFaultMode::kPersistent, 0);
  while (lua_checkstack(L, 1)) {
    lua_pushnil(L);
  }
  sc.leaf_ok = RunLuaLeaf(ctx, OkLeaf, nullptr);
  sc.code = ctx.error.code;
  CopyMessage(sc, ctx);
}

TEST(ProtectedLuaTest, DeniedStackGrowthIsReportedWithoutRunningTheLeaf) {
  RawState s;
  Scenario sc;
  sc.alloc = &s.alloc();
  sc.body = StackExhaustedBody;
  const Observed o = RunScenario(s, sc, [] {});
  ASSERT_EQ(o.status, LUA_OK);
  EXPECT_FALSE(sc.leaf_ok);
  EXPECT_EQ(sc.code, ErrorCode::Stack);
  EXPECT_STREQ(sc.message, "out of lua stack space");
  ASSERT_EQ(o.nresults, 3) << "cleanup must restore the stack so the result tuple still fits";
  EXPECT_EQ(o.text, "out of lua stack space");
}

// A leaf that acquires two Lua objects into anchor slots, allocating between them.
int AcquireLeaf(lua_State* L) {
  lua_createtable(L, 0, 4);
  SetAnchor(L, kLeafAnchorIndex, AnchorSlot::kRow);
  lua_createtable(L, 0, 4);
  lua_pushlstring(L, "a moderately long string value to force a heap allocation", 58);
  lua_setfield(L, -2, "k");
  SetAnchor(L, kLeafAnchorIndex, AnchorSlot::kResourceAttrs);
  return 0;
}

void AcquireBody(OperationContext& ctx, Scenario& sc) {
  sc.leaf_ok = RunLuaLeaf(ctx, AcquireLeaf, nullptr);
  sc.code = ctx.error.code;
  sc.row_type_at_failure = AnchorType(ctx.L, ctx.anchor_index, AnchorSlot::kRow);
  sc.attrs_type_at_failure = AnchorType(ctx.L, ctx.anchor_index, AnchorSlot::kResourceAttrs);
}

TEST(FailureSafetyTest, PersistentOomAfterAcquisitionRestoresTheSameState) {
  RawState s;
  lua_State* L = s.L();
  // Warm up: a successful run, then a baseline taken with everything collected.
  Scenario warm;
  warm.body = AcquireBody;
  ASSERT_EQ(RunScenario(s, warm, [] {}).status, LUA_OK);
  ASSERT_TRUE(warm.leaf_ok);
  FullCollect(L);
  const std::size_t base_roots = test::CountRegistryRoots(L);
  const std::size_t base_bytes = s.alloc().live_bytes();

  bool failed_after_first_acquisition = false;
  bool eventually_succeeded = false;
  for (std::size_t skip = 0; skip < 64 && !eventually_succeeded; ++skip) {
    SCOPED_TRACE("persistent denial from growth request " + std::to_string(skip));
    Scenario sc;
    sc.body = AcquireBody;
    const Observed o =
        RunScenario(s, sc, [&] { s.alloc().SetPlan(LuaFaultMode::kPersistent, skip); });
    ASSERT_EQ(o.status, LUA_OK) << "the process closure itself must never raise";
    if (sc.leaf_ok) {
      eventually_succeeded = true;
    } else {
      EXPECT_EQ(sc.code, ErrorCode::Memory);
      if (sc.row_type_at_failure == LUA_TTABLE) {
        failed_after_first_acquisition = true;
      }
    }
    // The failure path must have returned the documented tuple with the truthful count.
    if (!sc.leaf_ok) {
      ASSERT_EQ(o.nresults, 3);
      EXPECT_TRUE(o.first_nil);
      EXPECT_EQ(o.last, 0);
    }
    FullCollect(L);
    EXPECT_EQ(test::CountRegistryRoots(L), base_roots);
    EXPECT_EQ(s.alloc().live_bytes(), base_bytes);
    for (int i = 0; i < static_cast<int>(AnchorSlot::kCount); ++i) {
      EXPECT_EQ(AnchorSlotType(L, ModuleKind::Otel, static_cast<AnchorSlot>(i)), LUA_TBOOLEAN)
          << "slot " << i << " still holds an object";
    }
    // Same state, memory available again: the next valid leaf succeeds.
    Scenario next;
    next.body = AcquireBody;
    ASSERT_EQ(RunScenario(s, next, [] {}).status, LUA_OK);
    EXPECT_TRUE(next.leaf_ok);
    FullCollect(L);
    EXPECT_EQ(s.alloc().live_bytes(), base_bytes);
  }
  EXPECT_TRUE(failed_after_first_acquisition)
      << "the sweep never failed after an object was anchored";
  EXPECT_TRUE(eventually_succeeded);
}

}  // namespace

namespace {
struct LoaderCase {
  const char* module;
  const char* name;
  lua_CFunction open;
};
extern "C" int luaopen_vector_otel_metrics(lua_State*);
extern "C" int luaopen_vector_native_logs(lua_State*);
extern "C" int luaopen_vector_native_traces(lua_State*);
const LoaderCase kLoaders[] = {
    {"vector_otel", "vector_otel", luaopen_vector_otel},
    {"vector_native", "vector_native", luaopen_vector_native},
    {"vector_otel", "vector_otel.logs", luaopen_vector_otel_logs},
    {"vector_otel", "vector_otel.traces", luaopen_vector_otel_traces},
    {"vector_otel", "vector_otel.metrics", luaopen_vector_otel_metrics},
    {"vector_native", "vector_native.logs", luaopen_vector_native_logs},
    {"vector_native", "vector_native.traces", luaopen_vector_native_traces},
    {"vector_native", "vector_native.metrics", luaopen_vector_native_metrics},
};

int LoadedTarget(lua_State* L, bool global) {
  if (global) {
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "loaded");
    lua_remove(L, -2);
  } else {
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  }
  return lua_gettop(L);
}

struct RollbackObservation {
  LuaFaultAllocator* allocator = nullptr;
  std::size_t attempts = 0;
  bool allocated = false;
  int calls = 0;
  int partial = 0;
};
thread_local RollbackObservation* rollback_observation = nullptr;
void ObserveRollback(lua_State* L, int journal, bool before) {
  auto& observation = *rollback_observation;
  if (!before) {
    observation.allocated |= observation.allocator->stats().attempts != observation.attempts;
    return;
  }
  ++observation.calls;
  observation.attempts = observation.allocator->stats().attempts;
  // Fixed journal layout is internal to this fault-target instrumentation.
  lua_rawgeti(L, journal, 6);
  const int count = static_cast<int>(lua_tointeger(L, -1));
  lua_pop(L, 1);
  for (int i = 0; i < count; ++i) {
    const int slot = 7 + i * 4;
    lua_rawgeti(L, journal, slot);
    lua_rawgeti(L, journal, slot + 1);
    lua_rawget(L, -2);
    lua_rawgeti(L, journal, slot + 2);
    if (!lua_rawequal(L, -1, -2)) ++observation.partial;
    lua_pop(L, 3);
  }
}

TEST(ModuleRegistration, AllLoadersRollbackAndRetryUnderPersistentAndWindowOom) {
  for (const auto& loader : kLoaders) {
    for (bool distinct : {false, true}) {
      for (LuaFaultMode mode : {LuaFaultMode::kPersistent, LuaFaultMode::kWindow}) {
        bool success = false;
        int failures = 0;
        int rollback_calls = 0;
        int partial_writes = 0;
        for (std::size_t skip = 0; skip < 256; ++skip) {
          SCOPED_TRACE(std::string(loader.name) + " skip=" + std::to_string(skip) +
                       " distinct=" + std::to_string(distinct));
          RawState state;
          lua_State* L = state.L();
          luaL_openlibs(L);
          if (distinct) {
            lua_getglobal(L, "package");
            lua_newtable(L);
            lua_setfield(L, -2, "loaded");
            lua_pop(L, 1);
          }
          const std::string names[] = {loader.module, std::string(loader.module) + ".logs",
                                       std::string(loader.module) + ".traces",
                                       std::string(loader.module) + ".metrics"};
          int snapshots[2][4]{};
          const void* identities[2]{};
          for (int target = 0; target < 2; ++target) {
            const int table = LoadedTarget(L, target == 1);
            identities[target] = lua_topointer(L, table);
            // Cover absent, false and object old values, not merely existence.
            lua_pushboolean(L, false);
            lua_setfield(L, table, names[1].c_str());
            lua_newtable(L);
            lua_setfield(L, table, names[2].c_str());
            for (int i = 0; i < 4; ++i) {
              lua_getfield(L, table, names[i].c_str());
              snapshots[target][i] = luaL_ref(L, LUA_REGISTRYINDEX);
            }
            lua_pop(L, 1);
          }
          // Aliased targets were seeded twice, so recapture after both seeds.
          for (int target = 0; target < 2; ++target) {
            const int table = LoadedTarget(L, target == 1);
            for (int i = 0; i < 4; ++i) {
              luaL_unref(L, LUA_REGISTRYINDEX, snapshots[target][i]);
              lua_getfield(L, table, names[i].c_str());
              snapshots[target][i] = luaL_ref(L, LUA_REGISTRYINDEX);
            }
            lua_pop(L, 1);
          }
          lua_pushcfunction(L, loader.open);
          RollbackObservation observation;
          observation.allocator = &state.alloc();
          rollback_observation = &observation;
          tls_registration_rollback_observer = ObserveRollback;
          state.alloc().SetPlan(mode, skip, 8);
          const int status = lua_pcall(L, 0, 1, 0);
          const auto stats = state.alloc().stats();
          state.alloc().ClearPlan();
          tls_registration_rollback_observer = nullptr;
          rollback_observation = nullptr;
          EXPECT_FALSE(observation.allocated);
          rollback_calls += observation.calls;
          partial_writes += observation.partial;
          if (status != LUA_OK) {
            ++failures;
            EXPECT_EQ(status, LUA_ERRMEM);
            lua_pop(L, 1);
            for (int target = 0; target < 2; ++target) {
              const int table = LoadedTarget(L, target == 1);
              ASSERT_EQ(lua_topointer(L, table), identities[target]);
              for (int i = 0; i < 4; ++i) {
                lua_getfield(L, table, names[i].c_str());
                if (snapshots[target][i] == LUA_REFNIL)
                  lua_pushnil(L);
                else
                  lua_rawgeti(L, LUA_REGISTRYINDEX, snapshots[target][i]);
                EXPECT_TRUE(lua_rawequal(L, -1, -2)) << names[i];
                lua_pop(L, 2);
              }
              lua_pop(L, 1);
            }
            // Same state, recovered allocator: complete anchor remains reusable.
            lua_pushcfunction(L, loader.open);
            ASSERT_EQ(lua_pcall(L, 0, 1, 0), LUA_OK);
            ASSERT_TRUE(lua_istable(L, -1));
            lua_pop(L, 1);
          } else {
            ASSERT_TRUE(lua_istable(L, -1));
            lua_pop(L, 1);
            success = stats.denied == 0;
          }
          EXPECT_EQ(lua_gettop(L), 0);
          if (success) break;
        }
        EXPECT_GT(failures, 0);
        EXPECT_TRUE(success);
        if (distinct && std::strcmp(loader.name, loader.module) == 0) EXPECT_GT(rollback_calls, 0);
        if (distinct && std::strcmp(loader.name, loader.module) == 0) EXPECT_GT(partial_writes, 0);
      }
    }
  }
}

int RegisterTestBatch(lua_State* L) {
  // Values are already rooted in arguments, no C++ owner crosses a Lua raise.
  const ModuleRegistration entries[] = {{"transaction.first", 1},
                                        {"transaction.second", 2},
                                        {"transaction.third", 3},
                                        {"transaction.fourth", 4}};
  RegisterModulesInPackageLoaded(L, entries, 4);
  return 0;
}

TEST(ModuleRegistration, MissingRegistryAndGlobalPackageCompatibility) {
  for (bool package : {false, true}) {
    bool success = false;
    for (std::size_t skip = 0; skip < 128; ++skip) {
      RawState state;
      lua_State* L = state.L();
      if (package) luaL_openlibs(L);
      lua_pushnil(L);
      lua_setfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
      lua_pushcfunction(L, RegisterTestBatch);
      for (int i = 0; i < 4; ++i) lua_newtable(L);
      state.alloc().SetPlan(LuaFaultMode::kPersistent, skip);
      const int status = lua_pcall(L, 4, 0, 0);
      const auto stats = state.alloc().stats();
      state.alloc().ClearPlan();
      if (status != LUA_OK) {
        lua_pop(L, 1);
        lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
        EXPECT_TRUE(lua_isnil(L, -1));
        lua_pop(L, 1);
        if (package) {
          const int table = LoadedTarget(L, true);
          lua_getfield(L, table, "transaction.first");
          EXPECT_TRUE(lua_isnil(L, -1));
          lua_pop(L, 2);
        }
      } else
        success = stats.denied == 0;
      if (success) break;
    }
    EXPECT_TRUE(success);
  }
}
struct PublicationReturnHook {
  const char* watched_name = nullptr;
  LuaFaultAllocator* allocator = nullptr;
  int error_ref = LUA_NOREF;
  bool deny_rollback_allocations = false;
  bool fired = false;
};
thread_local PublicationReturnHook* publication_return_hook = nullptr;

void FailAfterRegistryPublication(lua_State* L, lua_Debug* debug) {
  auto* hook = publication_return_hook;
  if (!hook || debug->event != LUA_HOOKRET) return;
  lua_pushliteral(L, LUA_LOADED_TABLE);
  lua_rawget(L, LUA_REGISTRYINDEX);
  bool published = false;
  if (lua_istable(L, -1)) {
    lua_pushstring(L, hook->watched_name);
    lua_rawget(L, -2);
    published = lua_istable(L, -1);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  if (!published) return;
  hook->fired = true;
  lua_sethook(L, nullptr, 0, 0);
  // The original error is rooted before enabling allocation denial.
  lua_rawgeti(L, LUA_REGISTRYINDEX, hook->error_ref);
  if (hook->deny_rollback_allocations) hook->allocator->SetPlan(LuaFaultMode::kPersistent, 0);
  lua_error(L);
}

TEST(ModuleRegistration, ReturnHookFailureRestoresRegistrySlotAndAllowsRetry) {
  for (const auto& loader : kLoaders) {
    for (int prior = 0; prior < 4; ++prior) {
      for (bool deny : {false, true}) {
        SCOPED_TRACE(std::string(loader.name) + " prior=" + std::to_string(prior) +
                     " deny=" + std::to_string(deny));
        RawState state;
        lua_State* L = state.L();
        luaL_openlibs(L);
        const std::string names[] = {loader.module, std::string(loader.module) + ".logs",
                                     std::string(loader.module) + ".traces",
                                     std::string(loader.module) + ".metrics"};
        const int public_table = LoadedTarget(L, true);
        const void* public_identity = lua_topointer(L, public_table);
        int snapshots[4]{};
        for (int i = 0; i < 4; ++i) {
          if (std::strcmp(names[i].c_str(), loader.name) == 0)
            lua_pushnil(L);
          else if (i % 2 == 0)
            lua_newtable(L);
          else
            lua_pushboolean(L, false);
          lua_pushvalue(L, -1);
          lua_setfield(L, public_table, names[i].c_str());
          snapshots[i] = luaL_ref(L, LUA_REGISTRYINDEX);
        }
        if (prior == 0)
          lua_pushnil(L);
        else if (prior == 1)
          lua_pushboolean(L, false);
        else if (prior == 2)
          lua_pushliteral(L, "original registry sentinel");
        else
          lua_pushvalue(L, public_table);
        lua_pushvalue(L, -1);
        const int old_registry = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_setfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
        lua_pop(L, 1);
        lua_pushliteral(L, "injected publication return-hook error");
        const int error_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        PublicationReturnHook hook{loader.name, &state.alloc(), error_ref, deny, false};
        RollbackObservation observation;
        observation.allocator = &state.alloc();
        rollback_observation = &observation;
        tls_registration_rollback_observer = ObserveRollback;
        publication_return_hook = &hook;
        lua_pushcfunction(L, loader.open);
        lua_sethook(L, FailAfterRegistryPublication, LUA_MASKRET, 0);
        const int status = lua_pcall(L, 0, 1, 0);
        lua_sethook(L, nullptr, 0, 0);
        const auto stats = state.alloc().stats();
        state.alloc().ClearPlan();
        publication_return_hook = nullptr;
        tls_registration_rollback_observer = nullptr;
        rollback_observation = nullptr;
        ASSERT_TRUE(hook.fired);
        ASSERT_EQ(status, LUA_ERRRUN);
        lua_rawgeti(L, LUA_REGISTRYINDEX, error_ref);
        EXPECT_TRUE(lua_rawequal(L, -1, -2));
        lua_pop(L, 2);
        EXPECT_EQ(observation.calls, 1);
        EXPECT_FALSE(observation.allocated);
        if (deny) EXPECT_EQ(stats.attempts, 0u);
        lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
        if (old_registry == LUA_REFNIL)
          lua_pushnil(L);
        else
          lua_rawgeti(L, LUA_REGISTRYINDEX, old_registry);
        EXPECT_TRUE(lua_rawequal(L, -1, -2));
        lua_pop(L, 2);
        const int loaded = LoadedTarget(L, true);
        EXPECT_EQ(lua_topointer(L, loaded), public_identity);
        for (int i = 0; i < 4; ++i) {
          lua_getfield(L, loaded, names[i].c_str());
          if (snapshots[i] == LUA_REFNIL)
            lua_pushnil(L);
          else
            lua_rawgeti(L, LUA_REGISTRYINDEX, snapshots[i]);
          EXPECT_TRUE(lua_rawequal(L, -1, -2)) << names[i];
          lua_pop(L, 2);
        }
        lua_pop(L, 1);
        EXPECT_EQ(lua_gettop(L), 0);
        lua_pushcfunction(L, loader.open);
        ASSERT_EQ(lua_pcall(L, 0, 1, 0), LUA_OK);
        EXPECT_TRUE(lua_istable(L, -1));
        lua_pop(L, 1);
        EXPECT_EQ(lua_gettop(L), 0);
      }
    }
  }
}

TEST(ModuleRegistration, RequireUsesPublishedTablesWithoutMetamethods) {
  for (const char* module : {"vector_otel", "vector_native"}) {
    RawState state;
    lua_State* L = state.L();
    luaL_openlibs(L);
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "preload");
    lua_pushcfunction(
        L, std::strcmp(module, "vector_otel") == 0 ? luaopen_vector_otel : luaopen_vector_native);
    lua_setfield(L, -2, module);
    lua_pop(L, 2);
    const std::string chunk =
        "local m=require('" + std::string(module) + "'); assert(m==require('" + module +
        "')); assert(m.logs==require('" + module + ".logs')); assert(m.traces==require('" + module +
        ".traces')); assert(m.metrics==require('" + module + ".metrics')); return true";
    ASSERT_EQ(luaL_loadstring(L, chunk.c_str()), LUA_OK);
    ASSERT_EQ(lua_pcall(L, 0, 1, 0), LUA_OK) << lua_tostring(L, -1);
    EXPECT_TRUE(lua_toboolean(L, -1));
  }
  RawState state;
  lua_State* L = state.L();
  luaL_openlibs(L);
  ASSERT_EQ(
      luaL_dostring(
          L, "setmetatable(package.loaded,{__newindex=function() error('metamethod called') end})"),
      LUA_OK);
  lua_pushcfunction(L, luaopen_vector_otel);
  EXPECT_EQ(lua_pcall(L, 0, 1, 0), LUA_OK) << lua_tostring(L, -1);
}

}  // namespace
}  // namespace vector_otel::core
