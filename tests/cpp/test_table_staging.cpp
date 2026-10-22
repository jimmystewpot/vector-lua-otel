// Table-mode reads and attribute staging (fault target only).
//
// Table input is read in the dispatcher's outer frame, where a Lua error would skip C++
// destructors. These tests pin the two properties that make that safe: the readers never allocate
// from the Lua heap (so they cannot raise), and the staged attributes are exactly what the legacy
// Lua-table builder produced.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <lua.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "operation_fixtures.hpp"
#include "vector_otel/core/fast_json_writer.hpp"
#include "vector_otel/core/read_keys.hpp"
#include "vector_otel/core/staged_row.hpp"
#include "vector_otel/core/table_reader.hpp"

namespace vector_otel::core {
namespace {

using namespace fixtures;

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
      std::vector<std::string> fields;
      lua_pushnil(L);
      while (lua_next(L, idx) != 0) {
        std::string field;
        // keys are dumped by type so that 1 and "1" differ
        if (lua_type(L, -2) == LUA_TNUMBER) {
          field += '#';
        }
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

bool LoadChunk(lua_State* L, Scenario& sc) {
  luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
  lua_pop(L, 1);
  if (luaL_loadstring(L, sc.chunk) != LUA_OK || lua_pcall(L, 0, 1, 0) != LUA_OK) {
    sc.fixture_error = true;
    return false;
  }
  return true;
}

struct Results {
  std::string legacy;
  std::string staged;
  std::string legacy_service;
  std::string staged_service;
  bool staged_nothing = false;
  std::size_t denied = 0;
  bool stack_restored = false;
  std::vector<std::string> texts;  // ScalarText vs luaL_tolstring
  std::vector<std::string> expected_texts;
  bool original_unchanged = true;
};
Results g;

// Stages `attrs` and returns the staged map dumped as the legacy builder would have produced it.
void DifferentialBody(OperationContext& ctx, Scenario& sc) {
  lua_State* L = ctx.L;
  if (!LoadChunk(L, sc)) {
    return;
  }
  const int idx = lua_gettop(L);
  FastJsonWriter writer;

  std::string_view legacy_service;
  PushTableAttributes(L, idx, writer, &legacy_service);
  Dump(L, -1, g.legacy);
  g.legacy_service = std::string(legacy_service);
  lua_pop(L, 1);

  StagedRow row;
  row.Reset();
  std::string_view staged_service;
  const int before = lua_gettop(L);
  StageTableAttributes(row, "attrs", L, idx, writer, &staged_service);
  g.stack_restored = lua_gettop(L) == before;
  g.staged_service = std::string(staged_service);
  g.staged_nothing = row.root_children() == 0;
  if (!g.staged_nothing) {
    sc.leaf_ok = MaterializeToAnchor(ctx, row, AnchorSlot::kResourceAttrs);
    PushAnchor(L, ctx.anchor_index, AnchorSlot::kResourceAttrs);
    lua_pushliteral(L, "attrs");
    lua_rawget(L, -2);
    Dump(L, -1, g.staged);
    lua_pop(L, 2);
  }
}

void RunDifferential(const std::string& chunk, bool expect_table = true) {
  SCOPED_TRACE(chunk);
  RawState s;
  g = Results{};
  Scenario sc;
  sc.body = DifferentialBody;
  sc.chunk = chunk.c_str();
  const Observed o = RunScenario(s, sc, [] {});
  ASSERT_EQ(o.status, LUA_OK);
  ASSERT_FALSE(sc.fixture_error);
  EXPECT_TRUE(g.stack_restored);
  EXPECT_EQ(g.staged_service, g.legacy_service);
  if (!expect_table) {
    EXPECT_TRUE(g.staged_nothing);
    return;
  }
  ASSERT_FALSE(g.staged_nothing);
  EXPECT_TRUE(sc.leaf_ok);
  EXPECT_EQ(g.staged, g.legacy);
}

const char* const kEquivalentInputs[] = {
    // key/value arrays with plain scalars
    "return {{key='a',value='x'},{key='b',value=5},{key='c',value=true},{key='d',value=1.5}}",
    // AnyValue wrappers in both spellings
    "return {{key='s',value={stringValue='v'}},{key='s2',value={string_value='w'}},"
    "{key='i',value={intValue=7}},{key='i2',value={int_value=7.9}},{key='i3',value={intValue='12'}}"
    ","
    "{key='u',value={uintValue='18446744073709551615'}},{key='u2',value={uint_value=42}},"
    "{key='b',value={boolValue=false}},{key='b2',value={bool_value=1}},"
    "{key='d',value={doubleValue=2.5}},{key='d2',value={double_value='3.5'}},"
    "{key='n',value={stringValue=5}}}",
    // complex values become JSON strings
    "return {{key='arr',value={arrayValue={values={{stringValue='x'},{intValue=2}}}}},"
    "{key='kv',value={kvlistValue={values={{key='k',value={stringValue='v'}}}}}},"
    "{key='raw',value={1,2,3}}}",
    // malformed entries are skipped
    "return {{key='ok',value='v'},{key='',value='empty-key'},{key='no-value'},{value='no-key'},"
    "'not-a-table',{key=5,value='numeric-key'},{key='fn',value=print}}",
    // duplicate keys: the later entry wins, exactly like a Lua table assignment
    "return {{key='k',value='first'},{key='k',value='second'}}",
    // direct maps
    "return {a='x',b=1,c=true,d=2.5,e={stringValue='z'},f={intValue=3},g={1,2}}",
    // numeric keys are stringified the way Lua's tostring does it
    "return {'x','y',k='v'}",
    "return {[1.5]='f',[-3]='neg',[2^40]='big'}",
    // unsupported value types are dropped
    "return {a='x',fn=print,t=setmetatable({}, {})}",
    // service.name capture
    "return {{key='service.name',value='svc-a'}}",
    "return {{key='service_name',value={stringValue='svc-b'}}}",
    "return {{key='service.name',value=''},{key='service_name',value='fallback'}}",
    "return {['service.name']='dot',service_name='underscore'}",
    "return {service_name='underscore',['service.name']='dot'}",
    "return {a=1,service_name='underscore',b=2,['service.name']='dot',c=3}",
    "return {['service.name']='dot',x=1,y=2,service_name='underscore',z=3}",
    "return {['service.name']='',service_name='underscore'}",
    "return {{key='service.name',value='first'},{key='service_name',value='second'}}",
    "return {{key='service.name',value='unknown_service'},{key='service_name',value='real'}}",
    "return {{key='service_name',value='first'},{key='service.name',value='second'}}",
    "return {service_name='only-underscore'}",
    "return {['service.name']=5}",
    // empty and unusual tables
    "return {}",
    "return {{}}",
    "return {{key=1}}",
};

TEST(TableStagingTest, StagedAttributesEqualTheLegacyLuaTable) {
  for (const char* chunk : kEquivalentInputs) {
    RunDifferential(chunk);
  }
}

// PushTableAttributes() leaves an empty table for a non-table; so does the staged map.
TEST(TableStagingTest, NonTableAttributesStageAnEmptyMapLikeTheLegacyTable) {
  RunDifferential("return 'text'");
  RunDifferential("return 5");
  RunDifferential("return true");
}

void FieldsBody(OperationContext& ctx, Scenario& sc) {
  lua_State* L = ctx.L;
  if (!LoadChunk(L, sc)) {
    return;
  }
  const int idx = lua_gettop(L);
  FastJsonWriter writer;
  StagedRow tape;
  tape.Reset();
  StageTableAttributeFields(tape, L, idx, writer);
  sc.leaf_ok = MaterializeToAnchor(ctx, tape, AnchorSlot::kScopeAttrs);
  PushAnchor(L, ctx.anchor_index, AnchorSlot::kScopeAttrs);
  Dump(L, -1, g.staged);
  lua_pop(L, 1);
}

// Fields staged at the root become the attributes table itself (how shared resource/scope
// attribute tables are built once per resource/scope).
TEST(TableStagingTest, RootLevelFieldsMaterializeAsTheAttributesTable) {
  RawState s;
  g = Results{};
  Scenario sc;
  sc.body = FieldsBody;
  sc.chunk = "return {{key='a',value='x'},{key='n',value={intValue=3}}}";
  const Observed o = RunScenario(s, sc, [] {});
  ASSERT_EQ(o.status, LUA_OK);
  EXPECT_TRUE(sc.leaf_ok);
  EXPECT_EQ(g.staged, "{\"a\":\"x\",\"n\":3}");
}

// ---- reads never touch the Lua allocator ------------------------------------------------------

void DeniedBody(OperationContext& ctx, Scenario& sc) {
  lua_State* L = ctx.L;
  if (!LoadChunk(L, sc)) {
    return;
  }
  const int idx = lua_gettop(L);
  FastJsonWriter writer;  // C++ memory only
  StagedRow row;
  row.Reset();
  std::string_view service;
  // From here on every Lua allocation fails; a read that allocated would raise and unwind the
  // pcall of the scenario, which the test would report as a non-OK status.
  sc.alloc->SetPlan(LuaFaultMode::kPersistent, 0);
  StageTableAttributes(row, "attrs", L, idx, writer, &service);
  // The table readers used by the extractors, on keys that are interned at bootstrap.
  (void)ReadTableString(L, idx, "key", "value");
  (void)ReadTableInt64(L, idx, "intValue", "int_value");
  (void)ReadTableUint64(L, idx, "uintValue", "uint_value");
  uint64_t ts = 0;
  (void)ReadTableTimestampField(L, idx, "timeUnixNano", "time_unix_nano", ts);
  (void)GetTableFieldRaw(L, idx, "attributes");
  lua_settop(L, idx);
  sc.denied = sc.alloc->stats().denied;
  sc.number = row.node_count();
  sc.flag = true;
}

TEST(TableStagingTest, ReadsAndStagingNeverAllocateFromTheLuaHeap) {
  for (const char* chunk : kEquivalentInputs) {
    SCOPED_TRACE(chunk);
    RawState s;
    Scenario sc;
    sc.body = DeniedBody;
    sc.chunk = chunk;
    sc.alloc = &s.alloc();
    const Observed o = RunScenario(s, sc, [] {});
    s.alloc().ClearPlan();
    ASSERT_EQ(o.status, LUA_OK) << "a read raised under a Lua allocation failure";
    ASSERT_FALSE(sc.fixture_error);
    EXPECT_TRUE(sc.flag);
    EXPECT_EQ(sc.denied, 0u) << "a reader allocated from the Lua heap";
  }
}

// ---- the interned key set ---------------------------------------------------------------------

void KeysBody(OperationContext& ctx, Scenario& sc) {
  lua_State* L = ctx.L;
  sc.alloc->SetPlan(LuaFaultMode::kPersistent, 0);
  for (std::string_view key : kReadKeyList) {
    lua_pushlstring(L, key.data(), key.size());  // would raise if the string were not interned
    lua_pop(L, 1);
  }
  sc.denied = sc.alloc->stats().denied;
  sc.number = sizeof(kReadKeyList) / sizeof(kReadKeyList[0]);
  sc.flag = true;
}

TEST(TableStagingTest, EveryListedReadKeyIsInternedAtBootstrap) {
  RawState s;
  Scenario sc;
  sc.body = KeysBody;
  sc.alloc = &s.alloc();
  const Observed o = RunScenario(s, sc, [] {});
  s.alloc().ClearPlan();
  ASSERT_EQ(o.status, LUA_OK) << "pushing a listed key allocated";
  EXPECT_TRUE(sc.flag);
  EXPECT_GT(sc.number, 100u);
  EXPECT_EQ(sc.denied, 0u);
}

TEST(TableStagingTest, ReadKeyListIsSortedUniqueAndShort) {
  std::string previous;
  for (std::string_view key : kReadKeyList) {
    EXPECT_LE(key.size(), kMaxReadKeyLength) << key;
    EXPECT_FALSE(key.empty());
    EXPECT_LT(previous, std::string(key)) << "list must stay sorted and free of duplicates";
    previous = std::string(key);
  }
}

// ---- ScalarText -------------------------------------------------------------------------------

void ScalarTextBody(OperationContext& ctx, Scenario& sc) {
  lua_State* L = ctx.L;
  if (!LoadChunk(L, sc)) {
    return;
  }
  const int list = lua_gettop(L);
  const lua_Unsigned n = lua_rawlen(L, list);
  for (lua_Unsigned i = 1; i <= n; ++i) {
    lua_rawgeti(L, list, static_cast<lua_Integer>(i));
    const int value = lua_gettop(L);
    const int type_before = lua_type(L, value);
    char buffer[kScalarTextBuffer];
    std::string_view text;
    const bool ok = ScalarText(L, value, buffer, text);
    g.texts.push_back(ok ? std::string(text) : "<none>");
    if (lua_type(L, value) != type_before) {
      g.original_unchanged = false;  // lua_tolstring would have rewritten the number in place
    }
    std::size_t len = 0;
    const char* expected = luaL_tolstring(L, value, &len);
    g.expected_texts.emplace_back(expected, len);
    lua_settop(L, list);
  }
  sc.flag = true;
}

TEST(TableStagingTest, ScalarTextMatchesLuaTostringWithoutConvertingTheValue) {
  RawState s;
  g = Results{};
  Scenario sc;
  sc.body = ScalarTextBody;
  sc.chunk =
      "return {'str', 0, -1, 42, 7, 1.0, -0.0, 0.5, 1e15, 1e100, 2^53, 2^63, "
      "-2^63, 3.14159265358979, 1/3, 123456789012345, 9007199254740993, 0.1}";
  const Observed o = RunScenario(s, sc, [] {});
  ASSERT_EQ(o.status, LUA_OK);
  ASSERT_TRUE(sc.flag);
  ASSERT_EQ(g.texts.size(), g.expected_texts.size());
  for (std::size_t i = 0; i < g.texts.size(); ++i) {
    EXPECT_EQ(g.texts[i], g.expected_texts[i]) << "case " << i;
  }
  EXPECT_TRUE(g.original_unchanged);
}

void ScalarTextRejectsBody(OperationContext& ctx, Scenario& sc) {
  lua_State* L = ctx.L;
  char buffer[kScalarTextBuffer];
  std::string_view text;
  lua_pushboolean(L, 1);
  const bool b = ScalarText(L, -1, buffer, text);
  lua_pushnil(L);
  const bool n = ScalarText(L, -1, buffer, text);
  lua_newtable(L);
  const bool t = ScalarText(L, -1, buffer, text);
  lua_pop(L, 3);
  sc.flag = !b && !n && !t;
}

TEST(TableStagingTest, ScalarTextOnlyAcceptsStringsAndNumbers) {
  RawState s;
  Scenario sc;
  sc.body = ScalarTextRejectsBody;
  const Observed o = RunScenario(s, sc, [] {});
  ASSERT_EQ(o.status, LUA_OK);
  EXPECT_TRUE(sc.flag);
}

}  // namespace
}  // namespace vector_otel::core
