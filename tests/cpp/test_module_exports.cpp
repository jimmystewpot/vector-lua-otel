#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <string>

#include "vector_otel/core/module_runner.hpp"
#include "vector_otel/core/module_state.hpp"
#include "vector_otel/core/process_runner.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

extern "C" {
int luaopen_vector_otel(lua_State* L);
int luaopen_vector_otel_logs(lua_State* L);
int luaopen_vector_otel_traces(lua_State* L);
int luaopen_vector_otel_metrics(lua_State* L);
int luaopen_vector_native(lua_State* L);
int luaopen_vector_native_logs(lua_State* L);
int luaopen_vector_native_traces(lua_State* L);
int luaopen_vector_native_metrics(lua_State* L);
}

class ModuleExportsTest : public ::testing::Test {
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
  }

  void TearDown() override {
    if (L_ != nullptr) {
      lua_close(L_);
      L_ = nullptr;
    }
  }

  lua_State* L_{nullptr};
};

TEST_F(ModuleExportsTest, ModuleStructureAndVersion) {
  luaopen_vector_otel(L_);
  ASSERT_TRUE(lua_istable(L_, -1));

  // Check _VERSION
  lua_getfield(L_, -1, "_VERSION");
  EXPECT_TRUE(lua_isstring(L_, -1));
  EXPECT_STREQ(lua_tostring(L_, -1), "0.2.0");
  lua_pop(L_, 1);

  // Check logs submodule
  lua_getfield(L_, -1, "logs");
  ASSERT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, -1, "process");
  EXPECT_TRUE(lua_iscfunction(L_, -1));
  lua_pop(L_, 2);  // pop process, logs

  // Check traces submodule
  lua_getfield(L_, -1, "traces");
  ASSERT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, -1, "process");
  EXPECT_TRUE(lua_iscfunction(L_, -1));
  lua_pop(L_, 2);  // pop process, traces

  // Check metrics submodule
  lua_getfield(L_, -1, "metrics");
  ASSERT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, -1, "process");
  EXPECT_TRUE(lua_iscfunction(L_, -1));
  lua_pop(L_, 2);  // pop process, metrics

  lua_pop(L_, 1);  // pop main module table
}

TEST_F(ModuleExportsTest, PackageLoadedRegistration) {
  luaopen_vector_otel(L_);
  int mod_idx = lua_gettop(L_);

  // Check package.loaded table
  lua_getglobal(L_, "package");
  ASSERT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, -1, "loaded");
  ASSERT_TRUE(lua_istable(L_, -1));

  // Check package.loaded["vector_otel.logs"]
  lua_getfield(L_, -1, "vector_otel.logs");
  EXPECT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, mod_idx, "logs");
  EXPECT_TRUE(lua_rawequal(L_, -1, -2));
  lua_pop(L_, 2);

  // Check package.loaded["vector_otel.traces"]
  lua_getfield(L_, -1, "vector_otel.traces");
  EXPECT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, mod_idx, "traces");
  EXPECT_TRUE(lua_rawequal(L_, -1, -2));
  lua_pop(L_, 2);

  // Check package.loaded["vector_otel.metrics"]
  lua_getfield(L_, -1, "vector_otel.metrics");
  EXPECT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, mod_idx, "metrics");
  EXPECT_TRUE(lua_rawequal(L_, -1, -2));
  lua_pop(L_, 2);

  lua_pop(L_, 3);  // pop loaded, package, mod
}

TEST_F(ModuleExportsTest, LogsProcessSuccessContract) {
  luaopen_vector_otel(L_);
  lua_setglobal(L_, "vector_otel");

  const char* script = R"(
    local payload = '{"resourceLogs":[{"resource":{"attributes":[{"key":"service.name","value":{"stringValue":"svc"}}]},"scopeLogs":[{"scope":{"name":"s"},"logRecords":[{"timeUnixNano":"1293840000000000000","body":{"stringValue":"hello"}}]}]}]}'
    local count, err = vector_otel.logs.process(payload, mock_emit)
    return count, err
  )";

  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
  EXPECT_EQ(lua_gettop(L_), 2);
  EXPECT_TRUE(lua_isinteger(L_, 1));
  EXPECT_EQ(lua_tointeger(L_, 1), 1);
  EXPECT_TRUE(lua_isnil(L_, 2));

  // Verify emit was called
  lua_getglobal(L_, "emitted");
  EXPECT_EQ(lua_rawlen(L_, -1), 1u);
  lua_pop(L_, 3);
}

