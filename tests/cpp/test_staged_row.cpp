#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <lua.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "fault_new.hpp"
#include "operation_fixtures.hpp"
#include "vector_otel/core/staged_row.hpp"

namespace vector_otel::core {
namespace {

using namespace fixtures;
using test::CppFaultMode;
using test::CppFaultScope;

// ---- recording emit callbacks (plain C functions: the raw state has no standard library) --------

void Dump(lua_State* L, int idx, std::string& out) {
  idx = lua_absindex(L, idx);
  switch (lua_type(L, idx)) {
    case LUA_TSTRING: {
      std::size_t n = 0;
      const char* s = lua_tolstring(L, idx, &n);
      out += '"';
      out.append(s, n);
      out += '"';
      break;
    }
    case LUA_TNUMBER:
      if (lua_isinteger(L, idx)) {
        out += std::to_string(lua_tointeger(L, idx));
      } else {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%.17gf", lua_tonumber(L, idx));
        out += buf;
      }
      break;
    case LUA_TBOOLEAN:
      out += lua_toboolean(L, idx) ? "true" : "false";
      break;
    case LUA_TTABLE: {
      const lua_Unsigned n = lua_rawlen(L, idx);
      if (n > 0) {
        out += '[';
        for (lua_Unsigned i = 1; i <= n; ++i) {
          if (i > 1) out += ',';
          lua_rawgeti(L, idx, static_cast<lua_Integer>(i));
          Dump(L, -1, out);
          lua_pop(L, 1);
        }
        out += ']';
        break;
      }
      std::vector<std::string> fields;
      lua_pushnil(L);
      while (lua_next(L, idx) != 0) {
        std::string field;
        Dump(L, -2, field);
        field += ':';
        Dump(L, -1, field);
        fields.push_back(std::move(field));
        lua_pop(L, 1);
      }
      std::sort(fields.begin(), fields.end());
      out += '{';
      for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) out += ',';
        out += fields[i];
      }
      out += '}';
      break;
    }
    default:
      out += "?";
  }
}

struct Recorder {
  std::vector<std::string> events;
  std::vector<const void*> resource_ptrs;
  int rows_delivered = 0;
  int calls = 0;
  int fail_at_call = -1;  // zero-based call index that fails
  int fail_mode = 0;      // 1: raise string before recording, 2: record then raise, 3: raise table
  void Reset() { *this = Recorder{}; }
};
Recorder g_rec;

