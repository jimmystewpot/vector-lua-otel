#include <gtest/gtest.h>

#include <string>
#include <string_view>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include "vector_otel/native_extractors/native_logs_extractor.hpp"

namespace vector_otel {
namespace native_extractors {
namespace {

class NativeExtractorsTest : public ::testing::Test {
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

    lua_getglobal(L_, "mock_emit");
    ASSERT_TRUE(lua_isfunction(L_, -1));
    emit_idx_ = lua_gettop(L_);
  }

  void TearDown() override {
    if (L_ != nullptr) {
      lua_close(L_);
      L_ = nullptr;
    }
  }

  lua_State* L_{nullptr};
  int emit_idx_{0};
};

TEST_F(NativeExtractorsTest, NativeLogsTable_FullOtlpDecodedEvent) {
  const char* script = R"(
    return {
      timestamp = {
        year = 2026, month = 10, day = 22,
        hour = 10, min = 0, sec = 1, nanosec = 123456789
      },
      observed_timestamp = {
        year = 2026, month = 10, day = 22,
        hour = 10, min = 0, sec = 2, nanosec = 0
      },
      resources = {
        ["service.name"] = "checkout-svc",
        ["host.name"] = "node-1"
      },
      resource_schema_url = "https://opentelemetry.io/schemas/1.21.0",
      scope = {
        name = "my.logger",
        version = "1.2.0",
        schema_url = "https://opentelemetry.io/schemas/scope/1.0",
        attributes = {
          sc_k = "sc_v"
        }
      },
      attributes = {
        ["http.status_code"] = 200,
        max_i = 9223372036854775807,
        nested = { k = "v" }
      },
      trace_id = "5b8efff798038103d269b633813fc60c",
      span_id = "eee19b7ec3c1b174",
      severity_text = "INFO",
      severity_number = 9,
      flags = 4,
      event_name = "checkout.completed",
      message = "hello world"
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK) << lua_tostring(L_, -1);
  int table_idx = lua_gettop(L_);
  int top_before = lua_gettop(L_);

  auto res = ProcessNativeLogsTable(L_, table_idx, emit_idx_);
  EXPECT_EQ(lua_gettop(L_), top_before);
  lua_pop(L_, 1);

  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 1);