TEST_F(ModuleExportsTest, LogsProcessFailureContractMalformedJson) {
  luaopen_vector_otel(L_);
  lua_setglobal(L_, "vector_otel");

  const char* script = R"(
    local count, err, emitted_count = vector_otel.logs.process("{bad json", mock_emit)
    return count, err, emitted_count
  )";

  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
  EXPECT_EQ(lua_gettop(L_), 3);
  // 1st value MUST be nil on error because in Lua 0 is truthy
  EXPECT_TRUE(lua_isnil(L_, 1));
  // 2nd value is error string
  EXPECT_TRUE(lua_isstring(L_, 2));
  EXPECT_GT(lua_rawlen(L_, 2), 0u);
  // 3rd value is count (integer 0)
  EXPECT_TRUE(lua_isinteger(L_, 3));
  EXPECT_EQ(lua_tointeger(L_, 3), 0);
  lua_pop(L_, 3);
}

TEST_F(ModuleExportsTest, TracesProcessSuccessAndFailureContracts) {
  luaopen_vector_otel(L_);
  lua_setglobal(L_, "vector_otel");

  // Success case
  const char* ok_script = R"(
    local payload = '{"resourceSpans":[{"resource":{},"scopeSpans":[{"scope":{},"spans":[{"traceId":"5b8efff798038103d269b633813fc60c","spanId":"eee19b7ec3c1b174","name":"span1","startTimeUnixNano":"1293840000000000000","endTimeUnixNano":"1293840000100000000"}]}]}]}'
    local count, err = vector_otel.traces.process(payload, mock_emit)
    return count, err
  )";
  ASSERT_EQ(luaL_dostring(L_, ok_script), LUA_OK);
  EXPECT_EQ(lua_gettop(L_), 2);
  EXPECT_TRUE(lua_isinteger(L_, 1));
  EXPECT_EQ(lua_tointeger(L_, 1), 1);
  EXPECT_TRUE(lua_isnil(L_, 2));
  lua_pop(L_, 2);

  // Failure case
  const char* fail_script = R"(
    local count, err, emitted_count = vector_otel.traces.process("invalid json", mock_emit)
    return count, err, emitted_count
  )";
  ASSERT_EQ(luaL_dostring(L_, fail_script), LUA_OK);
  EXPECT_EQ(lua_gettop(L_), 3);
  EXPECT_TRUE(lua_isnil(L_, 1));
  EXPECT_TRUE(lua_isstring(L_, 2));
  EXPECT_TRUE(lua_isinteger(L_, 3));
  EXPECT_EQ(lua_tointeger(L_, 3), 0);
  lua_pop(L_, 3);
}

TEST_F(ModuleExportsTest, MetricsProcessSuccessAndFailureContracts) {
  luaopen_vector_otel(L_);
  lua_setglobal(L_, "vector_otel");

  // Success case (empty resourceMetrics)
  const char* ok_script = R"(
    local payload = '{"resourceMetrics":[]}'
    local count, err = vector_otel.metrics.process(payload, mock_emit)
    return count, err
  )";
  ASSERT_EQ(luaL_dostring(L_, ok_script), LUA_OK);
  EXPECT_EQ(lua_gettop(L_), 2);
  EXPECT_TRUE(lua_isinteger(L_, 1));
  EXPECT_EQ(lua_tointeger(L_, 1), 0);
  EXPECT_TRUE(lua_isnil(L_, 2));
  lua_pop(L_, 2);

  // Failure case
  const char* fail_script = R"(
    local count, err, emitted_count = vector_otel.metrics.process("{not a json", mock_emit)
    return count, err, emitted_count
  )";
  ASSERT_EQ(luaL_dostring(L_, fail_script), LUA_OK);
  EXPECT_EQ(lua_gettop(L_), 3);
  EXPECT_TRUE(lua_isnil(L_, 1));
  EXPECT_TRUE(lua_isstring(L_, 2));
  EXPECT_TRUE(lua_isinteger(L_, 3));
  EXPECT_EQ(lua_tointeger(L_, 3), 0);
  lua_pop(L_, 3);
}