int RecordEmit(lua_State* L) {
  const int call = g_rec.calls++;
  if (call == g_rec.fail_at_call && g_rec.fail_mode == 1) {
    lua_pushliteral(L, "boom");
    return lua_error(L);
  }
  std::string text;
  Dump(L, 1, text);
  g_rec.events.push_back(text);
  lua_pushstring(L, "log");
  if (lua_rawget(L, 1) == LUA_TTABLE) {
    lua_pushstring(L, "resource");
    if (lua_rawget(L, -2) == LUA_TTABLE) {
      g_rec.resource_ptrs.push_back(lua_topointer(L, -1));
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  g_rec.rows_delivered += 1;
  if (call == g_rec.fail_at_call && g_rec.fail_mode == 2) {
    lua_pushliteral(L, "boom after side effect");
    return lua_error(L);
  }
  if (call == g_rec.fail_at_call && g_rec.fail_mode == 3) {
    lua_newtable(L);
    return lua_error(L);
  }
  return 0;
}

// ---- row factory --------------------------------------------------------------------------------

StagedRow& ThreadRow() {
  static thread_local StagedRow row;
  return row;
}

void StageSample(StagedRow& row, int i) {
  row.Reset();
  const std::string id = "id-" + std::to_string(i);
  row.Str("id", id);
  row.Int("n", i);
  row.Dbl("d", 1.5);
  row.Bool("ok", true);
  row.BeginMap("attrs");
  row.Str("k", "v");
  row.BeginArray("list");
  row.Int({}, 1);
  row.Str({}, "x");
  row.End();
  row.End();
}

std::string SampleDump(int i) {
  return "{\"attrs\":{\"k\":\"v\",\"list\":[1,\"x\"]},\"d\":1.5f,\"id\":\"id-" + std::to_string(i) +
         "\",\"n\":" + std::to_string(i) + ",\"ok\":true}";
}

std::string Wrapped(const std::string& row) { return "{\"log\":" + row + "}"; }

void InstallEmit(OperationContext& ctx) {
  lua_pushcfunction(ctx.L, RecordEmit);
  SetAnchor(ctx.L, ctx.anchor_index, AnchorSlot::kEmit);
}

void EmitRowsBody(OperationContext& ctx, Scenario& sc) {
  InstallEmit(ctx);
  StagedRow& row = ThreadRow();
  for (int i = 0; i < sc.rows; ++i) {
    StageSample(row, i);
    if (!EmitStagedRow(ctx, row)) {
      sc.leaf_ok = false;
      return;
    }
    if (AnchorType(ctx.L, ctx.anchor_index, AnchorSlot::kEvent) != LUA_TBOOLEAN) {
      sc.leaf_ok = false;
      return;
    }
  }
  sc.leaf_ok = true;
}

void ExpectAnchorsClear(lua_State* L, ModuleKind kind) {
  for (int i = 0; i < kInvocationSlotCount; ++i) {
    EXPECT_EQ(AnchorSlotType(L, kind, static_cast<AnchorSlot>(i)), LUA_TBOOLEAN)
        << "anchor slot " << i << " still holds a value";
  }
}

Observed RunRows(RawState& s, int rows, int fail_at = -1, int fail_mode = 0) {
  g_rec.Reset();
  g_rec.fail_at_call = fail_at;
  g_rec.fail_mode = fail_mode;
  Scenario sc;
  sc.body = EmitRowsBody;
  sc.rows = rows;
  return RunScenario(s, sc, [] {});
}

// ---- builder (no Lua) ---------------------------------------------------------------------------

TEST(StagedRowBuilderTest, CopiesStringsAndRetainsCapacityAcrossRows) {
  StagedRow row;
  char scratch[16];
  std::memcpy(scratch, "service-a", 10);
  row.Reset();
  row.Str("name", scratch);
  row.Str(Key::Dynamic(scratch), "dyn");
  std::memset(scratch, 'Z', 9);
  EXPECT_EQ(scratch[0], 'Z');  // caller buffer really was overwritten after staging
  ASSERT_EQ(row.node_count(), 2u);
  const auto* nodes = row.nodes();
  EXPECT_EQ(std::string(row.arena() + nodes[0].str_off, nodes[0].str_len), "service-a");
  EXPECT_EQ(std::string(row.arena() + nodes[1].key_off, nodes[1].key_len), "service-a");

  // second, identical-shape row after Reset(): no C++ allocation at all
  for (int warm = 0; warm < 2; ++warm) {
    StageSample(row, 1);
  }
  CppFaultScope scope(CppFaultMode::kOneShot, std::numeric_limits<std::size_t>::max());
  StageSample(row, 2);
  StageSample(row, 3);
  EXPECT_EQ(scope.stats().attempts, 0u) << "steady-state staging allocated";
}

TEST(StagedRowBuilderTest, ResetClearsTheRowAndReleasesOversizedBuffers) {
  StagedRow row;
  StageSample(row, 1);
  ASSERT_GT(row.node_count(), 0u);
  ASSERT_GT(row.arena_size(), 0u);
  row.Reset();
  EXPECT_EQ(row.node_count(), 0u);
  EXPECT_EQ(row.arena_size(), 0u);
  EXPECT_EQ(row.root_children(), 0u);
  EXPECT_EQ(row.depth(), 0);
  EXPECT_GT(row.node_capacity(), 0u) << "normal rows keep their capacity";

  const std::string huge(StagedRow::kRetainedArenaBytes + 1, 'h');
  row.Str("huge", huge);
  ASSERT_GT(row.arena_capacity(), StagedRow::kRetainedArenaBytes);
  row.Reset();
  EXPECT_LE(row.arena_capacity(), StagedRow::kRetainedArenaBytes) << "huge buffer was retained";
}

TEST(StagedRowBuilderTest, DepthAndBalanceAreEnforced) {
  StagedRow row;
  row.Reset();
  EXPECT_THROW(row.End(), std::logic_error);
  for (int i = 0; i < StagedRow::kMaxDepth; ++i) {
    row.BeginMap("m");
  }
  EXPECT_EQ(row.depth(), StagedRow::kMaxDepth);
  EXPECT_FALSE(row.Complete());
  EXPECT_THROW(row.BeginMap("too-deep"), std::length_error);
  for (int i = 0; i < StagedRow::kMaxDepth; ++i) {
    row.End();
  }
  EXPECT_TRUE(row.Complete());
}

// ---- protected materialisation and emission
// -------------------------------------------------------

TEST(StagedRowEmitTest, SingleRowEventHasExactShape) {
  RawState s;
  const Observed o = RunRows(s, 2, 1);
  ASSERT_EQ(o.status, LUA_OK);
  ASSERT_EQ(o.nresults, 2);
  EXPECT_FALSE(o.first_nil);
  EXPECT_TRUE(o.second_nil);
  ASSERT_EQ(g_rec.events.size(), 2u);
  EXPECT_EQ(g_rec.events[0], Wrapped(SampleDump(0)));
  EXPECT_EQ(g_rec.events[1], Wrapped(SampleDump(1)));
  ExpectAnchorsClear(s.L(), ModuleKind::Otel);
}

void ScratchBody(OperationContext& ctx, Scenario& sc) {
  InstallEmit(ctx);
  StagedRow& row = ThreadRow();
  row.Reset();
  char id[8];
  char key[8];
  std::memcpy(id, "ABCDEFG", 8);
  std::memcpy(key, "dynkey", 7);
  row.Str("id", std::string_view(id, 7));
  row.Str(Key::Dynamic(std::string_view(key, 6)), std::string_view(id, 3));
  row.Int("max", std::numeric_limits<std::int64_t>::max());
  row.Int("min", std::numeric_limits<std::int64_t>::min());
  row.Uint("clamped", std::numeric_limits<std::uint64_t>::max());
  row.Uint("exact", 9007199254740993ull);  // 2^53 + 1, not representable as double
  std::memset(id, 'Z', 7);
  std::memset(key, 'Y', 6);
  const std::string big(200000, 'q');  // forces the arena to grow after earlier strings were staged
  row.Str("big", big);
  lua_gc(ctx.L, LUA_GCCOLLECT);
  sc.leaf_ok = EmitStagedRow(ctx, row) && FlushStagedRows(ctx);
}

TEST(StagedRowEmitTest, ScratchAndStackStringsSurviveMaterialization) {
  RawState s;
  g_rec.Reset();
  Scenario sc;
  sc.body = ScratchBody;
  const Observed o = RunScenario(s, sc, [] {});
  ASSERT_EQ(o.status, LUA_OK);
  ASSERT_EQ(o.nresults, 2);
  ASSERT_TRUE(sc.leaf_ok);
  ASSERT_EQ(g_rec.events.size(), 1u);
  const std::string expected_row = "{\"big\":\"" + std::string(200000, 'q') +
                                   "\",\"clamped\":9223372036854775807,\"dynkey\":\"ABC\","
                                   "\"exact\":9007199254740993,\"id\":\"ABCDEFG\","
                                   "\"max\":9223372036854775807,\"min\":-9223372036854775808}";
  EXPECT_EQ(g_rec.events[0], Wrapped(expected_row));
}

void SharedAttrsBody(OperationContext& ctx, Scenario& sc) {
  InstallEmit(ctx);
  StagedRow tape;
  tape.Str("service.name", "svc");
  tape.Int("pid", 7);
  if (!MaterializeToAnchor(ctx, tape, AnchorSlot::kResourceAttrs)) return;
  StagedRow& row = ThreadRow();
  for (int i = 0; i < 2; ++i) {
    row.Reset();
    row.Int("n", i);
    row.Anchor("resource", AnchorSlot::kResourceAttrs);
    if (!EmitStagedRow(ctx, row)) return;
  }
  sc.leaf_ok = true;
}

TEST(StagedRowEmitTest, AnchoredTablesAreSharedAcrossRows) {
  RawState s;
  g_rec.Reset();
  Scenario sc;
  sc.body = SharedAttrsBody;
  const Observed o = RunScenario(s, sc, [] {});
  ASSERT_EQ(o.status, LUA_OK);
  ASSERT_TRUE(sc.leaf_ok);
  ASSERT_EQ(g_rec.events.size(), 2u);
  EXPECT_EQ(g_rec.events[0],
            "{\"log\":{\"n\":0,\"resource\":{\"pid\":7,\"service.name\":\"svc\"}}}");
  EXPECT_EQ(g_rec.events[1],
            "{\"log\":{\"n\":1,\"resource\":{\"pid\":7,\"service.name\":\"svc\"}}}");
  ASSERT_EQ(g_rec.resource_ptrs.size(), 2u);
  EXPECT_EQ(g_rec.resource_ptrs[0], g_rec.resource_ptrs[1]) << "resource table was copied per row";
  ExpectAnchorsClear(s.L(), ModuleKind::Otel);
}

// ---- delivered counts
// -----------------------------------------------------------------------------

TEST(StagedRowCountTest, CompletedCallbacksAdvanceCountBeforeTheNextFailure) {
  // Calls 0 and 1 succeed, call 2 raises -> (nil, "boom", 2)
  RawState s;
  const Observed o = RunRows(s, 5, /*fail_at=*/2, /*fail_mode=*/1);
  ASSERT_EQ(o.status, LUA_OK);
  ASSERT_EQ(o.nresults, 3);
  EXPECT_TRUE(o.first_nil);
  EXPECT_EQ(o.text, "boom");
  EXPECT_EQ(o.last, 2);
  EXPECT_EQ(g_rec.events.size(), 2u);
  ExpectAnchorsClear(s.L(), ModuleKind::Otel);
}

TEST(StagedRowCountTest, ACallbackThatFailsAfterItsSideEffectIsNotCounted) {
  RawState s;
  const Observed o = RunRows(s, 1, /*fail_at=*/0, /*fail_mode=*/2);
  ASSERT_EQ(o.status, LUA_OK);
  ASSERT_EQ(o.nresults, 3);
  EXPECT_TRUE(o.first_nil);
  EXPECT_EQ(o.text, "boom after side effect");
  EXPECT_EQ(o.last, 0);
  EXPECT_EQ(g_rec.rows_delivered, 1) << "the side effect did happen";
}

TEST(StagedRowCountTest, NonStringCallbackErrorIsReportedGenerically) {
  RawState s;
  const Observed o = RunRows(s, 1, /*fail_at=*/0, /*fail_mode=*/3);
  ASSERT_EQ(o.status, LUA_OK);
  ASSERT_EQ(o.nresults, 3);
  EXPECT_TRUE(o.first_nil);
  EXPECT_EQ(o.text, "Lua operation failed");
  EXPECT_EQ(o.last, 0);
  ExpectAnchorsClear(s.L(), ModuleKind::Otel);
}

// ---- allocator sweeps
// -----------------------------------------------------------------------------

// Every growth request index is denied persistently from that point on. Whatever happens, the
// invariants must hold: no Lua error escapes the closure, the returned count equals the rows that
// reached a callback that returned, anchors are released, and the same state is fully usable after
// a recovery GC.
void SweepEmitAllocations(int rows) {
  bool saw_failure = false;
  bool saw_success = false;
  for (std::size_t skip = 0; skip < 600 && !saw_success; ++skip) {
    SCOPED_TRACE("persistent denial from growth request " + std::to_string(skip));
    RawState s;
    // Warm the state so only per-call allocations are swept.
    ASSERT_TRUE(EnsureModuleState(s.L(), ModuleKind::Otel));
    lua_pop(s.L(), 1);
    g_rec.Reset();
    Scenario sc;
    sc.body = EmitRowsBody;
    sc.rows = rows;
    const Observed o =
        RunScenario(s, sc, [&] { s.alloc().SetPlan(LuaFaultMode::kPersistent, skip); });
    ASSERT_EQ(o.status, LUA_OK) << "an error escaped the dispatcher frame";
    ExpectAnchorsClear(s.L(), ModuleKind::Otel);
    if (o.nresults == 3) {
      saw_failure = true;
      EXPECT_TRUE(o.first_nil);
      EXPECT_EQ(o.last, g_rec.rows_delivered) << "count must equal rows in completed callbacks";
    } else {
      ASSERT_EQ(o.nresults, 2);
      EXPECT_EQ(g_rec.rows_delivered, rows);
      saw_success = true;
    }
    // recovery: collect, then the same state serves a full, correct call
    lua_gc(s.L(), LUA_GCCOLLECT);
    const Observed again = RunRows(s, rows);
    ASSERT_EQ(again.status, LUA_OK);
    ASSERT_EQ(again.nresults, 2);
    EXPECT_EQ(g_rec.rows_delivered, rows);
  }
  EXPECT_TRUE(saw_failure);
  EXPECT_TRUE(saw_success);
}

TEST(StagedRowSweepTest, SingleRowEmissionSurvivesDenialAtEveryAllocation) {
  SweepEmitAllocations(/*rows=*/3);
}

}  // namespace
}  // namespace vector_otel::core
