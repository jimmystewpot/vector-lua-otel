#include <gtest/gtest.h>

#include <string>

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
  EXPECT_STREQ(lua_tostring(L_, -1), "0.1.0");
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

// Test for Lua memory error recovery (Task 5: re-entrancy guard survives OOM longjmp)
TEST(ProcessLogsRecoversFromLuaMemoryErrorWithoutStickingReentrancyGuard,
     OOMDuringExtractionDoesNotStickReentrancyFlag) {
  const char* valid_payload =
      R"({"resourceLogs":[{"resource":{"attributes":[{"key":"service.name","value":{"stringValue":"svc"}}]},"scopeLogs":[{"scope":{"name":"s"},"logRecords":[{"timeUnixNano":"1293840000000000000","body":{"stringValue":"hello"}}]}]}]})";

  struct Budget {
    size_t remaining = 0;
  };

  // Sweep budget to find where OOM happens inside extraction
  int found_stuck_budget = -1;
  for (size_t budget = 0; budget < 3001; budget += (budget < 500 ? 1 : 7)) {
    Budget budget_data{budget};
    lua_State* L = lua_newstate(
        [](void* ud, void* ptr, size_t osize, size_t nsize) -> void* {
          Budget* b = static_cast<Budget*>(ud);
          if (nsize == 0) {
            free(ptr);
            return nullptr;
          }
          if (nsize > osize) {
            size_t grow = nsize - osize;
            if (grow > b->remaining) return nullptr;
            b->remaining -= grow;
          }
          return realloc(ptr, nsize);
        },
        &budget_data);

    if (L == nullptr) continue;

    luaL_openlibs(L);
    const char* init = "function noop_emit(e) end";
    if (luaL_dostring(L, init) != LUA_OK) {
      lua_close(L);
      continue;
    }

    if (luaopen_vector_otel(L) != 0) {
      lua_close(L);
      continue;
    }
    lua_setglobal(L, "vector_otel");

    // First call with limited budget (may fail)
    budget_data.remaining = budget;
    lua_pushlstring(L, valid_payload, strlen(valid_payload));
    lua_getglobal(L, "noop_emit");
    lua_call(L, 2, 3);
    lua_pop(L, 3);

    // Second call with unlimited budget
    budget_data.remaining = SIZE_MAX;
    lua_gc(L, LUA_GCCOLLECT, 0);

    lua_pushlstring(L, valid_payload, strlen(valid_payload));
    lua_getglobal(L, "noop_emit");
    lua_call(L, 2, 3);

    // Check result
    bool is_nil = lua_isnil(L, -3);
    const char* err_msg = lua_tostring(L, -2);
    bool is_reentrancy_error = err_msg && strstr(err_msg, "re-entrant");

    if (is_nil && is_reentrancy_error && found_stuck_budget < 0) {
      found_stuck_budget = budget;
      printf("FOUND STUCK BUDGET: %zu\n", budget);
    }

    lua_pop(L, 3);
    lua_close(L);
  }

  if (found_stuck_budget >= 0) {
    FAIL() << "Re-entrancy guard stuck at budget=" << found_stuck_budget;
  }
}
