#include <gtest/gtest.h>

#include "vector_otel/core/lua_bridge.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace vector_otel {
namespace core {
namespace {

class LuaBridgeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    L_ = luaL_newstate();
    ASSERT_NE(L_, nullptr);
    luaL_openlibs(L_);

    const char* init_script = R"(
      emitted = {}
      function mock_emit(event)
        table.insert(emitted, event)
      end
    )";
    ASSERT_EQ(luaL_dostring(L_, init_script), LUA_OK);

    // Push mock_emit onto stack at index 1
    lua_getglobal(L_, "mock_emit");
    ASSERT_TRUE(lua_isfunction(L_, -1));
  }

  void TearDown() override {
    if (L_ != nullptr) {
      lua_close(L_);
      L_ = nullptr;
    }
  }

  lua_State* L_{nullptr};
};

TEST_F(LuaBridgeTest, N1FlatEmission) {
  int initial_top = lua_gettop(L_);
  EXPECT_EQ(initial_top, 1);  // mock_emit at index 1

  LuaEmitter emitter(L_, 1, 1);
  EXPECT_EQ(emitter.EmittedCount(), 0);

  // Record 1
  emitter.StartRecord();
  emitter.SetString("Timestamp", "2026-10-16T00:00:00.000000000Z");
  emitter.SetString("ServiceName", "frontend");
  emitter.SetInt64("SeverityNumber", 9);
  emitter.SetUint64("Flags", 1);
  emitter.SetDouble("Value", 123.456);
  emitter.SetBool("Dropped", false);
  emitter.EndRecord();

  EXPECT_EQ(emitter.EmittedCount(), 1);

  // Record 2
  emitter.StartRecord();
  emitter.SetString("Timestamp", "2026-10-16T00:00:01.000000000Z");
  emitter.SetString("ServiceName", "backend");
  emitter.SetBool("Dropped", true);
  emitter.EndRecord();

  EXPECT_EQ(emitter.EmittedCount(), 2);

  emitter.Flush();
  EXPECT_EQ(emitter.EmittedCount(), 2);
  EXPECT_EQ(lua_gettop(L_), initial_top);

  // Verify in Lua
  const char* verify_script = R"(
    assert(#emitted == 2, "expected 2 emitted events, got " .. #emitted)
    
    local r1 = emitted[1].log
    assert(r1 ~= nil, "emitted[1].log must exist")
    assert(r1.Timestamp == "2026-10-16T00:00:00.000000000Z", "r1.Timestamp mismatch")
    assert(r1.ServiceName == "frontend", "r1.ServiceName mismatch")
    assert(r1.SeverityNumber == 9, "r1.SeverityNumber mismatch")
    assert(r1.Flags == 1, "r1.Flags mismatch")
    assert(math.abs(r1.Value - 123.456) < 1e-6, "r1.Value mismatch")
    assert(r1.Dropped == false, "r1.Dropped mismatch")

    local r2 = emitted[2].log
    assert(r2 ~= nil, "emitted[2].log must exist")
    assert(r2.Timestamp == "2026-10-16T00:00:01.000000000Z", "r2.Timestamp mismatch")
    assert(r2.ServiceName == "backend", "r2.ServiceName mismatch")
    assert(r2.Dropped == true, "r2.Dropped mismatch")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(LuaBridgeTest, SetTableFromStack) {
  int initial_top = lua_gettop(L_);
  LuaEmitter emitter(L_, 1, 1);

  emitter.StartRecord();
  emitter.SetString("ServiceName", "auth-service");

  // Push child table onto stack
  lua_createtable(L_, 0, 2);
  lua_pushstring(L_, "foo_val");
  lua_setfield(L_, -2, "foo");
  lua_pushinteger(L_, 42);
  lua_setfield(L_, -2, "bar");

  // Table is at top of stack; SetTableFromStack pops it and sets it in record
  emitter.SetTableFromStack("LogAttributes");
  emitter.EndRecord();
  emitter.Flush();

  EXPECT_EQ(emitter.EmittedCount(), 1);
  EXPECT_EQ(lua_gettop(L_), initial_top);

  const char* verify_script = R"(
    assert(#emitted == 1, "expected 1 emitted event")
    local r1 = emitted[1].log
    assert(r1.ServiceName == "auth-service")
    assert(r1.LogAttributes ~= nil, "LogAttributes table missing")
    assert(r1.LogAttributes.foo == "foo_val")
    assert(r1.LogAttributes.bar == 42)
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(LuaBridgeTest, NGreaterThan1BatchEmission) {
  int initial_top = lua_gettop(L_);

  // N = 2 batch size
  LuaEmitter emitter(L_, 1, 2);
  EXPECT_EQ(emitter.EmittedCount(), 0);

  // Record 1
  emitter.StartRecord();
  emitter.SetString("Timestamp", "t1");
  emitter.SetString("ServiceName", "s1");
  emitter.EndRecord();
  EXPECT_EQ(emitter.EmittedCount(), 1);

  // Batch should not have emitted yet
  lua_getglobal(L_, "emitted");
  lua_len(L_, -1);
  int count_after_1 = static_cast<int>(lua_tointeger(L_, -1));
  lua_pop(L_, 2);
  EXPECT_EQ(count_after_1, 0);

  // Record 2 -> Should trigger batch emission of size 2
  emitter.StartRecord();
  emitter.SetString("Timestamp", "t2");
  emitter.SetString("ServiceName", "s2");
  emitter.EndRecord();
  EXPECT_EQ(emitter.EmittedCount(), 2);

  lua_getglobal(L_, "emitted");
  lua_len(L_, -1);
  int count_after_2 = static_cast<int>(lua_tointeger(L_, -1));
  lua_pop(L_, 2);
  EXPECT_EQ(count_after_2, 1);

  // Record 3 -> partial batch of 1
  emitter.StartRecord();
  emitter.SetString("Timestamp", "t3");
  emitter.SetString("ServiceName", "s3");
  emitter.EndRecord();
  EXPECT_EQ(emitter.EmittedCount(), 3);

  // Still only 1 emitted batch before Flush
  lua_getglobal(L_, "emitted");
  lua_len(L_, -1);
  int count_after_3 = static_cast<int>(lua_tointeger(L_, -1));
  lua_pop(L_, 2);
  EXPECT_EQ(count_after_3, 1);

  // Flush emits the final partial batch
  emitter.Flush();
  EXPECT_EQ(emitter.EmittedCount(), 3);
  EXPECT_EQ(lua_gettop(L_), initial_top);

  const char* verify_script = R"(
    assert(#emitted == 2, "expected 2 emitted batches, got " .. #emitted)

    -- Batch 1 has 2 records
    local b1 = emitted[1].log
    assert(b1 ~= nil, "batch 1 log missing")
    assert(b1.records ~= nil, "batch 1 records missing")
    assert(#b1.records == 2, "expected 2 records in batch 1, got " .. #b1.records)
    assert(b1.records[1].Timestamp == "t1")
    assert(b1.records[1].ServiceName == "s1")
    assert(b1.records[2].Timestamp == "t2")
    assert(b1.records[2].ServiceName == "s2")

    -- Batch 2 has 1 record
    local b2 = emitted[2].log
    assert(b2 ~= nil, "batch 2 log missing")
    assert(b2.records ~= nil, "batch 2 records missing")
    assert(#b2.records == 1, "expected 1 record in batch 2, got " .. #b2.records)
    assert(b2.records[1].Timestamp == "t3")
    assert(b2.records[1].ServiceName == "s3")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(LuaBridgeTest, StackBalanceAcrossSequences) {
  int initial_top = lua_gettop(L_);

  // Sequence with N=1
  {
    LuaEmitter emitter(L_, 1, 1);
    for (int i = 0; i < 5; ++i) {
      emitter.StartRecord();
      emitter.SetInt64("Index", i);
      emitter.EndRecord();
    }
    emitter.Flush();
    EXPECT_EQ(lua_gettop(L_), initial_top);
    EXPECT_EQ(emitter.EmittedCount(), 5);
  }
  EXPECT_EQ(lua_gettop(L_), initial_top);

  // Sequence with N=3 (5 records -> 1 batch of 3, 1 batch of 2 on flush)
  {
    LuaEmitter emitter(L_, 1, 3);
    for (int i = 0; i < 5; ++i) {
      emitter.StartRecord();
      emitter.SetInt64("Index", i);
      emitter.EndRecord();
    }
    emitter.Flush();
    EXPECT_EQ(lua_gettop(L_), initial_top);
    EXPECT_EQ(emitter.EmittedCount(), 5);
  }
  EXPECT_EQ(lua_gettop(L_), initial_top);
}

TEST_F(LuaBridgeTest, NegativeEmitIndex) {
  // Push dummy values to make mock_emit not at index 1
  lua_pushstring(L_, "dummy1");
  lua_pushstring(L_, "dummy2");
  // Now push mock_emit again at top (index -1)
  lua_getglobal(L_, "mock_emit");

  int top_before = lua_gettop(L_);
  // Pass -1 as emit index
  LuaEmitter emitter(L_, -1, 1);
  emitter.StartRecord();
  emitter.SetString("Key", "Val");
  emitter.EndRecord();
  emitter.Flush();

  EXPECT_EQ(lua_gettop(L_), top_before);
  EXPECT_EQ(emitter.EmittedCount(), 1);

  // Pop dummy values and mock_emit
  lua_pop(L_, 3);
}

TEST_F(LuaBridgeTest, NullStateSafety) {
  LuaEmitter emitter(nullptr, 0, 1);
  EXPECT_EQ(emitter.EmittedCount(), 0);

  emitter.StartRecord();
  emitter.SetString("key", "val");
  emitter.SetInt64("key", 1);
  emitter.SetUint64("key", 2);
  emitter.SetDouble("key", 3.0);
  emitter.SetBool("key", true);
  emitter.SetTableFromStack("key");
  emitter.EndRecord();
  emitter.Flush();

  EXPECT_EQ(emitter.EmittedCount(), 1);
}

TEST_F(LuaBridgeTest, ScopedLuaRefBasicAndMove) {
  lua_newtable(L_);
  int ref = luaL_ref(L_, LUA_REGISTRYINDEX);
  EXPECT_NE(ref, LUA_NOREF);

  {
    ScopedLuaRef scoped(L_, ref);
    EXPECT_TRUE(scoped.valid());
    EXPECT_EQ(scoped.get(), ref);

    // Verify move construction
    ScopedLuaRef moved(std::move(scoped));
    EXPECT_TRUE(moved.valid());
    EXPECT_EQ(moved.get(), ref);
    EXPECT_FALSE(scoped.valid());
    EXPECT_EQ(scoped.get(), LUA_NOREF);

    // Verify move assignment
    ScopedLuaRef moved2;
    EXPECT_FALSE(moved2.valid());
    moved2 = std::move(moved);
    EXPECT_TRUE(moved2.valid());
    EXPECT_EQ(moved2.get(), ref);
    EXPECT_FALSE(moved.valid());

    // Verify reset
    moved2.reset();
    EXPECT_FALSE(moved2.valid());
    EXPECT_EQ(moved2.get(), LUA_NOREF);
  }
}

TEST_F(LuaBridgeTest, ScopedLuaRefDestructorUnrefs) {
  lua_newtable(L_);
  int ref1 = luaL_ref(L_, LUA_REGISTRYINDEX);
  EXPECT_NE(ref1, LUA_NOREF);

  {
    ScopedLuaRef scoped(L_, ref1);
    EXPECT_TRUE(scoped.valid());
  }

  // After destruction, ref1 was unreferenced and returned to Lua's registry
  // free list. The next luaL_ref will reuse the freed slot ref1.
  lua_newtable(L_);
  int ref2 = luaL_ref(L_, LUA_REGISTRYINDEX);
  EXPECT_EQ(ref2, ref1);
  luaL_unref(L_, LUA_REGISTRYINDEX, ref2);
}

TEST(LuaBridgeStandaloneTest, CompileTimeBatchSizeConstant) {
  EXPECT_EQ(kCompileTimeBatchSize, VECTOR_OTEL_EMIT_BATCH_SIZE);
}

TEST_F(LuaBridgeTest, ProtectedEmitCatchesLuaError) {
  const char* error_script = R"(
    function failing_emit(event)
      error("downstream sink queue full")
    end
  )";
  ASSERT_EQ(luaL_dostring(L_, error_script), LUA_OK);

  lua_getglobal(L_, "failing_emit");
  int fail_emit_idx = lua_gettop(L_);

  LuaEmitter emitter(L_, fail_emit_idx, 1);
  EXPECT_FALSE(emitter.HasError());

  emitter.StartRecord();
  emitter.SetString("ServiceName", "test-svc");
  emitter.EndRecord();

  EXPECT_TRUE(emitter.HasError());
  EXPECT_NE(emitter.ErrorMessage().find("downstream sink queue full"), std::string_view::npos);
  EXPECT_EQ(emitter.EmittedCount(), 0);

  // Subsequent records and flush should safely no-op without crashing
  emitter.StartRecord();
  emitter.SetString("ServiceName", "test-svc-2");
  emitter.EndRecord();
  emitter.Flush();

  lua_pop(L_, 1);  // pop failing_emit
}

TEST_F(LuaBridgeTest, CustomRecordFieldName) {
  LuaEmitter emitter(L_, 1, 1, "trace");
  emitter.StartRecord();
  emitter.SetString("SpanName", "GET /api");
  emitter.EndRecord();
  emitter.Flush();

  EXPECT_FALSE(emitter.HasError());
  EXPECT_EQ(emitter.EmittedCount(), 1);

  const char* verify_script = R"(
    local ev = emitted[#emitted]
    assert(ev.trace ~= nil, "ev.trace must exist")
    assert(ev.trace.SpanName == "GET /api", "SpanName mismatch")
    assert(ev.log == nil, "ev.log must not exist")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(LuaBridgeTest, DestructorRestoresStackTopWhenInRecord) {
  int initial_top = lua_gettop(L_);
  {
    core::LuaEmitter emitter(L_, 1);
    emitter.StartRecord();
    EXPECT_GT(lua_gettop(L_), initial_top);
    // Destructor runs without calling EndRecord()
  }
  EXPECT_EQ(lua_gettop(L_), initial_top);
}

TEST_F(LuaBridgeTest, DestructorRestoresStackTopWhenInRecordBatchMode) {
  int initial_top = lua_gettop(L_);
  {
    core::LuaEmitter emitter(L_, 1, 4);
    emitter.StartRecord();
    EXPECT_GT(lua_gettop(L_), initial_top);
    // Destructor runs without calling EndRecord()
  }
  EXPECT_EQ(lua_gettop(L_), initial_top);
}

TEST_F(LuaBridgeTest, DestructorRestoresStackTopWhenHasError) {
  const char* error_script = R"(
    function failing_emit_stack(event)
      error("emit failed")
    end
  )";
  ASSERT_EQ(luaL_dostring(L_, error_script), LUA_OK);

  lua_getglobal(L_, "failing_emit_stack");
  int fail_idx = lua_gettop(L_);
  int initial_top = fail_idx;

  {
    core::LuaEmitter emitter(L_, fail_idx, 1);
    emitter.StartRecord();
    emitter.EndRecord();  // Failing emit sets has_error_ = true

    EXPECT_TRUE(emitter.HasError());
    // Simulate unpopped intermediate values pushed before destruction
    lua_pushstring(L_, "leftover_value");
    EXPECT_GT(lua_gettop(L_), initial_top);
  }
  // Destructor observes has_error_ and cleans up stack to initial_top_
  EXPECT_EQ(lua_gettop(L_), initial_top);

  lua_pop(L_, 1);  // pop failing_emit_stack
}

TEST_F(LuaBridgeTest, DestructorMoveSemanticsStackRestoration) {
  int initial_top = lua_gettop(L_);
  {
    core::LuaEmitter emitter1(L_, 1);
    emitter1.StartRecord();
    EXPECT_GT(lua_gettop(L_), initial_top);

    // Move construct emitter2 from emitter1
    core::LuaEmitter emitter2(std::move(emitter1));
    EXPECT_GT(lua_gettop(L_), initial_top);
  }
  EXPECT_EQ(lua_gettop(L_), initial_top);
}

}  // namespace
}  // namespace core
}  // namespace vector_otel