  const char* verify = R"(
    assert(#emitted == 1, "Expected 1 emitted event")
    local r = emitted[1].log
    assert(r ~= nil, "emitted[1].log must exist")
    assert(r.Timestamp == "2026-10-22T10:00:01.123456789Z", "Timestamp mismatch: " .. tostring(r.Timestamp))
    assert(r.TraceId == "5b8efff798038103d269b633813fc60c", "TraceId mismatch: " .. tostring(r.TraceId))
    assert(r.SpanId == "eee19b7ec3c1b174", "SpanId mismatch: " .. tostring(r.SpanId))
    assert(r.TraceFlags == 4, "TraceFlags mismatch: " .. tostring(r.TraceFlags))
    assert(r.SeverityText == "INFO", "SeverityText mismatch: " .. tostring(r.SeverityText))
    assert(r.SeverityNumber == 9, "SeverityNumber mismatch: " .. tostring(r.SeverityNumber))
    assert(r.ServiceName == "checkout-svc", "ServiceName mismatch: " .. tostring(r.ServiceName))
    assert(r.Body == "hello world", "Body mismatch: " .. tostring(r.Body))
    assert(r.ResourceSchemaUrl == "https://opentelemetry.io/schemas/1.21.0", "ResourceSchemaUrl mismatch: " .. tostring(r.ResourceSchemaUrl))
    assert(type(r.ResourceAttributes) == "table", "ResourceAttributes must be table")
    assert(r.ResourceAttributes["service.name"] == "checkout-svc", "ResourceAttributes service.name mismatch")
    assert(r.ResourceAttributes["host.name"] == "node-1", "ResourceAttributes host.name mismatch")
    assert(r.ScopeSchemaUrl == "https://opentelemetry.io/schemas/scope/1.0", "ScopeSchemaUrl mismatch: " .. tostring(r.ScopeSchemaUrl))
    assert(r.ScopeName == "my.logger", "ScopeName mismatch: " .. tostring(r.ScopeName))
    assert(r.ScopeVersion == "1.2.0", "ScopeVersion mismatch: " .. tostring(r.ScopeVersion))
    assert(type(r.ScopeAttributes) == "table", "ScopeAttributes must be table")
    assert(r.ScopeAttributes.sc_k == "sc_v", "ScopeAttributes.sc_k mismatch")
    assert(type(r.LogAttributes) == "table", "LogAttributes must be table")
    assert(r.LogAttributes["http.status_code"] == 200, "LogAttributes http.status_code mismatch")
    assert(r.LogAttributes.max_i == 9223372036854775807, "LogAttributes.max_i mismatch")
    assert(r.LogAttributes.nested == '{"k":"v"}', "LogAttributes.nested mismatch: " .. tostring(r.LogAttributes.nested))
    assert(r.EventName == "checkout.completed", "EventName mismatch: " .. tostring(r.EventName))
    assert(type(r.processed_at) == "string" and #r.processed_at == 30, "processed_at invalid: " .. tostring(r.processed_at))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, NativeLogsTable_OuterEventWrapperAndBatchArray) {
  // 1. Outer { log = { message = "wrapped" } }
  {
    const char* script = R"(
      return {
        log = {
          message = "wrapped",
          service_name = "top-svc"
        }
      }
    )";
    ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
    int table_idx = lua_gettop(L_);
    auto res = ProcessNativeLogsTable(L_, table_idx, emit_idx_);
    lua_pop(L_, 1);
    EXPECT_TRUE(res.success) << res.error;
    EXPECT_EQ(res.emitted_count, 1);
  }

  // 2. 2-element batch array { { message = "m1" }, { body = { k = "m2" } } }
  {
    const char* script = R"(
      return {
        { message = "m1" },
        { body = { k = "m2" } }
      }
    )";
    ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
    int table_idx = lua_gettop(L_);
    auto res = ProcessNativeLogsTable(L_, table_idx, emit_idx_);
    lua_pop(L_, 1);
    EXPECT_TRUE(res.success) << res.error;
    EXPECT_EQ(res.emitted_count, 2);
  }

  // 3. Outer { log = { { message = "m3", body = "ignored" } }, source_type = "opentelemetry" }
  // plus metatable __index that must not be invoked by GetTableFieldRaw
  {
    const char* script = R"(
      local outer = {
        source_type = "opentelemetry",
        log = {
          { message = "m3", body = "ignored" }
        }
      }
      setmetatable(outer.log[1], {
        __index = function() error("metamethod should not be called") end
      })
      return outer
    )";
    ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
    int table_idx = lua_gettop(L_);
    auto res = ProcessNativeLogsTable(L_, table_idx, emit_idx_);
    lua_pop(L_, 1);
    EXPECT_TRUE(res.success) << res.error;
    EXPECT_EQ(res.emitted_count, 1);
  }

  const char* verify = R"(
    assert(#emitted == 4, "Expected 4 emitted events, got " .. #emitted)
    assert(emitted[1].log.Body == "wrapped", "Event 1 Body mismatch")
    assert(emitted[1].log.ServiceName == "top-svc", "Event 1 ServiceName mismatch")
    assert(emitted[2].log.Body == "m1", "Event 2 Body mismatch")
    assert(emitted[2].log.ServiceName == "unknown_service", "Event 2 ServiceName mismatch")
    assert(emitted[3].log.Body == '{"k":"m2"}', "Event 3 Body mismatch: " .. tostring(emitted[3].log.Body))
    assert(emitted[4].log.Body == "m3", "Event 4 Body mismatch: " .. tostring(emitted[4].log.Body))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, NativeLogsTable_MissingTimestampFallsBackToObservedThenNow) {
  // 1. Only observed_timestamp present
  {
    const char* script = R"(
      return {
        observed_timestamp = "2026-10-22T12:34:56.789012345Z",
        message = "obs_only"
      }
    )";
    ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
    int table_idx = lua_gettop(L_);
    auto res = ProcessNativeLogsTable(L_, table_idx, emit_idx_);
    lua_pop(L_, 1);
    EXPECT_TRUE(res.success) << res.error;
    EXPECT_EQ(res.emitted_count, 1);
  }