TEST_F(ModuleExportsTest, InvalidArgumentTypesReturnErrorContract) {
  luaopen_vector_otel(L_);
  lua_setglobal(L_, "vector_otel");

  // Argument 1 not a string
  const char* bad_arg1 = R"(
    local count, err, emitted_count = vector_otel.logs.process(12345, mock_emit)
    return count, err, emitted_count
  )";
  ASSERT_EQ(luaL_dostring(L_, bad_arg1), LUA_OK);
  EXPECT_EQ(lua_gettop(L_), 3);
  EXPECT_TRUE(lua_isnil(L_, 1));
  EXPECT_TRUE(lua_isstring(L_, 2));
  EXPECT_TRUE(lua_isinteger(L_, 3));
  lua_pop(L_, 3);

  // Argument 2 not a function
  const char* bad_arg2 = R"(
    local count, err, emitted_count = vector_otel.logs.process('{"resourceLogs":[]}', "not_a_func")
    return count, err, emitted_count
  )";
  ASSERT_EQ(luaL_dostring(L_, bad_arg2), LUA_OK);
  EXPECT_EQ(lua_gettop(L_), 3);
  EXPECT_TRUE(lua_isnil(L_, 1));
  EXPECT_TRUE(lua_isstring(L_, 2));
  EXPECT_TRUE(lua_isinteger(L_, 3));
  lua_pop(L_, 3);
}

TEST_F(ModuleExportsTest, StandaloneSubmoduleOpeners) {
  // Test direct open of vector_otel_logs
  lua_State* L1 = luaL_newstate();
  luaL_openlibs(L1);
  luaopen_vector_otel_logs(L1);
  EXPECT_TRUE(lua_istable(L1, -1));
  lua_getfield(L1, -1, "process");
  EXPECT_TRUE(lua_iscfunction(L1, -1));
  lua_pop(L1, 2);
  lua_close(L1);

  // Test direct open of vector_otel_traces
  lua_State* L2 = luaL_newstate();
  luaL_openlibs(L2);
  luaopen_vector_otel_traces(L2);
  EXPECT_TRUE(lua_istable(L2, -1));
  lua_getfield(L2, -1, "process");
  EXPECT_TRUE(lua_iscfunction(L2, -1));
  lua_pop(L2, 2);
  lua_close(L2);

  // Test direct open of vector_otel_metrics
  lua_State* L3 = luaL_newstate();
  luaL_openlibs(L3);
  luaopen_vector_otel_metrics(L3);
  EXPECT_TRUE(lua_istable(L3, -1));
  lua_getfield(L3, -1, "process");
  EXPECT_TRUE(lua_iscfunction(L3, -1));
  lua_pop(L3, 2);
  lua_close(L3);
}

namespace {

struct AllocBudget {
  size_t remaining = SIZE_MAX;
};

// Allocator that fails any growth request exceeding the remaining budget. Frees and shrinks
// always succeed so that lua_close() is safe.
void* BudgetedAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
  auto* budget = static_cast<AllocBudget*>(ud);
  if (nsize == 0) {
    free(ptr);
    return nullptr;
  }
  const size_t old_size = ptr == nullptr ? 0 : osize;
  if (nsize > old_size) {
    const size_t growth = nsize - old_size;
    if (growth > budget->remaining) {
      return nullptr;
    }
    budget->remaining -= growth;
  }
  return realloc(ptr, nsize);
}

}  // namespace

