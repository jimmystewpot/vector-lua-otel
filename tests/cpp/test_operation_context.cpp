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
  static_assert(sizeof(cases) / sizeof(cases[0]) ==
                static_cast<std::size_t>(ImmutableSlot::kCount));
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
}  // namespace vector_otel::core