  // 2. Neither timestamp nor observed_timestamp present -> falls back to processed_at (now())
  {
    const char* script = R"(
      return {
        message = "no_ts"
      }
    )";
    ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
    int table_idx = lua_gettop(L_);
    auto res = ProcessNativeLogsTable(L_, table_idx, emit_idx_);
    lua_pop(L_, 1);
    EXPECT_TRUE(res.success) << res.error;
    EXPECT_EQ(res.emitted_count, 1);
  }

  // 3. Explicit epoch 0 timestamp and no observed_timestamp -> "1970-01-01T00:00:00.000000000Z"
  {
    const char* script = R"(
      return {
        timestamp = 0,
        message = "zero_ts"
      }
    )";
    ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
    int table_idx = lua_gettop(L_);
    auto res = ProcessNativeLogsTable(L_, table_idx, emit_idx_);
    lua_pop(L_, 1);
    EXPECT_TRUE(res.success) << res.error;
    EXPECT_EQ(res.emitted_count, 1);
  }

  const char* verify = R"(
    assert(#emitted == 3, "Expected 3 emitted events")
    local r1 = emitted[1].log
    assert(r1.Timestamp == "2026-10-22T12:34:56.789012345Z", "r1.Timestamp mismatch: " .. tostring(r1.Timestamp))
    local r2 = emitted[2].log
    assert(r2.Timestamp == r2.processed_at, "r2.Timestamp should equal processed_at, got " .. tostring(r2.Timestamp) .. " vs " .. tostring(r2.processed_at))
    assert(r2.Timestamp ~= "1970-01-01T00:00:00.000000000Z", "r2.Timestamp should not be epoch 0")
    local r3 = emitted[3].log
    assert(r3.Timestamp == "1970-01-01T00:00:00.000000000Z", "r3.Timestamp should be epoch 0, got " .. tostring(r3.Timestamp))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, NativeLogsTable_EmptyTableReturnsZeroNoEmit) {
  {
    const char* script = "return {}";
    ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
    int table_idx = lua_gettop(L_);
    auto res = ProcessNativeLogsTable(L_, table_idx, emit_idx_);
    lua_pop(L_, 1);
    EXPECT_TRUE(res.success);
    EXPECT_EQ(res.emitted_count, 0);
  }
  {
    const char* script = "return { log = {} }";
    ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
    int table_idx = lua_gettop(L_);
    auto res = ProcessNativeLogsTable(L_, table_idx, emit_idx_);
    lua_pop(L_, 1);
    EXPECT_TRUE(res.success);
    EXPECT_EQ(res.emitted_count, 0);
  }
  {
    lua_pushstring(L_, "not a table");
    int bad_idx = lua_gettop(L_);
    auto res = ProcessNativeLogsTable(L_, bad_idx, emit_idx_);
    lua_pop(L_, 1);
    EXPECT_FALSE(res.success);
    EXPECT_EQ(res.emitted_count, 0);
  }
}