// A Lua memory error raised inside the extractor longjmps out of C++ frames. The re-entrancy
// flag must still be cleared so later calls on the same thread keep working.
TEST(ModuleOomTest, ProcessLogsRecoversFromLuaMemoryErrorWithoutStickingReentrancyGuard) {
  int memory_errors_seen = 0;
  for (size_t budget = 0; budget < 6000; budget += (budget < 1500 ? 3 : 25)) {
    AllocBudget alloc;
    lua_State* L = lua_newstate(BudgetedAlloc, &alloc);
    ASSERT_NE(L, nullptr);
    luaL_openlibs(L);
    luaopen_vector_otel(L);
    lua_setglobal(L, "vector_otel");

    // Everything that allocates is prepared while the budget is unlimited.
    const char* setup = R"(
      payload = '{"resourceLogs":[{"resource":{"attributes":[{"key":"service.name","value":{"stringValue":"svc"}}]},"scopeLogs":[{"scope":{"name":"s"},"logRecords":[{"timeUnixNano":"1293840000000000000","body":{"stringValue":"hello"}}]}]}]}'
      function noop_emit(event) end
      function run() return vector_otel.logs.process(payload, noop_emit) end
    )";
    ASSERT_EQ(luaL_dostring(L, setup), LUA_OK) << lua_tostring(L, -1);
    ASSERT_EQ(lua_gettop(L), 0);

    // Limited-budget call: may hit a memory error anywhere. lua_pcall keeps it contained.
    alloc.remaining = budget;
    lua_getglobal(L, "run");
    const int first_status = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (first_status == LUA_ERRMEM) {
      ++memory_errors_seen;
    }
    lua_settop(L, 0);

    // Recovery call with unlimited budget must succeed, not report a stuck guard.
    alloc.remaining = SIZE_MAX;
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_getglobal(L, "run");
    ASSERT_EQ(lua_pcall(L, 0, 2, 0), LUA_OK) << lua_tostring(L, -1);
    EXPECT_TRUE(lua_isinteger(L, 1))
        << "budget=" << budget << " err=" << (lua_isstring(L, 2) ? lua_tostring(L, 2) : "?");
    if (lua_isinteger(L, 1)) {
      EXPECT_EQ(lua_tointeger(L, 1), 1) << "budget=" << budget;
    }
    lua_close(L);
  }
  // The sweep must actually exercise the failure path, otherwise the test proves nothing.
  EXPECT_GT(memory_errors_seen, 0);
}

TEST_F(ModuleExportsTest, NativeModuleStructureAndVersion) {
  luaopen_vector_native(L_);
  ASSERT_TRUE(lua_istable(L_, -1));

  // Check _VERSION
  lua_getfield(L_, -1, "_VERSION");
  EXPECT_TRUE(lua_isstring(L_, -1));
  EXPECT_STREQ(lua_tostring(L_, -1), "0.2.0");
  lua_pop(L_, 1);

  // Check logs submodule
  lua_getfield(L_, -1, "logs");
  ASSERT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, -1, "process");
  EXPECT_TRUE(lua_iscfunction(L_, -1));
  lua_pop(L_, 2);

  // Check traces submodule
  lua_getfield(L_, -1, "traces");
  ASSERT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, -1, "process");
  EXPECT_TRUE(lua_iscfunction(L_, -1));
  lua_pop(L_, 2);

  // Check metrics submodule
  lua_getfield(L_, -1, "metrics");
  ASSERT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, -1, "process");
  EXPECT_TRUE(lua_iscfunction(L_, -1));
  lua_pop(L_, 2);

  lua_pop(L_, 1);
}

TEST_F(ModuleExportsTest, NativePackageLoadedRegistration) {
  luaopen_vector_native(L_);
  int mod_idx = lua_gettop(L_);

  lua_getglobal(L_, "package");
  ASSERT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, -1, "loaded");
  ASSERT_TRUE(lua_istable(L_, -1));

  lua_getfield(L_, -1, "vector_native");
  EXPECT_TRUE(lua_istable(L_, -1));
  EXPECT_TRUE(lua_rawequal(L_, -1, mod_idx));
  lua_pop(L_, 1);

  lua_getfield(L_, -1, "vector_native.logs");
  EXPECT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, mod_idx, "logs");
  EXPECT_TRUE(lua_rawequal(L_, -1, -2));
  lua_pop(L_, 2);

  lua_getfield(L_, -1, "vector_native.traces");
  EXPECT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, mod_idx, "traces");
  EXPECT_TRUE(lua_rawequal(L_, -1, -2));
  lua_pop(L_, 2);

  lua_getfield(L_, -1, "vector_native.metrics");
  EXPECT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, mod_idx, "metrics");
  EXPECT_TRUE(lua_rawequal(L_, -1, -2));
  lua_pop(L_, 2);

  lua_pop(L_, 3);
}

TEST_F(ModuleExportsTest, NativeProcessContractsAndValidation) {
  luaopen_vector_native(L_);
  lua_setglobal(L_, "vector_native");

  // 1. Logs table & string success
  const char* logs_ok = R"(
    local c1, e1 = vector_native.logs.process({ message = "hi" }, mock_emit)
    local c2, e2 = vector_native.logs.process('{"message":"hi2"}', mock_emit)
    assert(c1 == 1 and e1 == nil)
    assert(c2 == 1 and e2 == nil)
  )";
  ASSERT_EQ(luaL_dostring(L_, logs_ok), LUA_OK) << lua_tostring(L_, -1);

  // 2. Traces table & string success
  const char* traces_ok = R"(
    local c1, e1 = vector_native.traces.process({ name = "span1" }, mock_emit)
    local c2, e2 = vector_native.traces.process('{"name":"span2"}', mock_emit)
    assert(c1 == 1 and e1 == nil)
    assert(c2 == 1 and e2 == nil)
  )";
  ASSERT_EQ(luaL_dostring(L_, traces_ok), LUA_OK) << lua_tostring(L_, -1);

  // 3. Metrics table & string success + unsupported variant failure
  const char* metrics_check = R"(
    local c1, e1 = vector_native.metrics.process({ name = "m1", gauge = { value = 1.0 } }, mock_emit)
    local c2, e2 = vector_native.metrics.process('{"name":"m2","counter":{"value":2}}', mock_emit)
    assert(c1 == 1 and e1 == nil)
    assert(c2 == 1 and e2 == nil)
    local c_drop, e_drop = vector_native.metrics.process({ name = "bad", distribution = {} }, mock_emit)
    assert(c_drop == 0 and e_drop == nil)
  )";
  ASSERT_EQ(luaL_dostring(L_, metrics_check), LUA_OK) << lua_tostring(L_, -1);

  // 4. Argument validation
  const char* arg_validation = R"(
    local c1, e1, p1 = vector_native.logs.process(123, mock_emit)
    assert(c1 == nil and e1 == "payload must be a string or table" and p1 == 0)
    local c2, e2, p2 = vector_native.traces.process({}, "not_func")
    assert(c2 == nil and e2 == "emit callback must be a function" and p2 == 0)
    local c3, e3, p3 = vector_native.metrics.process(true, mock_emit)
    assert(c3 == nil and e3 == "payload must be a string or table" and p3 == 0)
  )";
  ASSERT_EQ(luaL_dostring(L_, arg_validation), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(ModuleExportsTest, NativeReentrancyGuard) {
  luaopen_vector_native(L_);
  lua_setglobal(L_, "vector_native");

  const char* reentrant_script = R"(
    local inner_c, inner_err, inner_p
    local count, err = vector_native.logs.process({ message = "outer" }, function(ev)
      inner_c, inner_err, inner_p = vector_native.logs.process({ message = "inner" }, mock_emit)
    end)
    assert(count == 1 and err == nil)
    assert(inner_c == nil)
    assert(inner_err == "re-entrant vector_otel.process() invocation is not permitted")
    assert(inner_p == 0)
  )";
  ASSERT_EQ(luaL_dostring(L_, reentrant_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(ModuleExportsTest, NativeStandaloneSubmoduleOpeners) {
  lua_State* L1 = luaL_newstate();
  luaL_openlibs(L1);
  luaopen_vector_native_logs(L1);
  EXPECT_TRUE(lua_istable(L1, -1));
  lua_getfield(L1, -1, "process");
  EXPECT_TRUE(lua_iscfunction(L1, -1));
  lua_pop(L1, 2);
  lua_close(L1);

  lua_State* L2 = luaL_newstate();
  luaL_openlibs(L2);
  luaopen_vector_native_traces(L2);
  EXPECT_TRUE(lua_istable(L2, -1));
  lua_getfield(L2, -1, "process");
  EXPECT_TRUE(lua_iscfunction(L2, -1));
  lua_pop(L2, 2);
  lua_close(L2);

  lua_State* L3 = luaL_newstate();
  luaL_openlibs(L3);
  luaopen_vector_native_metrics(L3);
  EXPECT_TRUE(lua_istable(L3, -1));
  lua_getfield(L3, -1, "process");
  EXPECT_TRUE(lua_iscfunction(L3, -1));
  lua_pop(L3, 2);
  lua_close(L3);
}

TEST_F(ModuleExportsTest, ReentrantProcessCallReturnsErrorContract) {
  luaopen_vector_otel(L_);
  lua_setglobal(L_, "vector_otel");

  const char* script = R"(
    local reentrant_err = nil
    local function reentrant_emit(event)
      local count, err = vector_otel.logs.process('{"resourceLogs":[]}', function() end)
      reentrant_err = err
    end
    local payload = '{"resourceLogs":[{"resource":{},"scopeLogs":[{"scope":{},"logRecords":[{"body":{"stringValue":"hi"}}]}]}]}'
    local count, err = vector_otel.logs.process(payload, reentrant_emit)
    return count, err, reentrant_err
  )";
  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK) << lua_tostring(L_, -1);
  EXPECT_EQ(lua_gettop(L_), 3);
  EXPECT_TRUE(lua_isinteger(L_, 1));
  EXPECT_TRUE(lua_isnil(L_, 2));
  EXPECT_TRUE(lua_isstring(L_, 3));
  EXPECT_TRUE(std::string(lua_tostring(L_, 3)).find("re-entrant") != std::string::npos);
  lua_pop(L_, 3);
}

TEST_F(ModuleExportsTest, EmitCallbackThrowsLuaErrorReturnsContract) {
  luaopen_vector_native(L_);
  lua_setglobal(L_, "vector_native");

  const char* script = R"(
    local function exploding_emit(event)
      error("emit exploded!")
    end
    local payload = { message = "boom" }
    local count, err, partial = vector_native.logs.process(payload, exploding_emit)
    return count, err, partial
  )";
  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK) << lua_tostring(L_, -1);
  EXPECT_EQ(lua_gettop(L_), 3);
  EXPECT_TRUE(lua_isnil(L_, 1));
  EXPECT_TRUE(lua_isstring(L_, 2));
  EXPECT_TRUE(std::string(lua_tostring(L_, 2)).find("emit exploded") != std::string::npos);
  EXPECT_TRUE(lua_isinteger(L_, 3));
  lua_pop(L_, 3);
}