TEST_F(NativeExtractorsTest, NativeLogsJson_FullAndFallback) {
  std::string json =
      R"({"message":"json body","timestamp":"2026-10-22T10:00:01.123456789Z","resources":{"service.name":"json-svc"},"attributes":{"k":42},"scope":{"name":"s","version":"1"}})";
  auto res1 = ProcessNativeLogs(L_, json, emit_idx_);
  EXPECT_TRUE(res1.success) << res1.error;
  EXPECT_EQ(res1.emitted_count, 1);

  auto res2 = ProcessNativeLogs(L_, "{}", emit_idx_);
  EXPECT_TRUE(res2.success) << res2.error;
  EXPECT_EQ(res2.emitted_count, 0);

  std::string json_no_ts = R"({"body":"fallback body","message":"no ts json"})";
  auto res3 = ProcessNativeLogs(L_, json_no_ts, emit_idx_);
  EXPECT_TRUE(res3.success) << res3.error;
  EXPECT_EQ(res3.emitted_count, 1);

  std::string json_arr =
      R"([{"message":"arr1","observed_timestamp":"2026-10-22T11:00:00.000000000Z"},{"body":{"nested":true},"service_name":"arr-svc"}])";
  auto res4 = ProcessNativeLogs(L_, json_arr, emit_idx_);
  EXPECT_TRUE(res4.success) << res4.error;
  EXPECT_EQ(res4.emitted_count, 2);

  std::string json_wrapped_with_src =
      R"({"log":{"message":"wrapped_json","resource":{"service.name":"old-svc"},"resources":{"host":"h1"}},"source_type":"opentelemetry"})";
  auto res5 = ProcessNativeLogs(L_, json_wrapped_with_src, emit_idx_);
  EXPECT_TRUE(res5.success) << res5.error;
  EXPECT_EQ(res5.emitted_count, 1);

  auto res_bad = ProcessNativeLogs(L_, "{broken", emit_idx_);
  EXPECT_FALSE(res_bad.success);
  EXPECT_EQ(res_bad.emitted_count, 0);

  const char* verify = R"(
    assert(#emitted == 5, "Expected 5 emitted events, got " .. #emitted)
    local r1 = emitted[1].log
    assert(r1.Body == "json body", "r1.Body mismatch: " .. tostring(r1.Body))
    assert(r1.Timestamp == "2026-10-22T10:00:01.123456789Z", "r1.Timestamp mismatch: " .. tostring(r1.Timestamp))
    assert(r1.ServiceName == "json-svc", "r1.ServiceName mismatch: " .. tostring(r1.ServiceName))
    assert(r1.ResourceAttributes["service.name"] == "json-svc", "r1.ResourceAttributes mismatch")
    assert(r1.LogAttributes.k == 42, "r1.LogAttributes.k mismatch")
    assert(r1.ScopeName == "s", "r1.ScopeName mismatch")
    assert(r1.ScopeVersion == "1", "r1.ScopeVersion mismatch")

    local r2 = emitted[2].log
    assert(r2.Body == "no ts json", "r2.Body mismatch: " .. tostring(r2.Body))
    assert(r2.Timestamp == r2.processed_at, "r2.Timestamp should equal processed_at")
    assert(r2.ServiceName == "unknown_service", "r2.ServiceName mismatch")

    local r3 = emitted[3].log
    assert(r3.Body == "arr1", "r3.Body mismatch")
    assert(r3.Timestamp == "2026-10-22T11:00:00.000000000Z", "r3.Timestamp mismatch")

    local r4 = emitted[4].log
    assert(r4.Body == '{"nested":true}', "r4.Body mismatch: " .. tostring(r4.Body))
    assert(r4.ServiceName == "arr-svc", "r4.ServiceName mismatch: " .. tostring(r4.ServiceName))

    local r5 = emitted[5].log
    assert(r5.Body == "wrapped_json", "r5.Body mismatch: " .. tostring(r5.Body))
    assert(r5.ServiceName == "unknown_service", "r5.ServiceName mismatch: " .. tostring(r5.ServiceName))
    assert(r5.ResourceAttributes.host == "h1", "r5.ResourceAttributes.host mismatch")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

}  // namespace
}  // namespace native_extractors
}  // namespace vector_otel