TEST_F(ModuleExportsTest, NonCallableEmitCallbackReturnsErrorContract) {
  luaopen_vector_otel(L_);
  lua_setglobal(L_, "vector_otel");
  luaopen_vector_native(L_);
  lua_setglobal(L_, "vector_native");

  const char* script = R"(
    local c1, e1, p1 = vector_otel.logs.process("{}", "not_a_function")
    local c2, e2, p2 = vector_native.logs.process({}, 12345)
    return c1, e1, p1, c2, e2, p2
  )";
  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK) << lua_tostring(L_, -1);
  EXPECT_EQ(lua_gettop(L_), 6);
  EXPECT_TRUE(lua_isnil(L_, 1));
  EXPECT_TRUE(lua_isstring(L_, 2));
  EXPECT_STREQ(lua_tostring(L_, 2), "emit callback must be a function");
  EXPECT_EQ(lua_tointeger(L_, 3), 0);

  EXPECT_TRUE(lua_isnil(L_, 4));
  EXPECT_TRUE(lua_isstring(L_, 5));
  EXPECT_STREQ(lua_tostring(L_, 5), "emit callback must be a function");
  EXPECT_EQ(lua_tointeger(L_, 6), 0);
  lua_pop(L_, 6);
}

TEST_F(ModuleExportsTest, InvalidPayloadTypeReturnsErrorContract) {
  luaopen_vector_otel(L_);
  lua_setglobal(L_, "vector_otel");
  luaopen_vector_native(L_);
  lua_setglobal(L_, "vector_native");

  const char* script = R"(
    local noop = function() end
    local c1, e1, p1 = vector_otel.logs.process(true, noop)
    local c2, e2, p2 = vector_native.logs.process(12345, noop)
    return c1, e1, p1, c2, e2, p2
  )";
  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK) << lua_tostring(L_, -1);
  EXPECT_EQ(lua_gettop(L_), 6);
  EXPECT_TRUE(lua_isnil(L_, 1));
  EXPECT_TRUE(lua_isstring(L_, 2));
  EXPECT_STREQ(lua_tostring(L_, 2), "payload must be a string or table");
  EXPECT_EQ(lua_tointeger(L_, 3), 0);

  EXPECT_TRUE(lua_isnil(L_, 4));
  EXPECT_TRUE(lua_isstring(L_, 5));
  EXPECT_STREQ(lua_tostring(L_, 5), "payload must be a string or table");
  EXPECT_EQ(lua_tointeger(L_, 6), 0);
  lua_pop(L_, 6);
}

namespace {
using vector_otel::core::ModuleKind;
using vector_otel::core::OperationContext;
using vector_otel::core::OtelParser;
using vector_otel::core::Signal;
using vector_otel::core::SignalExtractors;

bool ThrowingStdExtractor(OperationContext&, OtelParser&, simdjson::padded_string_view) {
  throw std::runtime_error("simulated std::exception");
}
bool ThrowingNonStdExtractor(OperationContext&, OtelParser&, simdjson::padded_string_view) {
  throw 42;
}
// Returns failure without recording an error code: a contract violation the runner must still
// report as a generic extraction error.
bool SilentFailureExtractor(OperationContext&, OtelParser&, simdjson::padded_string_view) {
  return false;
}
bool DummyTableExtractor(OperationContext&) { return true; }

int ProcessWithStdThrow(lua_State* L) {
  return vector_otel::core::DispatchProcess(
      L, ModuleKind::Otel, Signal::Logs,
      SignalExtractors{ThrowingStdExtractor, DummyTableExtractor});
}
int ProcessWithNonStdThrow(lua_State* L) {
  return vector_otel::core::DispatchProcess(
      L, ModuleKind::Otel, Signal::Logs,
      SignalExtractors{ThrowingNonStdExtractor, DummyTableExtractor});
}
int ProcessWithSilentFailure(lua_State* L) {
  return vector_otel::core::DispatchProcess(
      L, ModuleKind::Otel, Signal::Logs,
      SignalExtractors{SilentFailureExtractor, DummyTableExtractor});
}
const luaL_Reg kDispatchTestFuncs[] = {
    {"std_throw", ProcessWithStdThrow},
    {"non_std_throw", ProcessWithNonStdThrow},
    {"silent_failure", ProcessWithSilentFailure},
    {nullptr, nullptr},
};
}  // namespace

// Exceptions thrown by an extractor (std::exception, non-std, or a silent failure) never escape
// DispatchProcess: each becomes (nil, message, 0) and the state keeps working.
TEST_F(ModuleExportsTest, DispatchProcessExceptionHandlingAndSilentFailure) {
  ASSERT_EQ(vector_otel::core::OpenSignalTable(L_, ModuleKind::Otel, kDispatchTestFuncs), 1);
  lua_setglobal(L_, "dispatch_test");

  const char* script = R"(
    local noop = function() end
    local c1, e1, p1 = dispatch_test.std_throw("{}", noop)
    local c2, e2, p2 = dispatch_test.non_std_throw("{}", noop)
    local c3, e3, p3 = dispatch_test.silent_failure("{}", noop)
    return c1, e1, p1, c2, e2, p2, c3, e3, p3
  )";
  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK) << lua_tostring(L_, -1);
  EXPECT_EQ(lua_gettop(L_), 9);

  EXPECT_TRUE(lua_isnil(L_, 1));
  ASSERT_TRUE(lua_isstring(L_, 2));
  EXPECT_STREQ(lua_tostring(L_, 2), "C++ exception: simulated std::exception");
  EXPECT_EQ(lua_tointeger(L_, 3), 0);

  EXPECT_TRUE(lua_isnil(L_, 4));
  ASSERT_TRUE(lua_isstring(L_, 5));
  EXPECT_STREQ(lua_tostring(L_, 5), "Unknown C++ exception");
  EXPECT_EQ(lua_tointeger(L_, 6), 0);

  EXPECT_TRUE(lua_isnil(L_, 7));
  ASSERT_TRUE(lua_isstring(L_, 8));
  EXPECT_STREQ(lua_tostring(L_, 8), "extraction failed");
  EXPECT_EQ(lua_tointeger(L_, 9), 0);

  lua_pop(L_, 9);
}

TEST_F(ModuleExportsTest, RegisterInPackageLoadedEdgeCases) {
  lua_newtable(L_);
  int table_idx = lua_gettop(L_);

  // 1. When global package is nil
  lua_pushnil(L_);
  lua_setglobal(L_, "package");
  vector_otel::core::RegisterInPackageLoaded(L_, "test_mod_nopkg", table_idx);

  // 2. When global package is a table but package.loaded is not a table
  lua_newtable(L_);
  lua_setglobal(L_, "package");
  vector_otel::core::RegisterInPackageLoaded(L_, "test_mod_noloaded", table_idx);

  lua_pop(L_, 1);
}
