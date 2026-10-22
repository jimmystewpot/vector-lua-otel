#include <gtest/gtest.h>

#include <string>
#include <string_view>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include "vector_otel/native_extractors/native_logs_extractor.hpp"
#include "vector_otel/native_extractors/native_metrics_extractor.hpp"
#include "vector_otel/native_extractors/native_traces_extractor.hpp"

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
    const char* script = R"(return { source_type = "opentelemetry", log = {} })";
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

TEST_F(NativeExtractorsTest, NativeTracesTable_FullSpanFromTraceToLog) {
  const char* script = R"(
    return {
      source_type = "opentelemetry",
      log = {
        trace_id = "4bf92f3577b34da6a3ce929d0e0e4736",
        span_id = "00f067aa0ba902b7",
        parent_span_id = "b9c7c989f97918e1",
        trace_state = "congo=t61rcWkgMzE",
        name = "HTTP POST",
        kind = 2,
        start_time_unix_nano = {
          year = 2026, month = 10, day = 22,
          hour = 10, min = 0, sec = 1, nanosec = 0
        },
        end_time_unix_nano = {
          year = 2026, month = 10, day = 22,
          hour = 10, min = 0, sec = 1, nanosec = 50000000
        },
        status = {
          code = 1,
          message = "ok"
        },
        resources = {
          ["service.name"] = "trace-svc",
          ["deployment.environment"] = "prod"
        },
        scope = {
          name = "otel.tracer",
          version = "2.1.0"
        },
        attributes = {
          ["http.method"] = "POST",
          ["http.status_code"] = 201
        },
        events = {
          {
            name = "ev1",
            time_unix_nano = {
              year = 2026, month = 10, day = 22,
              hour = 10, min = 0, sec = 1, nanosec = 5000000
            },
            attributes = {
              ek = "ev_val"
            }
          }
        },
        links = {
          {
            trace_id = "t2",
            span_id = "s2",
            trace_state = "ts",
            attributes = {
              lk = 1
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK) << lua_tostring(L_, -1);
  int table_idx = lua_gettop(L_);
  int top_before = lua_gettop(L_);

  auto res = ProcessNativeTracesTable(L_, table_idx, emit_idx_);
  EXPECT_EQ(lua_gettop(L_), top_before);
  lua_pop(L_, 1);

  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 1);

  const char* verify = R"(
    assert(#emitted == 1, "Expected 1 emitted span, got " .. #emitted)
    local r = emitted[1].log
    assert(r ~= nil, "emitted[1].log must exist")
    assert(r.Timestamp == "2026-10-22T10:00:01.000000000Z", "Timestamp mismatch: " .. tostring(r.Timestamp))
    assert(r.TraceId == "4bf92f3577b34da6a3ce929d0e0e4736", "TraceId mismatch: " .. tostring(r.TraceId))
    assert(r.SpanId == "00f067aa0ba902b7", "SpanId mismatch: " .. tostring(r.SpanId))
    assert(r.ParentSpanId == "b9c7c989f97918e1", "ParentSpanId mismatch: " .. tostring(r.ParentSpanId))
    assert(r.TraceState == "congo=t61rcWkgMzE", "TraceState mismatch: " .. tostring(r.TraceState))
    assert(r.SpanName == "HTTP POST", "SpanName mismatch: " .. tostring(r.SpanName))
    assert(r.SpanKind == "SPAN_KIND_SERVER", "SpanKind mismatch: " .. tostring(r.SpanKind))
    assert(r.ServiceName == "trace-svc", "ServiceName mismatch: " .. tostring(r.ServiceName))
    assert(type(r.ResourceAttributes) == "table", "ResourceAttributes must be table")
    assert(r.ResourceAttributes["service.name"] == "trace-svc", "ResourceAttributes service.name mismatch")
    assert(r.ResourceAttributes["deployment.environment"] == "prod", "ResourceAttributes deployment.environment mismatch")
    assert(r.ScopeName == "otel.tracer", "ScopeName mismatch: " .. tostring(r.ScopeName))
    assert(r.ScopeVersion == "2.1.0", "ScopeVersion mismatch: " .. tostring(r.ScopeVersion))
    assert(type(r.SpanAttributes) == "table", "SpanAttributes must be table")
    assert(r.SpanAttributes["http.method"] == "POST", "SpanAttributes http.method mismatch")
    assert(r.SpanAttributes["http.status_code"] == 201, "SpanAttributes http.status_code mismatch")
    assert(r.Duration == 50000000, "Duration mismatch: " .. tostring(r.Duration))
    assert(r.StatusCode == "STATUS_CODE_OK", "StatusCode mismatch: " .. tostring(r.StatusCode))
    assert(r.StatusMessage == "ok", "StatusMessage mismatch: " .. tostring(r.StatusMessage))
    assert(type(r.Events) == "string", "Events must be JSON string")
    assert(string.find(r.Events, '"Timestamp":"2026-10-22T10:00:01.005000000Z"', 1, true) ~= nil, "Events Timestamp missing: " .. tostring(r.Events))
    assert(string.find(r.Events, '"Name":"ev1"', 1, true) ~= nil, "Events Name missing: " .. tostring(r.Events))
    assert(string.find(r.Events, '"ek":"ev_val"', 1, true) ~= nil, "Events Attributes missing: " .. tostring(r.Events))
    assert(type(r.Links) == "string", "Links must be JSON string")
    assert(string.find(r.Links, '"TraceId":"t2"', 1, true) ~= nil, "Links TraceId missing: " .. tostring(r.Links))
    assert(string.find(r.Links, '"SpanId":"s2"', 1, true) ~= nil, "Links SpanId missing: " .. tostring(r.Links))
    assert(string.find(r.Links, '"TraceState":"ts"', 1, true) ~= nil, "Links TraceState missing: " .. tostring(r.Links))
    assert(string.find(r.Links, '"lk":1', 1, true) ~= nil, "Links Attributes missing: " .. tostring(r.Links))
    assert(type(r.processed_at) == "string" and #r.processed_at == 30, "processed_at invalid: " .. tostring(r.processed_at))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);

  // Also test batch array inside { trace = { ... } } with metatable __index guard
  const char* batch_script = R"(
    local outer = {
      source_type = "opentelemetry",
      trace = {
        {
          name = "span-batch-1",
          kind = "PRODUCER",
          status_code = "OK",
          status_message = "top-msg",
          service_name = "top-trace-svc",
          events = {
            {
              name = "kv-ev",
              timestamp = "2026-10-22T10:00:02.000000000Z",
              attributes = {
                { key = "kv_k", value = { stringValue = "kv_v" } }
              }
            }
          }
        },
        {
          span_name = "span-batch-2",
          span_kind = 5
        }
      }
    }
    setmetatable(outer.trace[1], {
      __index = function() error("metamethod must not be called") end
    })
    return outer
  )";
  ASSERT_EQ(luaL_dostring(L_, batch_script), LUA_OK) << lua_tostring(L_, -1);
  int batch_idx = lua_gettop(L_);
  auto batch_res = ProcessNativeTracesTable(L_, batch_idx, emit_idx_);
  lua_pop(L_, 1);
  EXPECT_TRUE(batch_res.success) << batch_res.error;
  EXPECT_EQ(batch_res.emitted_count, 2);

  const char* verify_batch = R"(
    assert(#emitted == 3, "Expected 3 total emitted spans, got " .. #emitted)
    local b1 = emitted[2].log
    assert(b1.SpanName == "span-batch-1", "b1.SpanName mismatch")
    assert(b1.SpanKind == "SPAN_KIND_PRODUCER", "b1.SpanKind mismatch: " .. tostring(b1.SpanKind))
    assert(b1.StatusCode == "STATUS_CODE_OK", "b1.StatusCode mismatch: " .. tostring(b1.StatusCode))
    assert(b1.StatusMessage == "top-msg", "b1.StatusMessage mismatch: " .. tostring(b1.StatusMessage))
    assert(b1.ServiceName == "top-trace-svc", "b1.ServiceName mismatch: " .. tostring(b1.ServiceName))
    assert(string.find(b1.Events, '"kv_k":"kv_v"', 1, true) ~= nil, "b1.Events KV mismatch: " .. tostring(b1.Events))
    local b2 = emitted[3].log
    assert(b2.SpanName == "span-batch-2", "b2.SpanName mismatch")
    assert(b2.SpanKind == "SPAN_KIND_CONSUMER", "b2.SpanKind mismatch: " .. tostring(b2.SpanKind))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_batch), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, NativeTracesTable_MissingStartTimestampFallsBackToIngestThenNow) {
  // 1. Only ingest_timestamp present -> Timestamp uses ingest_timestamp, Duration == 0
  {
    const char* script = R"(
      return {
        name = "ingest-span",
        ingest_timestamp = "2026-10-22T12:00:00.123456789Z",
        end_time_unix_nano = "2026-10-22T12:00:01.000000000Z"
      }
    )";
    ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
    int table_idx = lua_gettop(L_);
    auto res = ProcessNativeTracesTable(L_, table_idx, emit_idx_);
    lua_pop(L_, 1);
    EXPECT_TRUE(res.success) << res.error;
    EXPECT_EQ(res.emitted_count, 1);
  }

  // 2. Neither start_time_unix_nano nor ingest_timestamp -> Timestamp == processed_at, Duration ==
  // 0
  {
    const char* script = R"(
      return {
        name = "now-span",
        kind = "client",
        status = { code = "error", message = "boom" }
      }
    )";
    ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
    int table_idx = lua_gettop(L_);
    auto res = ProcessNativeTracesTable(L_, table_idx, emit_idx_);
    lua_pop(L_, 1);
    EXPECT_TRUE(res.success) << res.error;
    EXPECT_EQ(res.emitted_count, 1);
  }

  // 3. Explicit epoch 0 start_time_unix_nano -> Timestamp == "1970-01-01T00:00:00.000000000Z",
  // Duration == 0
  {
    const char* script = R"(
      return {
        name = "epoch-zero-span",
        start_time_unix_nano = 0,
        end_time_unix_nano = 100
      }
    )";
    ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
    int table_idx = lua_gettop(L_);
    auto res = ProcessNativeTracesTable(L_, table_idx, emit_idx_);
    lua_pop(L_, 1);
    EXPECT_TRUE(res.success) << res.error;
    EXPECT_EQ(res.emitted_count, 1);
  }

  const char* verify = R"(
    assert(#emitted == 3, "Expected 3 emitted spans, got " .. #emitted)
    local r1 = emitted[1].log
    assert(r1.Timestamp == "2026-10-22T12:00:00.123456789Z", "r1.Timestamp mismatch: " .. tostring(r1.Timestamp))
    assert(r1.Duration == 0, "r1.Duration mismatch: " .. tostring(r1.Duration))
    assert(r1.Events == "[]", "r1.Events mismatch: " .. tostring(r1.Events))
    assert(r1.Links == "[]", "r1.Links mismatch: " .. tostring(r1.Links))

    local r2 = emitted[2].log
    assert(r2.Timestamp == r2.processed_at, "r2.Timestamp should equal processed_at, got " .. tostring(r2.Timestamp) .. " vs " .. tostring(r2.processed_at))
    assert(r2.Duration == 0, "r2.Duration mismatch: " .. tostring(r2.Duration))
    assert(r2.SpanKind == "SPAN_KIND_CLIENT", "r2.SpanKind mismatch: " .. tostring(r2.SpanKind))
    assert(r2.StatusCode == "STATUS_CODE_ERROR", "r2.StatusCode mismatch: " .. tostring(r2.StatusCode))
    assert(r2.StatusMessage == "boom", "r2.StatusMessage mismatch: " .. tostring(r2.StatusMessage))

    local r3 = emitted[3].log
    assert(r3.Timestamp == "1970-01-01T00:00:00.000000000Z", "r3.Timestamp mismatch: " .. tostring(r3.Timestamp))
    assert(r3.Duration == 0, "r3.Duration mismatch: " .. tostring(r3.Duration))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, NativeTracesTable_EmptyTableReturnsZero) {
  {
    const char* script = "return {}";
    ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
    int table_idx = lua_gettop(L_);
    auto res = ProcessNativeTracesTable(L_, table_idx, emit_idx_);
    lua_pop(L_, 1);
    EXPECT_TRUE(res.success);
    EXPECT_EQ(res.emitted_count, 0);
  }
  {
    const char* script = "return { log = {} }";
    ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
    int table_idx = lua_gettop(L_);
    auto res = ProcessNativeTracesTable(L_, table_idx, emit_idx_);
    lua_pop(L_, 1);
    EXPECT_TRUE(res.success);
    EXPECT_EQ(res.emitted_count, 0);
  }
  {
    const char* script = "return { trace = {} }";
    ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
    int table_idx = lua_gettop(L_);
    auto res = ProcessNativeTracesTable(L_, table_idx, emit_idx_);
    lua_pop(L_, 1);
    EXPECT_TRUE(res.success);
    EXPECT_EQ(res.emitted_count, 0);
  }
  {
    lua_pushstring(L_, "not a table");
    int bad_idx = lua_gettop(L_);
    auto res = ProcessNativeTracesTable(L_, bad_idx, emit_idx_);
    lua_pop(L_, 1);
    EXPECT_FALSE(res.success);
    EXPECT_EQ(res.emitted_count, 0);
  }
}

TEST_F(NativeExtractorsTest, NativeTracesJson_FullAndDurationClamp) {
  std::string json1 = R"({
    "log": {
      "trace_id": "t-json-1",
      "span_id": "s-json-1",
      "parent_span_id": "p-json-1",
      "trace_state": "ts-1",
      "name": "GET /items",
      "kind": "SERVER",
      "start_time_unix_nano": "2026-10-22T10:00:01.000000000Z",
      "end_time_unix_nano": "2026-10-22T10:00:01.025000000Z",
      "status": { "code": 2, "message": "failed" },
      "resources": { "service.name": "json-trace-svc" },
      "scope": { "name": "json.scope", "version": "1.0.0" },
      "attributes": { "rpc.system": "grpc" },
      "events": [
        {
          "name": "exception",
          "time_unix_nano": "2026-10-22T10:00:01.010000000Z",
          "attributes": {
            "exception.type": "RuntimeError",
            "otlp_str": { "stringValue": "otlp_val" },
            "nested_obj": { "code": 500, "retry": false }
          }
        }
      ],
      "links": [
        {
          "trace_id": "lt1",
          "span_id": "ls1",
          "trace_state": "lts1",
          "attributes": [
            { "key": "link.k", "value": { "stringValue": "v" } },
            { "key": "link.nested", "value": { "inner": 42 } }
          ]
        }
      ]
    },
    "source_type": "opentelemetry"
  })";
  auto res1 = ProcessNativeTraces(L_, json1, emit_idx_);
  EXPECT_TRUE(res1.success) << res1.error;
  EXPECT_EQ(res1.emitted_count, 1);

  // Duration clamping when (end_ns - start_ns) > INT64_MAX, plus resource service_name reset
  std::string json_clamp = R"({
    "name": "clamp-span",
    "start_time_unix_nano": 1,
    "end_time_unix_nano": 18446744073709551615,
    "resource": { "service.name": "old-svc" },
    "resources": { "host": "h1" }
  })";
  auto res2 = ProcessNativeTraces(L_, json_clamp, emit_idx_);
  EXPECT_TRUE(res2.success) << res2.error;
  EXPECT_EQ(res2.emitted_count, 1);

  auto res_empty = ProcessNativeTraces(L_, "{}", emit_idx_);
  EXPECT_TRUE(res_empty.success);
  EXPECT_EQ(res_empty.emitted_count, 0);

  auto res_bad = ProcessNativeTraces(L_, "{broken", emit_idx_);
  EXPECT_FALSE(res_bad.success);
  EXPECT_EQ(res_bad.emitted_count, 0);

  const char* verify = R"(
    assert(#emitted == 2, "Expected 2 emitted spans, got " .. #emitted)
    local r1 = emitted[1].log
    assert(r1.Timestamp == "2026-10-22T10:00:01.000000000Z", "r1.Timestamp mismatch: " .. tostring(r1.Timestamp))
    assert(r1.TraceId == "t-json-1", "r1.TraceId mismatch")
    assert(r1.SpanId == "s-json-1", "r1.SpanId mismatch")
    assert(r1.ParentSpanId == "p-json-1", "r1.ParentSpanId mismatch")
    assert(r1.TraceState == "ts-1", "r1.TraceState mismatch")
    assert(r1.SpanName == "GET /items", "r1.SpanName mismatch")
    assert(r1.SpanKind == "SPAN_KIND_SERVER", "r1.SpanKind mismatch: " .. tostring(r1.SpanKind))
    assert(r1.ServiceName == "json-trace-svc", "r1.ServiceName mismatch: " .. tostring(r1.ServiceName))
    assert(r1.ResourceAttributes["service.name"] == "json-trace-svc", "r1.ResourceAttributes mismatch")
    assert(r1.ScopeName == "json.scope", "r1.ScopeName mismatch")
    assert(r1.ScopeVersion == "1.0.0", "r1.ScopeVersion mismatch")
    assert(r1.SpanAttributes["rpc.system"] == "grpc", "r1.SpanAttributes mismatch")
    assert(r1.Duration == 25000000, "r1.Duration mismatch: " .. tostring(r1.Duration))
    assert(r1.StatusCode == "STATUS_CODE_ERROR", "r1.StatusCode mismatch: " .. tostring(r1.StatusCode))
    assert(r1.StatusMessage == "failed", "r1.StatusMessage mismatch: " .. tostring(r1.StatusMessage))
    assert(string.find(r1.Events, '"Timestamp":"2026-10-22T10:00:01.010000000Z"', 1, true) ~= nil, "r1.Events Timestamp mismatch: " .. tostring(r1.Events))
    assert(string.find(r1.Events, '"Name":"exception"', 1, true) ~= nil, "r1.Events Name mismatch: " .. tostring(r1.Events))
    assert(string.find(r1.Events, '"exception.type":"RuntimeError"', 1, true) ~= nil, "r1.Events Attributes mismatch: " .. tostring(r1.Events))
    assert(string.find(r1.Events, '"otlp_str":"otlp_val"', 1, true) ~= nil, "r1.Events OTLP AnyValue mismatch: " .. tostring(r1.Events))
    assert(string.find(r1.Events, '"nested_obj":{"code":500,"retry":false}', 1, true) ~= nil, "r1.Events nested object mismatch: " .. tostring(r1.Events))
    assert(string.find(r1.Links, '"TraceId":"lt1"', 1, true) ~= nil, "r1.Links TraceId mismatch: " .. tostring(r1.Links))
    assert(string.find(r1.Links, '"link.k":"v"', 1, true) ~= nil, "r1.Links OTLP KV mismatch: " .. tostring(r1.Links))
    assert(string.find(r1.Links, '"link.nested":{"inner":42}', 1, true) ~= nil, "r1.Links nested object mismatch: " .. tostring(r1.Links))

    local r2 = emitted[2].log
    assert(r2.Duration == 9223372036854775807, "r2.Duration should clamp to INT64_MAX, got " .. tostring(r2.Duration))
    assert(r2.ServiceName == "unknown_service", "r2.ServiceName should reset to unknown_service, got " .. tostring(r2.ServiceName))
    assert(r2.ResourceAttributes.host == "h1", "r2.ResourceAttributes.host mismatch")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, NativeMetricsTable_CounterAndGaugeAndTagSplitting) {
  const char* script1 = R"(
    local ev = {
      metric = {
        name = "req_total",
        namespace = "http",
        kind = "incremental",
        description = "HTTP requests counter",
        unit = "1",
        flags = 2,
        timestamp = {
          year = 2026, month = 10, day = 22,
          hour = 10, min = 0, sec = 15, nanosec = 500000000
        },
        interval_ms = 10000,
        tags = {
          ["resource.service.name"] = "pay-svc",
          ["resource.host.name"] = "h1",
          ["scope.name"] = "meter.http",
          ["scope.version"] = "2.0",
          ["scope.attr"] = "sval",
          ["http.method"] = "GET"
        },
        counter = {
          value = 42.5
        }
      }
    }
    setmetatable(ev.metric, {
      __index = function() error("metamethod must not be called") end
    })
    return ev
  )";
  ASSERT_EQ(luaL_dostring(L_, script1), LUA_OK) << lua_tostring(L_, -1);
  int table_idx = lua_gettop(L_);
  int top_before = lua_gettop(L_);

  auto res1 = ProcessNativeMetricsTable(L_, table_idx, emit_idx_);
  EXPECT_EQ(lua_gettop(L_), top_before);
  lua_pop(L_, 1);

  EXPECT_TRUE(res1.success) << res1.error;
  EXPECT_EQ(res1.emitted_count, 1);

  const char* script2 = R"(
    return {
      name = "cpu_usage",
      kind = "absolute",
      timestamp = "2026-10-22T10:00:20.000000000Z",
      tags = {
        ["resource.service.name"] = "gauge-svc",
        core = "0"
      },
      gauge = {
        value = 99.25
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, script2), LUA_OK) << lua_tostring(L_, -1);
  table_idx = lua_gettop(L_);
  auto res2 = ProcessNativeMetricsTable(L_, table_idx, emit_idx_);
  lua_pop(L_, 1);

  EXPECT_TRUE(res2.success) << res2.error;
  EXPECT_EQ(res2.emitted_count, 1);

  const char* verify = R"(
    assert(#emitted == 2, "Expected 2 emitted metrics, got " .. #emitted)
    local r1 = emitted[1].log
    assert(r1 ~= nil, "emitted[1].log must exist")
    assert(r1.MetricName == "http.req_total", "MetricName mismatch: " .. tostring(r1.MetricName))
    assert(r1.MetricDescription == "HTTP requests counter", "MetricDescription mismatch: " .. tostring(r1.MetricDescription))
    assert(r1.MetricUnit == "1", "MetricUnit mismatch: " .. tostring(r1.MetricUnit))
    assert(r1.MetricType == "sum", "MetricType mismatch: " .. tostring(r1.MetricType))
    assert(r1.ServiceName == "pay-svc", "ServiceName mismatch: " .. tostring(r1.ServiceName))
    assert(type(r1.ResourceAttributes) == "table", "ResourceAttributes must be table")
    assert(r1.ResourceAttributes["service.name"] == "pay-svc", "ResourceAttributes service.name mismatch")
    assert(r1.ResourceAttributes["host.name"] == "h1", "ResourceAttributes host.name mismatch")
    assert(r1.ScopeName == "meter.http", "ScopeName mismatch: " .. tostring(r1.ScopeName))
    assert(r1.ScopeVersion == "2.0", "ScopeVersion mismatch: " .. tostring(r1.ScopeVersion))
    assert(type(r1.ScopeAttributes) == "table", "ScopeAttributes must be table")
    assert(r1.ScopeAttributes.attr == "sval", "ScopeAttributes.attr mismatch: " .. tostring(r1.ScopeAttributes.attr))
    assert(r1.ScopeAttributes.name == nil, "scope.name must not leak into ScopeAttributes")
    assert(r1.ScopeAttributes.version == nil, "scope.version must not leak into ScopeAttributes")
    assert(type(r1.Attributes) == "table", "Attributes must be table")
    assert(r1.Attributes["http.method"] == "GET", "Attributes http.method mismatch")
    assert(r1.Attributes["resource.service.name"] == nil, "resource.* must not leak into Attributes")
    assert(r1.Attributes["scope.attr"] == nil, "scope.* must not leak into Attributes")
    assert(math.abs(r1.Value - 42.5) < 1e-6, "Value mismatch: " .. tostring(r1.Value))
    assert(r1.Histogram == nil, "Histogram must be nil for sum")
    assert(r1.TimeUnix == "2026-10-22T10:00:15.500000000Z", "TimeUnix mismatch: " .. tostring(r1.TimeUnix))
    assert(r1.StartTimeUnix == "2026-10-22T10:00:05.500000000Z", "StartTimeUnix mismatch: " .. tostring(r1.StartTimeUnix))
    assert(r1.Flags == 2, "Flags mismatch: " .. tostring(r1.Flags))
    assert(type(r1.Exemplars) == "table", "Exemplars must be table")
    assert(type(r1.processed_at) == "string" and #r1.processed_at == 30, "processed_at invalid")

    local r2 = emitted[2].log
    assert(r2.MetricName == "cpu_usage", "r2.MetricName mismatch: " .. tostring(r2.MetricName))
    assert(r2.MetricType == "gauge", "r2.MetricType mismatch: " .. tostring(r2.MetricType))
    assert(r2.ServiceName == "gauge-svc", "r2.ServiceName mismatch")
    assert(r2.Attributes.core == "0", "r2.Attributes.core mismatch")
    assert(math.abs(r2.Value - 99.25) < 1e-6, "r2.Value mismatch")
    assert(r2.StartTimeUnix == "", "r2.StartTimeUnix should be empty when interval_ms omitted, got " .. tostring(r2.StartTimeUnix))
    assert(r2.TimeUnix == "2026-10-22T10:00:20.000000000Z", "r2.TimeUnix mismatch")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, NativeMetricsTable_HistogramParallelArraysStripsInfinity) {
  const char* script = R"(
    return {
      metric = {
        name = "req_latency",
        namespace = "rpc",
        timestamp = "2026-10-22T10:01:00.000000000Z",
        tags = {
          ["resource.service.name"] = "hist-svc",
          endpoint = "/pay"
        },
        aggregated_histogram = {
          buckets = { 10.0, 50.0, 100.0, math.huge },
          counts = { 1, 3, 5, 2 },
          count = 11,
          sum = 450.5
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK) << lua_tostring(L_, -1);
  int table_idx = lua_gettop(L_);
  auto res = ProcessNativeMetricsTable(L_, table_idx, emit_idx_);
  lua_pop(L_, 1);

  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 1);

  const char* verify = R"(
    assert(#emitted == 1, "Expected 1 emitted metric, got " .. #emitted)
    local r = emitted[1].log
    assert(r.MetricName == "rpc.req_latency", "MetricName mismatch: " .. tostring(r.MetricName))
    assert(r.MetricType == "histogram", "MetricType mismatch: " .. tostring(r.MetricType))
    assert(r.Value == nil, "Value must be nil for histogram")
    assert(type(r.Histogram) == "table", "Histogram must be table")
    assert(r.Histogram.Count == 11 and r.Histogram.count == 11, "Histogram.Count mismatch: " .. tostring(r.Histogram.Count))
    assert(math.abs(r.Histogram.Sum - 450.5) < 1e-6 and math.abs(r.Histogram.sum - 450.5) < 1e-6, "Histogram.Sum mismatch")
    assert(#r.Histogram.ExplicitBounds == 3, "ExplicitBounds length must be 3 (math.huge stripped), got " .. #r.Histogram.ExplicitBounds)
    assert(#r.Histogram.explicitBounds == 3, "explicitBounds length must be 3")
    assert(r.Histogram.ExplicitBounds[1] == 10.0 and r.Histogram.ExplicitBounds[2] == 50.0 and r.Histogram.ExplicitBounds[3] == 100.0, "ExplicitBounds values mismatch")
    assert(#r.Histogram.BucketCounts == 4, "BucketCounts length must be 4, got " .. #r.Histogram.BucketCounts)
    assert(#r.Histogram.bucketCounts == 4, "bucketCounts length must be 4")
    assert(r.Histogram.BucketCounts[1] == 1 and r.Histogram.BucketCounts[2] == 3 and r.Histogram.BucketCounts[3] == 5 and r.Histogram.BucketCounts[4] == 2, "BucketCounts values mismatch")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, NativeMetricsTable_HistogramObjectArrayAndSummary) {
  const char* script = R"(
    return {
      log = {
        {
          name = "hist_obj_buckets",
          timestamp = "2026-10-22T10:02:00.000000000Z",
          aggregated_histogram = {
            buckets = {
              { upper_limit = 10.0, count = 1 },
              { upper_limit = math.huge, count = 2 }
            },
            count = "18446744073709551615",
            sum = 25.0,
            min = 1.5,
            max = 20.0
          }
        },
        {
          name = "summary_metric",
          timestamp = "2026-10-22T10:02:01.000000000Z",
          tags = {
            ["resource.service.name"] = "sum-svc",
            route = "/login"
          },
          aggregated_summary = {
            quantiles = { 0.5, 0.99 },
            values = { 12.3, 99.1 },
            count = 10,
            sum = 200.0
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK) << lua_tostring(L_, -1);
  int table_idx = lua_gettop(L_);
  auto res = ProcessNativeMetricsTable(L_, table_idx, emit_idx_);
  lua_pop(L_, 1);

  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 2);

  const char* verify = R"(
    assert(#emitted == 2, "Expected 2 emitted metrics, got " .. #emitted)
    local r1 = emitted[1].log
    assert(r1.MetricName == "hist_obj_buckets", "r1.MetricName mismatch")
    assert(r1.MetricType == "histogram", "r1.MetricType mismatch")
    assert(r1.Value == nil, "r1.Value must be nil")
    assert(r1.Histogram.Count == 9223372036854775807, "r1.Histogram.Count should clamp to INT64_MAX")
    assert(#r1.Histogram.ExplicitBounds == 1 and r1.Histogram.ExplicitBounds[1] == 10.0, "r1.ExplicitBounds mismatch")
    assert(#r1.Histogram.BucketCounts == 2 and r1.Histogram.BucketCounts[1] == 1 and r1.Histogram.BucketCounts[2] == 2, "r1.BucketCounts mismatch")
    assert(r1.Histogram.Min == 1.5 and r1.Histogram.Max == 20.0, "r1.Min/Max mismatch")

    local r2 = emitted[2].log
    assert(r2.MetricName == "summary_metric", "r2.MetricName mismatch")
    assert(r2.MetricType == "summary", "r2.MetricType mismatch")
    assert(r2.ServiceName == "sum-svc", "r2.ServiceName mismatch")
    assert(r2.Attributes.route == "/login", "r2.Attributes.route mismatch")
    assert(r2.Value == nil, "r2.Value must be nil for summary")
    assert(r2.Histogram == nil, "r2.Histogram must be nil for summary")
    assert(type(r2.Summary) == "table", "r2.Summary must be table")
    assert(r2.Summary.Count == 10 and r2.Summary.count == 10, "r2.Summary.Count mismatch")
    assert(math.abs(r2.Summary.Sum - 200.0) < 1e-6, "r2.Summary.Sum mismatch")
    assert(#r2.Summary.QuantileValues == 2, "r2.Summary.QuantileValues length mismatch")
    assert(math.abs(r2.Summary.QuantileValues[1].Quantile - 0.5) < 1e-6 and math.abs(r2.Summary.QuantileValues[1].Value - 12.3) < 1e-6, "r2.Summary q1 mismatch")
    assert(math.abs(r2.Summary.QuantileValues[2].Quantile - 0.99) < 1e-6 and math.abs(r2.Summary.QuantileValues[2].Value - 99.1) < 1e-6, "r2.Summary q2 mismatch")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, NativeMetricsTable_MultiValueTagModeFull) {
  const char* script = R"(
    return {
      name = "mv_counter",
      timestamp = "2026-10-22T10:03:00.000000000Z",
      tags = {
        ["resource.service.name"] = { "old-svc", "final-svc" },
        ["resource.region"] = { "us-west-1", "us-east-1" },
        ["scope.name"] = { "my-scope" },
        ["scope.version"] = { "0.1", "1.0" },
        ["scope.env"] = { "dev", "prod-scope" },
        env = { " staging", "prod" }
      },
      counter = {
        value = 7
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK) << lua_tostring(L_, -1);
  int table_idx = lua_gettop(L_);
  auto res = ProcessNativeMetricsTable(L_, table_idx, emit_idx_);
  lua_pop(L_, 1);

  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 1);

  const char* verify = R"(
    assert(#emitted == 1, "Expected 1 emitted metric, got " .. #emitted)
    local r = emitted[1].log
    assert(r.ServiceName == "final-svc", "ServiceName mismatch: " .. tostring(r.ServiceName))
    assert(r.ResourceAttributes["service.name"] == "final-svc", "ResourceAttributes['service.name'] mismatch: " .. tostring(r.ResourceAttributes["service.name"]))
    assert(r.ResourceAttributes["region"] == "us-east-1", "ResourceAttributes['region'] mismatch")
    assert(r.ScopeName == "my-scope", "ScopeName mismatch: " .. tostring(r.ScopeName))
    assert(r.ScopeVersion == "1.0", "ScopeVersion mismatch: " .. tostring(r.ScopeVersion))
    assert(r.ScopeAttributes["env"] == "prod-scope", "ScopeAttributes['env'] mismatch")
    assert(r.Attributes["env"] == "prod", "Attributes['env'] mismatch: " .. tostring(r.Attributes["env"]))
    assert(r.Value == 7, "Value mismatch: " .. tostring(r.Value))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, NativeMetricsTable_MissingTimestampFallsBackToNow) {
  {
    const char* script = R"(
      return {
        name = "no_ts_gauge",
        interval_ms = 5000,
        gauge = { value = 1.25 }
      }
    )";
    ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
    int table_idx = lua_gettop(L_);
    auto res = ProcessNativeMetricsTable(L_, table_idx, emit_idx_);
    lua_pop(L_, 1);
    EXPECT_TRUE(res.success) << res.error;
    EXPECT_EQ(res.emitted_count, 1);
  }
  // Empty and sparse tables return (0, nil)
  {
    const char* script = "return {}";
    ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
    int table_idx = lua_gettop(L_);
    auto res = ProcessNativeMetricsTable(L_, table_idx, emit_idx_);
    lua_pop(L_, 1);
    EXPECT_TRUE(res.success);
    EXPECT_EQ(res.emitted_count, 0);
  }
  {
    const char* script = "return { metric = {} }";
    ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
    int table_idx = lua_gettop(L_);
    auto res = ProcessNativeMetricsTable(L_, table_idx, emit_idx_);
    lua_pop(L_, 1);
    EXPECT_TRUE(res.success);
    EXPECT_EQ(res.emitted_count, 0);
  }

  const char* verify = R"(
    assert(#emitted == 1, "Expected 1 emitted metric, got " .. #emitted)
    local r = emitted[1].log
    assert(r.TimeUnix == r.processed_at, "TimeUnix must fall back to processed_at when timestamp is absent")
    assert(r.StartTimeUnix == "", "StartTimeUnix must be empty when timestamp is absent, got " .. tostring(r.StartTimeUnix))
    assert(r.ServiceName == "unknown_service", "Default ServiceName must be unknown_service")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, NativeMetrics_UnsupportedNonOtlpVariantsReturnError) {
  const char* variants[] = {"distribution", "set", "sketch"};
  for (const char* v : variants) {
    // 1. Lua Table mode
    std::string lua_code = std::string("return { name = 'bad_m', ") + v + " = { values = {1} } }";
    ASSERT_EQ(luaL_dostring(L_, lua_code.c_str()), LUA_OK);
    int table_idx = lua_gettop(L_);
    auto tbl_res = ProcessNativeMetricsTable(L_, table_idx, emit_idx_);
    lua_pop(L_, 1);
    EXPECT_FALSE(tbl_res.success) << "Expected failure for table variant " << v;
    EXPECT_NE(tbl_res.error.find("Unsupported non-OTLP metric variant"), std::string::npos)
        << tbl_res.error;
    EXPECT_NE(tbl_res.error.find(v), std::string::npos) << tbl_res.error;
    EXPECT_EQ(tbl_res.emitted_count, 0);

    // 2. JSON String mode
    std::string json_code = std::string(R"({"name":"bad_m",")") + v + R"(":{"values":[1]}})";
    auto json_res = ProcessNativeMetrics(L_, json_code, emit_idx_);
    EXPECT_FALSE(json_res.success) << "Expected failure for JSON variant " << v;
    EXPECT_NE(json_res.error.find("Unsupported non-OTLP metric variant"), std::string::npos)
        << json_res.error;
    EXPECT_NE(json_res.error.find(v), std::string::npos) << json_res.error;
    EXPECT_EQ(json_res.emitted_count, 0);
  }

  // Also verify partial_count when 2nd item in a batch fails
  const char* batch_fail_script = R"(
    return {
      { name = "ok_counter", counter = { value = 10 } },
      { name = "bad_sketch", sketch = { agent_dd = {} } }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, batch_fail_script), LUA_OK);
  int batch_idx = lua_gettop(L_);
  auto batch_res = ProcessNativeMetricsTable(L_, batch_idx, emit_idx_);
  lua_pop(L_, 1);
  EXPECT_FALSE(batch_res.success);
  EXPECT_EQ(batch_res.emitted_count, 1);
}

TEST_F(NativeExtractorsTest, NativeMetricsJson_AllSupportedVariants) {
  std::string json_batch = R"([
    {
      "name": "req_count",
      "namespace": "api",
      "timestamp": "2026-10-22T10:10:00.000000000Z",
      "interval_ms": 5000,
      "kind": "incremental",
      "tags": {
        "resource.service.name": "json-metric-svc",
        "resource.env": "prod",
        "scope.name": "json.meter",
        "scope.version": "3.1",
        "scope.team": "core",
        "status": "200"
      },
      "counter": { "value": 123 }
    },
    {
      "name": "mem_bytes",
      "timestamp": "2026-10-22T10:10:01.000000000Z",
      "tags": {
        "resource.service.name": ["old-svc", "multi-svc"],
        "host": ["h1", "h2"]
      },
      "gauge": { "value": 456.75 }
    },
    {
      "name": "lat_hist",
      "timestamp": "2026-10-22T10:10:02.000000000Z",
      "aggregated_histogram": {
        "buckets": [
          { "upper_limit": 5.0, "count": 2 },
          { "upper_limit": 25.0, "count": 8 },
          { "upper_limit": "inf", "count": 1 }
        ],
        "count": 11,
        "sum": 123.5
      }
    },
    {
      "name": "lat_summary",
      "aggregated_summary": {
        "quantiles": [
          { "quantile": 0.5, "value": 4.2 },
          { "quantile": 0.99, "value": 21.0 }
        ],
        "count": 11,
        "sum": 123.5
      }
    }
  ])";
  auto res = ProcessNativeMetrics(L_, json_batch, emit_idx_);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 4);

  auto err_res =
      ProcessNativeMetrics(L_, R"({"metric":{"name":"bad","sketch":{"bins":[]}}})", emit_idx_);
  EXPECT_FALSE(err_res.success);
  EXPECT_NE(err_res.error.find("sketch"), std::string::npos);

  const char* verify = R"(
    assert(#emitted == 4, "Expected 4 emitted metrics, got " .. #emitted)
    local m1 = emitted[1].log
    assert(m1.MetricName == "api.req_count", "m1.MetricName mismatch: " .. tostring(m1.MetricName))
    assert(m1.MetricType == "sum", "m1.MetricType mismatch")
    assert(m1.AggregationTemporality == 1, "m1.AggregationTemporality mismatch: " .. tostring(m1.AggregationTemporality))
    assert(m1.ServiceName == "json-metric-svc", "m1.ServiceName mismatch")
    assert(m1.ResourceAttributes["service.name"] == "json-metric-svc")
    assert(m1.ResourceAttributes["env"] == "prod")
    assert(m1.ScopeName == "json.meter")
    assert(m1.ScopeVersion == "3.1")
    assert(m1.ScopeAttributes["team"] == "core")
    assert(m1.Attributes["status"] == "200")
    assert(m1.TimeUnix == "2026-10-22T10:10:00.000000000Z")
    assert(m1.StartTimeUnix == "2026-10-22T10:09:55.000000000Z", "m1.StartTimeUnix mismatch: " .. tostring(m1.StartTimeUnix))
    assert(m1.Value == 123)

    local m2 = emitted[2].log
    assert(m2.MetricName == "mem_bytes")
    assert(m2.MetricType == "gauge")
    assert(m2.ServiceName == "multi-svc", "m2.ServiceName mismatch: " .. tostring(m2.ServiceName))
    assert(m2.Attributes["host"] == "h2", "m2.Attributes['host'] mismatch: " .. tostring(m2.Attributes["host"]))
    assert(math.abs(m2.Value - 456.75) < 1e-6)

    local m3 = emitted[3].log
    assert(m3.MetricName == "lat_hist")
    assert(m3.MetricType == "histogram")
    assert(m3.AggregationTemporality == 0)
    assert(m3.Value == nil)
    assert(m3.Histogram.Count == 11)
    assert(m3.Histogram.AggregationTemporality == 0)
    assert(math.abs(m3.Histogram.Sum - 123.5) < 1e-6)
    assert(#m3.Histogram.ExplicitBounds == 2 and m3.Histogram.ExplicitBounds[1] == 5.0 and m3.Histogram.ExplicitBounds[2] == 25.0)
    assert(#m3.Histogram.BucketCounts == 3 and m3.Histogram.BucketCounts[3] == 1)

    local m4 = emitted[4].log
    assert(m4.MetricName == "lat_summary")
    assert(m4.MetricType == "summary")
    assert(m4.Value == nil)
    assert(m4.Histogram == nil)
    assert(type(m4.Summary) == "table")
    assert(m4.Summary.Count == 11 and m4.Summary.count == 11)
    assert(math.abs(m4.Summary.Sum - 123.5) < 1e-6)
    assert(#m4.Summary.QuantileValues == 2)
    assert(math.abs(m4.Summary.QuantileValues[1].Quantile - 0.5) < 1e-6 and math.abs(m4.Summary.QuantileValues[1].Value - 4.2) < 1e-6)
    assert(math.abs(m4.Summary.QuantileValues[2].Quantile - 0.99) < 1e-6 and math.abs(m4.Summary.QuantileValues[2].Value - 21.0) < 1e-6)
    assert(m4.TimeUnix == m4.processed_at)
    assert(m4.StartTimeUnix == "")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, JsonWrapperSourceTypeBeforeInnerObject) {
  auto log_res = ProcessNativeLogs(
      L_, R"({"source_type":"opentelemetry","log":{"message":"wrapped-after-source-type"}})",
      emit_idx_);
  EXPECT_TRUE(log_res.success) << log_res.error;
  EXPECT_EQ(log_res.emitted_count, 1);

  auto trace_res = ProcessNativeTraces(
      L_, R"({"source_type":"opentelemetry","log":{"name":"span-after-source-type"}})", emit_idx_);
  EXPECT_TRUE(trace_res.success) << trace_res.error;
  EXPECT_EQ(trace_res.emitted_count, 1);

  auto metric_res = ProcessNativeMetrics(
      L_,
      R"({"source_type":"opentelemetry","metric":{"name":"metric_after_src","gauge":{"value":9.5}}})",
      emit_idx_);
  EXPECT_TRUE(metric_res.success) << metric_res.error;
  EXPECT_EQ(metric_res.emitted_count, 1);

  const char* verify = R"(
    assert(#emitted == 3, "Expected 3 emitted events, got " .. #emitted)
    assert(emitted[1].log.Body == "wrapped-after-source-type", "Log Body mismatch: " .. tostring(emitted[1].log.Body))
    assert(emitted[2].log.SpanName == "span-after-source-type", "Trace SpanName mismatch: " .. tostring(emitted[2].log.SpanName))
    assert(emitted[3].log.MetricName == "metric_after_src", "MetricName mismatch: " .. tostring(emitted[3].log.MetricName))
    assert(math.abs(emitted[3].log.Value - 9.5) < 1e-6, "Metric Value mismatch: " .. tostring(emitted[3].log.Value))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, Resilience_EmptyJsonMalformedBatchAndDeepNesting) {
  // 1. Empty JSON input returns (false, "Empty JSON input", 0) across all three extractors
  auto empty_log = ProcessNativeLogs(L_, "", emit_idx_);
  EXPECT_FALSE(empty_log.success);
  EXPECT_EQ(empty_log.emitted_count, 0);

  auto empty_trace = ProcessNativeTraces(L_, "", emit_idx_);
  EXPECT_FALSE(empty_trace.success);
  EXPECT_EQ(empty_trace.emitted_count, 0);

  auto empty_metric = ProcessNativeMetrics(L_, "", emit_idx_);
  EXPECT_FALSE(empty_metric.success);
  EXPECT_EQ(empty_metric.emitted_count, 0);

  // 2. Non-object element mid-batch fails fast and preserves partial_count = 1
  auto bad_log_batch = ProcessNativeLogs(L_, R"([{"message":"ok"}, 123])", emit_idx_);
  EXPECT_FALSE(bad_log_batch.success);
  EXPECT_EQ(bad_log_batch.emitted_count, 1);

  auto bad_trace_batch = ProcessNativeTraces(L_, R"([{"name":"ok-span"}, 123])", emit_idx_);
  EXPECT_FALSE(bad_trace_batch.success);
  EXPECT_EQ(bad_trace_batch.emitted_count, 1);

  auto bad_metric_batch =
      ProcessNativeMetrics(L_, R"([{"name":"ok_m","gauge":{"value":1}}, 123])", emit_idx_);
  EXPECT_FALSE(bad_metric_batch.success);
  EXPECT_EQ(bad_metric_batch.emitted_count, 1);

  // 3. Deeply nested JSON object (> 16 levels) in span event attributes clamps safely
  std::string deep_json = R"({"name":"deep-span","events":[{"name":"ev","attributes":{"root":)";
  for (int i = 0; i < 20; ++i) {
    deep_json += R"({"n":)";
  }
  deep_json += R"("leaf")";
  for (int i = 0; i < 20; ++i) {
    deep_json += "}";
  }
  deep_json += "}}]}";

  auto deep_res = ProcessNativeTraces(L_, deep_json, emit_idx_);
  EXPECT_TRUE(deep_res.success) << deep_res.error;
  EXPECT_EQ(deep_res.emitted_count, 1);

  const char* verify = R"(
    assert(#emitted == 4, "Expected 4 emitted events total, got " .. #emitted)
    local deep_span = emitted[4].log
    assert(type(deep_span.Events) == "string", "Events must be JSON string")
    assert(string.find(deep_span.Events, "%[max_depth_exceeded%]") ~= nil,
           "Expected [max_depth_exceeded] in Events JSON, got: " .. tostring(deep_span.Events))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, Resilience_NonWrapperSubObjectAndDeepJsonArrays) {
  // 1. Flat log table containing a non-wrapper log = { file = "auth.rs", line = 42 } sub-object
  {
    const char* script = R"(
      return {
        message = "user login",
        log = { file = "auth.rs", line = 42 },
        resources = { ["service.name"] = "auth-svc" }
      }
    )";
    ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
    int table_idx = lua_gettop(L_);
    auto res = ProcessNativeLogsTable(L_, table_idx, emit_idx_);
    lua_pop(L_, 1);
    EXPECT_TRUE(res.success) << res.error;
    EXPECT_EQ(res.emitted_count, 1);
  }

  // 2. Flat log JSON with non-wrapper "log" sub-object before "message"
  {
    auto res = ProcessNativeLogs(
        L_, R"({"log":{"file":"auth.rs","line":42},"message":"json login"})", emit_idx_);
    EXPECT_TRUE(res.success) << res.error;
    EXPECT_EQ(res.emitted_count, 1);
  }

  // 3. Deeply nested JSON array (> 16 levels) in span event attributes clamps safely
  {
    std::string deep_arr_json =
        R"({"name":"deep-arr-span","events":[{"name":"ev","attributes":{"arr":)";
    for (int i = 0; i < 20; ++i) {
      deep_arr_json += "[";
    }
    deep_arr_json += R"({"stringValue":"unwrapped"})";
    for (int i = 0; i < 20; ++i) {
      deep_arr_json += "]";
    }
    deep_arr_json += "}}]}";

    auto res = ProcessNativeTraces(L_, deep_arr_json, emit_idx_);
    EXPECT_TRUE(res.success) << res.error;
    EXPECT_EQ(res.emitted_count, 1);
  }

  // 4. Invalid string-encoded bucket count in Lua histogram defaults cleanly to 0
  {
    const char* script = R"(
      return {
        name = "hist_bad_bc",
        histogram = {
          count = 5,
          sum = 10.0,
          explicit_bounds = { 1.0 },
          counts = { "5", "not_a_number" }
        }
      }
    )";
    ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
    int table_idx = lua_gettop(L_);
    auto res = ProcessNativeMetricsTable(L_, table_idx, emit_idx_);
    lua_pop(L_, 1);
    EXPECT_TRUE(res.success) << res.error;
    EXPECT_EQ(res.emitted_count, 1);
  }

  const char* verify = R"(
    assert(#emitted == 4, "Expected 4 emitted events, got " .. #emitted)
    assert(emitted[1].log.Body == "user login", "Event 1 Body mismatch")
    assert(emitted[1].log.ServiceName == "auth-svc", "Event 1 ServiceName mismatch")
    assert(emitted[2].log.Body == "json login", "Event 2 Body mismatch")
    assert(string.find(emitted[3].log.Events, "%[max_depth_exceeded%]") ~= nil,
           "Expected [max_depth_exceeded] in nested array Events JSON: " .. tostring(emitted[3].log.Events))
    assert(emitted[4].log.Histogram.BucketCounts[1] == 5)
    assert(emitted[4].log.Histogram.BucketCounts[2] == 0)
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, JsonWrapperRecognizedOuterFieldBeforeInnerAndNonZeroTzOffset) {
  auto log_res = ProcessNativeLogs(
      L_,
      R"({"timestamp":"2026-10-16T00:00:00Z","log":{"message":"inner-log-body","timestamp":"2026-10-16T14:00:00.123456789+02:00"}})",
      emit_idx_);
  EXPECT_TRUE(log_res.success) << log_res.error;
  EXPECT_EQ(log_res.emitted_count, 1);

  auto trace_res = ProcessNativeTraces(
      L_,
      R"({"timestamp":"2026-10-16T00:00:00Z","trace":{"name":"inner-span","start_time_unix_nano":"2026-10-16T07:30:00.5-0430"}})",
      emit_idx_);
  EXPECT_TRUE(trace_res.success) << trace_res.error;
  EXPECT_EQ(trace_res.emitted_count, 1);

  auto metric_res = ProcessNativeMetrics(
      L_,
      R"({"timestamp":"2026-10-16T00:00:00Z","metric":{"namespace":"http","name":"requests","timestamp":"2026-10-16T14:00:00+02:00","gauge":{"value":42.0}}})",
      emit_idx_);
  EXPECT_TRUE(metric_res.success) << metric_res.error;
  EXPECT_EQ(metric_res.emitted_count, 1);

  const char* verify = R"(
    assert(#emitted == 3, "Expected 3 emitted events, got " .. #emitted)
    assert(emitted[1].log.Body == "inner-log-body", "Log Body mismatch: " .. tostring(emitted[1].log.Body))
    assert(emitted[1].log.Timestamp == "2026-10-16T12:00:00.123456789Z", "Log Timestamp mismatch: " .. tostring(emitted[1].log.Timestamp))
    assert(emitted[2].log.SpanName == "inner-span", "Trace SpanName mismatch: " .. tostring(emitted[2].log.SpanName))
    assert(emitted[2].log.Timestamp == "2026-10-16T12:00:00.500000000Z", "Trace Timestamp mismatch: " .. tostring(emitted[2].log.Timestamp))
    assert(emitted[3].log.MetricName == "http.requests", "MetricName mismatch: " .. tostring(emitted[3].log.MetricName))
    assert(emitted[3].log.TimeUnix == "2026-10-16T12:00:00.000000000Z", "Metric TimeUnix mismatch: " .. tostring(emitted[3].log.TimeUnix))
    assert(math.abs(emitted[3].log.Value - 42.0) < 1e-6, "Metric Value mismatch: " .. tostring(emitted[3].log.Value))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, NativeLogsNumericSeverityCodes) {
  // Test numeric severity codes 1 to 24 and string severity name tokens
  std::string json = R"([
    {"message":"m1","severity_number":1,"severity_text":"TRACE"},
    {"message":"m2","severity_number":"SEVERITY_NUMBER_DEBUG3"},
    {"message":"m3","severity_number":9,"severity_text":"INFO"},
    {"message":"m4","severity_number":13},
    {"message":"m5","severity_number":17,"severity_text":"ERROR"},
    {"message":"m6","severity_number":"SEVERITY_NUMBER_FATAL4"}
  ])";
  auto res = ProcessNativeLogs(L_, json, emit_idx_);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 6);

  const char* verify = R"(
    assert(#emitted == 6, "Expected 6 emitted events, got " .. #emitted)
    assert(emitted[1].log.SeverityNumber == 1, "m1 SeverityNumber mismatch")
    assert(emitted[1].log.SeverityText == "TRACE", "m1 SeverityText mismatch")
    assert(emitted[2].log.SeverityNumber == 7, "m2 SeverityNumber mismatch")
    assert(emitted[3].log.SeverityNumber == 9, "m3 SeverityNumber mismatch")
    assert(emitted[3].log.SeverityText == "INFO", "m3 SeverityText mismatch")
    assert(emitted[4].log.SeverityNumber == 13, "m4 SeverityNumber mismatch")
    assert(emitted[5].log.SeverityNumber == 17, "m5 SeverityNumber mismatch")
    assert(emitted[5].log.SeverityText == "ERROR", "m5 SeverityText mismatch")
    assert(emitted[6].log.SeverityNumber == 24, "m6 SeverityNumber mismatch")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, NativeLogsArrayWrapperJsonAndTable) {
  // 1. JSON array wrapper: {"log":
  // [{"message":"m1","severity_number":9},{"message":"m2","severity_number":13}]}
  std::string json_wrapped =
      R"({"log":[{"message":"m1","severity_number":9},{"message":"m2","severity_number":13}]})";
  auto res1 = ProcessNativeLogs(L_, json_wrapped, emit_idx_);
  EXPECT_TRUE(res1.success) << res1.error;
  EXPECT_EQ(res1.emitted_count, 2);

  // 2. Table array wrapper: { log = { { message = "m3" }, { message = "m4" } } }
  const char* script = R"(
    return {
      log = {
        { message = "m3", severity_number = 5 },
        { message = "m4", severity_number = 17 }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK) << lua_tostring(L_, -1);
  int table_idx = lua_gettop(L_);
  auto res2 = ProcessNativeLogsTable(L_, table_idx, emit_idx_);
  lua_pop(L_, 1);
  EXPECT_TRUE(res2.success) << res2.error;
  EXPECT_EQ(res2.emitted_count, 2);

  const char* verify = R"(
    assert(#emitted == 4, "Expected 4 emitted events, got " .. #emitted)
    assert(emitted[1].log.Body == "m1", "m1 Body mismatch")
    assert(emitted[2].log.Body == "m2", "m2 Body mismatch")
    assert(emitted[3].log.Body == "m3", "m3 Body mismatch")
    assert(emitted[4].log.Body == "m4", "m4 Body mismatch")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, NativeLogsTimestampFallbackToProcessedAt) {
  // When no timestamps are provided, fall back to processed_at (ISO-8601 UTC nano)
  const char* script = R"(
    return {
      message = "pure fallback record"
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK) << lua_tostring(L_, -1);
  int table_idx = lua_gettop(L_);
  auto res = ProcessNativeLogsTable(L_, table_idx, emit_idx_);
  lua_pop(L_, 1);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 1);

  std::string json = R"({"message":"json fallback"})";
  auto res2 = ProcessNativeLogs(L_, json, emit_idx_);
  EXPECT_TRUE(res2.success) << res2.error;
  EXPECT_EQ(res2.emitted_count, 1);

  const char* verify = R"(
    assert(#emitted == 2, "Expected 2 emitted events, got " .. #emitted)
    local r1 = emitted[1].log
    assert(r1.Timestamp ~= nil and #r1.Timestamp == 30, "r1.Timestamp missing or invalid: " .. tostring(r1.Timestamp))
    assert(r1.Timestamp == r1.processed_at, "r1.Timestamp must match processed_at")

    local r2 = emitted[2].log
    assert(r2.Timestamp ~= nil and #r2.Timestamp == 30, "r2.Timestamp missing or invalid: " .. tostring(r2.Timestamp))
    assert(r2.Timestamp == r2.processed_at, "r2.Timestamp must match processed_at")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, NativeLogsBodyFormatsAndScopeJson) {
  // Test body stringValue in table mode and scope_schema_url in JSON mode
  const char* script = R"(
    return {
      body = { stringValue = "nested string value" },
      scope = { schema_url = "https://schemas.example.com/scope" }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK) << lua_tostring(L_, -1);
  int table_idx = lua_gettop(L_);
  auto res = ProcessNativeLogsTable(L_, table_idx, emit_idx_);
  lua_pop(L_, 1);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 1);

  std::string json = R"({
    "message": "scoped log",
    "scope": {
      "name": "my-logger",
      "version": "2.0.0",
      "schema_url": "https://opentelemetry.io/schemas/scope-v2"
    },
    "resource_schema_url": "https://opentelemetry.io/schemas/resource-v2",
    "trace_flags": 1,
    "event_name": "custom.log.event"
  })";
  auto res2 = ProcessNativeLogs(L_, json, emit_idx_);
  EXPECT_TRUE(res2.success) << res2.error;
  EXPECT_EQ(res2.emitted_count, 1);

  std::string json_bodies = R"([
    {"body": {"stringValue": "json stringValue"}},
    {"body": null},
    {"body": {"complex": true}}
  ])";
  auto res3 = ProcessNativeLogs(L_, json_bodies, emit_idx_);
  EXPECT_TRUE(res3.success) << res3.error;
  EXPECT_EQ(res3.emitted_count, 3);

  const char* verify = R"(
    assert(#emitted == 5, "Expected 5 emitted events, got " .. #emitted)
    assert(emitted[1].log.Body == "nested string value", "Table stringValue Body mismatch: " .. tostring(emitted[1].log.Body))
    assert(emitted[1].log.ScopeSchemaUrl == "https://schemas.example.com/scope", "ScopeSchemaUrl mismatch")

    local r2 = emitted[2].log
    assert(r2.ScopeName == "my-logger", "ScopeName mismatch")
    assert(r2.ScopeVersion == "2.0.0", "ScopeVersion mismatch")
    assert(r2.ScopeSchemaUrl == "https://opentelemetry.io/schemas/scope-v2", "ScopeSchemaUrl mismatch")
    assert(r2.ResourceSchemaUrl == "https://opentelemetry.io/schemas/resource-v2", "ResourceSchemaUrl mismatch")
    assert(r2.TraceFlags == 1, "TraceFlags mismatch")
    assert(r2.EventName == "custom.log.event", "EventName mismatch")

    assert(emitted[3].log.Body == "json stringValue", "r3 Body mismatch: " .. tostring(emitted[3].log.Body))
    assert(emitted[4].log.Body == "", "r4 Body mismatch: " .. tostring(emitted[4].log.Body))
    assert(emitted[5].log.Body == '{"complex":true}', "r5 Body mismatch: " .. tostring(emitted[5].log.Body))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, TracesStatusCodesAndMessages) {
  // Test numeric and string status codes, status message fields, and span kinds
  std::string json = R"([
    {
      "trace_id": "4bf92f3577b34da6a3ce929d0e0e4736",
      "span_id": "00f067aa0ba902b7",
      "name": "span-ok",
      "kind": 1,
      "status": {"code": 1, "message": "everything ok"}
    },
    {
      "trace_id": "4bf92f3577b34da6a3ce929d0e0e4736",
      "span_id": "00f067aa0ba902b8",
      "name": "span-err",
      "kind": "PRODUCER",
      "status_code": "STATUS_CODE_ERROR",
      "status_message": "failed to connect"
    },
    {
      "trace_id": "4bf92f3577b34da6a3ce929d0e0e4736",
      "span_id": "00f067aa0ba902b9",
      "name": "span-client",
      "kind": "client",
      "status_code": 0
    },
    {
      "trace_id": "4bf92f3577b34da6a3ce929d0e0e4736",
      "span_id": "00f067aa0ba902ba",
      "name": "span-consumer",
      "kind": "consumer"
    }
  ])";
  auto res = ProcessNativeTraces(L_, json, emit_idx_);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 4);

  // Also test table mode with status table and kind strings/numbers
  const char* script = R"(
    return {
      {
        trace_id = "4bf92f3577b34da6a3ce929d0e0e4736",
        span_id = "00f067aa0ba902bb",
        name = "tbl-span-ok",
        kind = "SPAN_KIND_INTERNAL",
        status = { code = "STATUS_CODE_OK", message = "table ok" }
      },
      {
        trace_id = "4bf92f3577b34da6a3ce929d0e0e4736",
        span_id = "00f067aa0ba902bc",
        name = "tbl-span-err",
        kind = 4,
        status = { code = 2, message = "table err" }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK) << lua_tostring(L_, -1);
  int table_idx = lua_gettop(L_);
  auto res2 = ProcessNativeTracesTable(L_, table_idx, emit_idx_);
  lua_pop(L_, 1);
  EXPECT_TRUE(res2.success) << res2.error;
  EXPECT_EQ(res2.emitted_count, 2);

  const char* verify = R"(
    assert(#emitted == 6, "Expected 6 emitted events, got " .. #emitted)
    assert(emitted[1].log.StatusCode == "STATUS_CODE_OK", "s1 StatusCode mismatch")
    assert(emitted[1].log.StatusMessage == "everything ok", "s1 StatusMessage mismatch")
    assert(emitted[1].log.SpanKind == "SPAN_KIND_INTERNAL", "s1 SpanKind mismatch")

    assert(emitted[2].log.StatusCode == "STATUS_CODE_ERROR", "s2 StatusCode mismatch")
    assert(emitted[2].log.StatusMessage == "failed to connect", "s2 StatusMessage mismatch")
    assert(emitted[2].log.SpanKind == "SPAN_KIND_PRODUCER", "s2 SpanKind mismatch")

    assert(emitted[3].log.StatusCode == "STATUS_CODE_UNSET", "s3 StatusCode mismatch")
    assert(emitted[3].log.SpanKind == "SPAN_KIND_CLIENT", "s3 SpanKind mismatch")

    assert(emitted[4].log.SpanKind == "SPAN_KIND_CONSUMER", "s4 SpanKind mismatch")

    assert(emitted[5].log.StatusCode == "STATUS_CODE_OK", "s5 StatusCode mismatch")
    assert(emitted[5].log.StatusMessage == "table ok", "s5 StatusMessage mismatch")
    assert(emitted[5].log.SpanKind == "SPAN_KIND_INTERNAL", "s5 SpanKind mismatch")

    assert(emitted[6].log.StatusCode == "STATUS_CODE_ERROR", "s6 StatusCode mismatch")
    assert(emitted[6].log.StatusMessage == "table err", "s6 StatusMessage mismatch")
    assert(emitted[6].log.SpanKind == "SPAN_KIND_PRODUCER", "s6 SpanKind mismatch")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, TracesSpanEventsAndLinksAttributes) {
  // Test spans containing events and links with various attribute types
  std::string json = R"({
    "trace_id": "4bf92f3577b34da6a3ce929d0e0e4736",
    "span_id": "00f067aa0ba902b7",
    "name": "span-events-links",
    "events": [
      {
        "name": "cache_lookup",
        "time_unix_nano": 1729598400000000000,
        "attributes": {
          "hit": {"boolValue": true},
          "keys": {"intValue": 42},
          "score": {"doubleValue": 99.5},
          "msg": {"stringValue": "found"}
        }
      }
    ],
    "links": [
      {
        "trace_id": "5b8efff798038103d269b633813fc60c",
        "span_id": "eee19b7ec3c1b174",
        "attributes": {
          "relation": "parent_context"
        }
      }
    ]
  })";
  auto res = ProcessNativeTraces(L_, json, emit_idx_);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 1);

  const char* verify = R"(
    assert(#emitted == 1, "Expected 1 emitted event")
    local r = emitted[1].log
    assert(type(r.Events) == "string", "Events must be a string")
    assert(r.Events:find("cache_lookup") ~= nil, "Events must contain cache_lookup")
    assert(r.Events:find('"hit":true') ~= nil, "Events must contain hit:true")
    assert(r.Events:find('"keys":42') ~= nil, "Events must contain keys:42")
    assert(r.Events:find('"msg":"found"') ~= nil, "Events must contain msg:found")

    assert(type(r.Links) == "string", "Links must be a string")
    assert(r.Links:find("5b8efff798038103d269b633813fc60c") ~= nil, "Links must contain trace_id")
    assert(r.Links:find("eee19b7ec3c1b174") ~= nil, "Links must contain span_id")
    assert(r.Links:find('"relation":"parent_context"') ~= nil, "Links must contain relation:parent_context")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);

  // Also test table mode events and links serialization
  const char* script = R"(
    return {
      trace_id = "4bf92f3577b34da6a3ce929d0e0e4736",
      span_id = "00f067aa0ba902b7",
      name = "table-events-links",
      resources = {
        attributes = {
          ["service.name"] = "nested-svc"
        }
      },
      events = {
        {
          name = "tbl_event",
          timestamp = "2026-10-22T10:00:00.000000000Z",
          attributes = {
            num_k = 100,
            str_k = "v",
            [123] = "numeric_key_val"
          }
        }
      },
      links = {
        {
          trace_id = "5b8efff798038103d269b633813fc60c",
          span_id = "eee19b7ec3c1b174",
          trace_state = "congo=t61rcWkgMzE",
          attributes = {
            link_k = "link_v"
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK) << lua_tostring(L_, -1);
  int table_idx = lua_gettop(L_);
  auto res2 = ProcessNativeTracesTable(L_, table_idx, emit_idx_);
  lua_pop(L_, 1);
  EXPECT_TRUE(res2.success) << res2.error;
  EXPECT_EQ(res2.emitted_count, 1);

  const char* verify_tbl = R"(
    assert(#emitted == 2, "Expected 2 emitted events, got " .. #emitted)
    local r2 = emitted[2].log
    assert(r2.ServiceName == "nested-svc", "Nested service name mismatch: " .. tostring(r2.ServiceName))
    assert(type(r2.Events) == "string", "r2.Events must be string")
    assert(r2.Events:find("tbl_event") ~= nil, "r2.Events missing tbl_event")
    assert(r2.Events:find("numeric_key_val") ~= nil, "r2.Events missing numeric key value")
    assert(type(r2.Links) == "string", "r2.Links must be string")
    assert(r2.Links:find("congo=t61rcWkgMzE") ~= nil, "r2.Links missing trace state")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_tbl), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, TracesArrayWrapperBatch) {
  // Test JSON array wrapper {"trace": [ ... ]}
  std::string json_wrapped = R"({
    "trace": [
      {
        "trace_id": "4bf92f3577b34da6a3ce929d0e0e4736",
        "span_id": "00f067aa0ba902b1",
        "name": "wrapped-span-1"
      },
      {
        "trace_id": "4bf92f3577b34da6a3ce929d0e0e4736",
        "span_id": "00f067aa0ba902b2",
        "name": "wrapped-span-2"
      }
    ]
  })";
  auto res = ProcessNativeTraces(L_, json_wrapped, emit_idx_);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 2);

  const char* verify = R"(
    assert(#emitted == 2, "Expected 2 emitted events")
    assert(emitted[1].log.SpanName == "wrapped-span-1", "s1 SpanName mismatch")
    assert(emitted[2].log.SpanName == "wrapped-span-2", "s2 SpanName mismatch")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, MetricsSummaryQuantileObjectsAndFlatArrays) {
  // 1. Table mode with object quantiles
  const char* script1 = R"(
    return {
      metric = {
        name = "http.latency.summary",
        kind = "aggregated_summary",
        timestamp = "2026-10-22T10:00:00.000000000Z",
        aggregated_summary = {
          count = 100,
          sum = 5000.0,
          quantiles = {
            { quantile = 0.5, value = 40.0 },
            { quantile = 0.99, value = 120.0 }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, script1), LUA_OK) << lua_tostring(L_, -1);
  int table_idx1 = lua_gettop(L_);
  auto res1 = ProcessNativeMetricsTable(L_, table_idx1, emit_idx_);
  lua_pop(L_, 1);
  EXPECT_TRUE(res1.success) << res1.error;
  EXPECT_EQ(res1.emitted_count, 1);

  // 2. Table mode with flat quantiles
  const char* script2 = R"(
    return {
      metric = {
        name = "flat.quantiles.summary",
        kind = "aggregated_summary",
        aggregated_summary = {
          count = 10,
          sum = 100.0,
          quantiles = { 0.5, 0.9 },
          values = { 5.0, 95.0 }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, script2), LUA_OK) << lua_tostring(L_, -1);
  int table_idx2 = lua_gettop(L_);
  auto res2 = ProcessNativeMetricsTable(L_, table_idx2, emit_idx_);
  lua_pop(L_, 1);
  EXPECT_TRUE(res2.success) << res2.error;
  EXPECT_EQ(res2.emitted_count, 1);

  // 3. JSON mode with object quantiles
  std::string json = R"({
    "metric": {
      "name": "json.summary",
      "kind": "aggregated_summary",
      "aggregated_summary": {
        "count": 50,
        "sum": 250.0,
        "quantiles": [
          {"quantile": 0.5, "value": 5.0},
          {"quantile": 0.95, "value": 15.0}
        ]
      }
    }
  })";
  auto res3 = ProcessNativeMetrics(L_, json, emit_idx_);
  EXPECT_TRUE(res3.success) << res3.error;
  EXPECT_EQ(res3.emitted_count, 1);

  const char* verify = R"(
    assert(#emitted == 3, "Expected 3 emitted events, got " .. #emitted)
    local r1 = emitted[1].log
    assert(r1.Summary ~= nil, "r1.Summary table missing")
    assert(r1.Summary.Count == 100, "r1.Summary.Count mismatch: " .. tostring(r1.Summary.Count))
    assert(r1.Summary.Sum == 5000.0, "r1.Summary.Sum mismatch")
    assert(#r1.Summary.QuantileValues == 2, "r1.Summary.QuantileValues count mismatch: " .. #r1.Summary.QuantileValues)
    assert(math.abs(r1.Summary.QuantileValues[1].Quantile - 0.5) < 1e-6, "Quantile[1] mismatch")
    assert(math.abs(r1.Summary.QuantileValues[1].Value - 40.0) < 1e-6, "Value[1] mismatch")
    assert(math.abs(r1.Summary.QuantileValues[2].Quantile - 0.99) < 1e-6, "Quantile[2] mismatch")
    assert(math.abs(r1.Summary.QuantileValues[2].Value - 120.0) < 1e-6, "Value[2] mismatch")

    local r2 = emitted[2].log
    assert(r2.Summary.Count == 10, "r2.Summary.Count mismatch")
    assert(#r2.Summary.QuantileValues == 2, "r2.Summary.QuantileValues count mismatch")
    assert(math.abs(r2.Summary.QuantileValues[2].Value - 95.0) < 1e-6, "r2 Value[2] mismatch")

    local r3 = emitted[3].log
    assert(r3.Summary.Count == 50, "r3.Summary.Count mismatch")
    assert(#r3.Summary.QuantileValues == 2, "r3.Summary.QuantileValues count mismatch")
    assert(math.abs(r3.Summary.QuantileValues[2].Value - 15.0) < 1e-6, "r3 Value[2] mismatch")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, MetricsJsonScopeAndResourceAttributes) {
  // Test JSON with resource, scope, and typed tags (int, double, bool, object)
  std::string json = R"({
    "metric": {
      "name": "system.cpu.load",
      "kind": "gauge",
      "gauge": {"value": 1.75},
      "resources": {
        "service.name": "custom-srv",
        "host.name": "host-42"
      },
      "scope": {
        "name": "system.metrics",
        "version": "1.0.1",
        "attributes": {
          "scope_lib": "cpu_watcher"
        }
      },
      "tags": {
        "cpu_core": 4,
        "load_ratio": 0.875,
        "is_idle": false,
        "extra_info": {"model": "x86"}
      }
    }
  })";
  auto res = ProcessNativeMetrics(L_, json, emit_idx_);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 1);

  const char* verify = R"(
    assert(#emitted == 1, "Expected 1 emitted event")
    local r = emitted[1].log
    assert(r.MetricName == "system.cpu.load", "MetricName mismatch")
    assert(r.ServiceName == "custom-srv", "ServiceName mismatch")
    assert(r.ScopeName == "system.metrics", "ScopeName mismatch")
    assert(r.ScopeVersion == "1.0.1", "ScopeVersion mismatch")
    assert(r.ResourceAttributes["service.name"] == "custom-srv", "ResourceAttributes service.name mismatch")
    assert(r.ResourceAttributes["host.name"] == "host-42", "ResourceAttributes host.name mismatch")
    assert(r.Attributes.cpu_core == 4, "Attributes.cpu_core mismatch")
    assert(math.abs(r.Attributes.load_ratio - 0.875) < 1e-6, "Attributes.load_ratio mismatch")
    assert(r.Attributes.is_idle == false, "Attributes.is_idle mismatch")
    assert(r.Attributes.extra_info:find('"model"') ~= nil and r.Attributes.extra_info:find('"x86"') ~= nil, "Attributes.extra_info mismatch: " .. tostring(r.Attributes.extra_info))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, MetricsSpecialFloatValuesAndHistogramBounds) {
  // 1. Test string numbers in table mode
  const char* script = R"(
    return {
      {
        name = "int.string.gauge",
        kind = "gauge",
        gauge = { value = "42" }
      },
      {
        name = "double.string.gauge",
        kind = "gauge",
        gauge = { value = "3.14159" }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK) << lua_tostring(L_, -1);
  int table_idx = lua_gettop(L_);
  auto res1 = ProcessNativeMetricsTable(L_, table_idx, emit_idx_);
  lua_pop(L_, 1);
  EXPECT_TRUE(res1.success) << res1.error;
  EXPECT_EQ(res1.emitted_count, 2);

  // 2. Test JSON histogram with string explicit bounds: NaN, Infinity, -Infinity (filtered out by
  // TryParseJsonFiniteBound)
  std::string json_hist = R"({
    "name": "hist.with.special.bounds",
    "kind": "aggregated_histogram",
    "aggregated_histogram": {
      "count": 10,
      "sum": 55.0,
      "bucket_counts": [2, 3, 5],
      "explicit_bounds": [1.0, "Infinity", 5.0, "NaN", "+Infinity", 10.0]
    }
  })";
  auto res2 = ProcessNativeMetrics(L_, json_hist, emit_idx_);
  EXPECT_TRUE(res2.success) << res2.error;
  EXPECT_EQ(res2.emitted_count, 1);

  const char* verify = R"(
    assert(#emitted == 3, "Expected 3 emitted events, got " .. #emitted)
    assert(emitted[1].log.Value == 42, "emitted[1].log.Value mismatch")
    assert(math.abs(emitted[2].log.Value - 3.14159) < 1e-5, "emitted[2].log.Value mismatch")

    local h = emitted[3].log.Histogram
    assert(h ~= nil, "Histogram table missing")
    assert(h.Count == 10, "Histogram Count mismatch")
    -- The finite bounds must only include 1.0, 5.0, 10.0 (Infinity, NaN, +Infinity filtered)
    assert(#h.ExplicitBounds == 3, "ExplicitBounds count mismatch: " .. #h.ExplicitBounds)
    assert(math.abs(h.ExplicitBounds[1] - 1.0) < 1e-6)
    assert(math.abs(h.ExplicitBounds[2] - 5.0) < 1e-6)
    assert(math.abs(h.ExplicitBounds[3] - 10.0) < 1e-6)
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);

  // 3. Table mode with resource, scope, and float counter
  const char* script_scope = R"(
    return {
      name = "table.scoped.counter",
      kind = "counter",
      counter = { value = 15.5 },
      resource = {
        attributes = {
          ["service.name"] = "table-metric-svc",
          cluster = "eu-west-1"
        }
      },
      scope = {
        name = "meter.lib",
        version = "3.2.1",
        attributes = {
          scope_tag = "prod"
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, script_scope), LUA_OK) << lua_tostring(L_, -1);
  int scope_tbl_idx = lua_gettop(L_);
  auto res3 = ProcessNativeMetricsTable(L_, scope_tbl_idx, emit_idx_);
  lua_pop(L_, 1);
  EXPECT_TRUE(res3.success) << res3.error;
  EXPECT_EQ(res3.emitted_count, 1);

  // 4. JSON mode array wrapper with top-level attributes
  std::string json_wrapped_metrics = R"({
    "metric": [
      {
        "name": "arr.gauge.1",
        "kind": "gauge",
        "gauge": {"value": 10},
        "attributes": {"attr_k": "attr_v"}
      },
      {
        "name": "arr.gauge.2",
        "kind": "gauge",
        "gauge": {"value": 20}
      }
    ]
  })";
  auto res4 = ProcessNativeMetrics(L_, json_wrapped_metrics, emit_idx_);
  EXPECT_TRUE(res4.success) << res4.error;
  EXPECT_EQ(res4.emitted_count, 2);

  // 5. JSON mode histogram with string and uint bucket counts
  std::string json_hist_types = R"({
    "name": "hist.types",
    "kind": "aggregated_histogram",
    "aggregated_histogram": {
      "count": 3,
      "sum": 12.0,
      "bucket_counts": [1, "2"],
      "explicit_bounds": ["2.5", 5.0]
    }
  })";
  auto res5 = ProcessNativeMetrics(L_, json_hist_types, emit_idx_);
  EXPECT_TRUE(res5.success) << res5.error;
  EXPECT_EQ(res5.emitted_count, 1);

  const char* verify_more = R"(
    assert(#emitted == 7, "Expected 7 emitted events, got " .. #emitted)
    local r4 = emitted[4].log
    assert(r4.MetricName == "table.scoped.counter", "r4 MetricName mismatch")
    assert(r4.ServiceName == "table-metric-svc", "r4 ServiceName mismatch")
    assert(r4.ScopeName == "meter.lib", "r4 ScopeName mismatch")
    assert(r4.ScopeVersion == "3.2.1", "r4 ScopeVersion mismatch")
    assert(math.abs(r4.Value - 15.5) < 1e-6, "r4 Value mismatch")
    assert(r4.ResourceAttributes["service.name"] == "table-metric-svc", "r4 ResourceAttributes mismatch")

    local r5 = emitted[5].log
    assert(r5.MetricName == "arr.gauge.1", "r5 MetricName mismatch")
    assert(r5.Attributes.attr_k == "attr_v", "r5 Attributes mismatch")

    local r6 = emitted[6].log
    assert(r6.MetricName == "arr.gauge.2", "r6 MetricName mismatch")

    local r7 = emitted[7].log
    assert(r7.Histogram.Count == 3, "r7 Histogram Count mismatch")
    assert(#r7.Histogram.BucketCounts == 2, "r7 BucketCounts length mismatch")
    assert(r7.Histogram.BucketCounts[2] == 2, "r7 string bucket count parsed mismatch")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_more), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, MetricsZeroBucketCountsAndNonOtlpExclusions) {
  // 1. Aggregated histogram with empty bucket counts
  const char* script = R"(
    return {
      name = "hist.empty.buckets",
      kind = "aggregated_histogram",
      aggregated_histogram = {
        count = 0,
        sum = 0.0,
        bucket_counts = {},
        explicit_bounds = {}
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK) << lua_tostring(L_, -1);
  int table_idx = lua_gettop(L_);
  auto res1 = ProcessNativeMetricsTable(L_, table_idx, emit_idx_);
  lua_pop(L_, 1);
  EXPECT_TRUE(res1.success) << res1.error;
  EXPECT_EQ(res1.emitted_count, 1);

  // 2. Non-OTLP metric variants (distribution, set, sketch) in table mode
  const char* script_unsupported = R"(
    return {
      name = "dist.metric",
      kind = "distribution",
      distribution = {
        samples = { 1.0, 2.0 }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, script_unsupported), LUA_OK) << lua_tostring(L_, -1);
  int bad_tbl_idx = lua_gettop(L_);
  auto res2 = ProcessNativeMetricsTable(L_, bad_tbl_idx, emit_idx_);
  lua_pop(L_, 1);
  EXPECT_FALSE(res2.success);
  EXPECT_NE(res2.error.find("Unsupported non-OTLP metric variant 'distribution'"),
            std::string::npos);

  // 3. Non-OTLP metric variants in JSON mode
  std::string json_sketch = R"({
    "name": "sketch.metric",
    "kind": "sketch",
    "sketch": {"bins": []}
  })";
  auto res3 = ProcessNativeMetrics(L_, json_sketch, emit_idx_);
  EXPECT_FALSE(res3.success);
  EXPECT_NE(res3.error.find("Unsupported non-OTLP metric variant 'sketch'"), std::string::npos);
}

TEST_F(NativeExtractorsTest, MetricsOtelPrefixTagsAndParallelQuantilesJson) {
  // 1. JSON metric with otel prefix tags and array tags
  std::string json_tags = R"({
    "name": "rpc.calls",
    "tags": {
      "otel.unit": "requests",
      "otel.description": "Number of RPC requests",
      "otel.scope.name": "rpc.meter",
      "otel.scope.version": "1.5.0",
      "endpoints": ["/api/v1", "/api/v2"]
    },
    "counter": {"value": 100}
  })";
  auto res1 = ProcessNativeMetrics(L_, json_tags, emit_idx_);
  EXPECT_TRUE(res1.success) << res1.error;
  EXPECT_EQ(res1.emitted_count, 1);

  // 2. JSON summary with parallel quantiles and values
  std::string json_summary = R"({
    "name": "rpc.duration",
    "aggregated_summary": {
      "count": 50,
      "sum": 125.5,
      "quantiles": [0.5, 0.95, 0.99],
      "values": [1.0, 5.0, 10.0]
    }
  })";
  auto res2 = ProcessNativeMetrics(L_, json_summary, emit_idx_);
  EXPECT_TRUE(res2.success) << res2.error;
  EXPECT_EQ(res2.emitted_count, 1);

  // 3. JSON histogram with min, max, counts, and explicitBounds
  std::string json_hist = R"({
    "name": "rpc.payload_size",
    "aggregated_histogram": {
      "count": 20,
      "sum": 2048.0,
      "min": 10.0,
      "max": 500.0,
      "counts": [5, 10, 5],
      "explicitBounds": [50.0, 200.0]
    }
  })";
  auto res3 = ProcessNativeMetrics(L_, json_hist, emit_idx_);
  EXPECT_TRUE(res3.success) << res3.error;
  EXPECT_EQ(res3.emitted_count, 1);

  const char* verify = R"(
    assert(#emitted >= 3)
    local m1 = emitted[#emitted - 2].log
    assert(m1.MetricUnit == "requests")
    assert(m1.MetricDescription == "Number of RPC requests")
    assert(m1.ScopeName == "rpc.meter")
    assert(m1.ScopeVersion == "1.5.0")
    assert(type(m1.Attributes.endpoints) == "string")

    local m2 = emitted[#emitted - 1].log
    assert(m2.MetricType == "summary")
    assert(type(m2.Summary) == "table")
    assert(#m2.Summary.QuantileValues == 3)
    assert(m2.Summary.QuantileValues[1].Quantile == 0.5)
    assert(m2.Summary.QuantileValues[1].Value == 1.0)

    local m3 = emitted[#emitted].log
    assert(m3.MetricType == "histogram")
    assert(type(m3.Histogram) == "table")
    assert(m3.Histogram.Min == 10.0)
    assert(m3.Histogram.Max == 500.0)
    assert(#m3.Histogram.BucketCounts == 3)
    assert(#m3.Histogram.ExplicitBounds == 2)
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, NativeLogsJsonFullAllFieldsCoverage) {
  std::string json = R"({
    "trace_id": "4bf92f3577b34da6a3ce929d0e0e4736",
    "span_id": "00f067aa0ba902b7",
    "flags": 1,
    "severity_text": "WARN",
    "severity_number": "SEVERITY_NUMBER_WARN",
    "body": {"stringValue": "log body via stringValue"},
    "event_name": "app.boot",
    "resource_schema_url": "https://opentelemetry.io/schemas/1.21.0",
    "resources": {
      "service.name": "full-svc",
      "env": "prod"
    },
    "scope": {
      "name": "app.logger",
      "version": "1.0.0",
      "schema_url": "https://opentelemetry.io/schemas/scope/1.0",
      "attributes": {"sc_key": "sc_val"}
    },
    "attributes": {
      "http.status": 200,
      "user": "alice"
    }
  })";
  auto res = ProcessNativeLogs(L_, json, emit_idx_);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 1);

  const char* verify = R"(
    local l = emitted[#emitted].log
    assert(l.TraceId == "4bf92f3577b34da6a3ce929d0e0e4736")
    assert(l.SpanId == "00f067aa0ba902b7")
    assert(l.TraceFlags == 1)
    assert(l.SeverityText == "WARN")
    assert(l.SeverityNumber == 13)
    assert(l.Body == "log body via stringValue")
    assert(l.EventName == "app.boot")
    assert(l.ResourceSchemaUrl == "https://opentelemetry.io/schemas/1.21.0")
    assert(l.ServiceName == "full-svc")
    assert(l.ResourceAttributes["service.name"] == "full-svc")
    assert(l.ScopeName == "app.logger")
    assert(l.ScopeVersion == "1.0.0")
    assert(l.ScopeSchemaUrl == "https://opentelemetry.io/schemas/scope/1.0")
    assert(l.ScopeAttributes.sc_key == "sc_val")
    assert(l.LogAttributes["http.status"] == 200)
    assert(l.LogAttributes.user == "alice")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, NativeTracesJsonFullAllFieldsCoverage) {
  std::string json = R"({
    "trace_id": "4bf92f3577b34da6a3ce929d0e0e4736",
    "span_id": "00f067aa0ba902b7",
    "parent_span_id": "5fb397be34d23b0f",
    "name": "full_trace_span",
    "kind": "client",
    "start_time_unix_nano": "1700000000000000000",
    "end_time_unix_nano": "1700000000050000000",
    "status": {"code": "STATUS_CODE_UNSET", "message": "unset status"},
    "resources": {"service.name": "trace-client-svc"},
    "scope": {"name": "client.tracer", "version": "2.0"},
    "attributes": {"req.header": "custom"},
    "events": [
      {
        "name": "cache_lookup",
        "time_unix_nano": "1700000000010000000",
        "attributes": {
          "hit": true,
          "u_val": {"uint_value": 100},
          "arr_val": {"array_value": {"values": []}},
          "kv_val": {"kvlist_value": {"values": []}}
        }
      }
    ],
    "links": [
      {
        "trace_id": "4bf92f3577b34da6a3ce929d0e0e4737",
        "span_id": "00f067aa0ba902b8",
        "attributes": {"link.reason": "parent_job"}
      }
    ]
  })";
  auto res = ProcessNativeTraces(L_, json, emit_idx_);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 1);

  const char* verify = R"(
    local t = emitted[#emitted].log
    assert(t.SpanKind == "SPAN_KIND_CLIENT")
    assert(t.StatusCode == "STATUS_CODE_UNSET")
    assert(t.ServiceName == "trace-client-svc")
    assert(t.ScopeName == "client.tracer")
    assert(type(t.Events) == "string" and t.Events:find("cache_lookup") ~= nil)
    assert(type(t.Links) == "string" and t.Links:find("parent_job") ~= nil)
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, NativeLogsAll24SeverityStringsJson) {
  const char* sev_names[] = {
      "SEVERITY_NUMBER_TRACE",  "SEVERITY_NUMBER_TRACE2", "SEVERITY_NUMBER_TRACE3",
      "SEVERITY_NUMBER_TRACE4", "SEVERITY_NUMBER_DEBUG",  "SEVERITY_NUMBER_DEBUG2",
      "SEVERITY_NUMBER_DEBUG3", "SEVERITY_NUMBER_DEBUG4", "SEVERITY_NUMBER_INFO",
      "SEVERITY_NUMBER_INFO2",  "SEVERITY_NUMBER_INFO3",  "SEVERITY_NUMBER_INFO4",
      "SEVERITY_NUMBER_WARN",   "SEVERITY_NUMBER_WARN2",  "SEVERITY_NUMBER_WARN3",
      "SEVERITY_NUMBER_WARN4",  "SEVERITY_NUMBER_ERROR",  "SEVERITY_NUMBER_ERROR2",
      "SEVERITY_NUMBER_ERROR3", "SEVERITY_NUMBER_ERROR4", "SEVERITY_NUMBER_FATAL",
      "SEVERITY_NUMBER_FATAL2", "SEVERITY_NUMBER_FATAL3", "SEVERITY_NUMBER_FATAL4"};

  std::string json = "[";
  for (int i = 0; i < 24; ++i) {
    if (i > 0) json += ",";
    json += R"({"severity_number":")" + std::string(sev_names[i]) + R"(","message":"msg"})";
  }
  json += "]";

  auto res = ProcessNativeLogs(L_, json, emit_idx_);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 24);

  const char* verify = R"(
    assert(#emitted >= 24)
    for i = 1, 24 do
      local l = emitted[#emitted - 24 + i].log
      assert(l.SeverityNumber == i, "Expected severity " .. i .. ", got " .. tostring(l.SeverityNumber))
    end
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, NativeLogsNestedResourceAttributesAndTopLevelServiceTable) {
  // Test nested attributes table in resources, top-level scope_attributes, and service_name
  const char* script = R"(
    return {
      message = "nested res test",
      service_name = "top-level-svc",
      resources = {
        schema_url = "https://schema.example.com",
        attributes = {
          ["env"] = "staging"
        }
      },
      scope_attributes = {
        ["scope_k"] = "scope_v"
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK) << lua_tostring(L_, -1);
  auto res = ProcessNativeLogsTable(L_, lua_gettop(L_), emit_idx_);
  lua_pop(L_, 1);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 1);

  const char* verify = R"(
    local l = emitted[#emitted].log
    assert(l.ServiceName == "top-level-svc")
    assert(l.ResourceSchemaUrl == "https://schema.example.com")
    assert(l.ResourceAttributes["env"] == "staging")
    assert(l.ScopeAttributes["scope_k"] == "scope_v")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, NativeTracesAllSpanKindsAndStatusCodesJsonAndTable) {
  // Test kinds 1..5 and status codes 0..2 in JSON and table modes
  std::string json = R"([
    {"trace_id":"1","span_id":"1","name":"k1","kind":1,"status":{"code":0}},
    {"trace_id":"2","span_id":"2","name":"k2","kind":2,"status":{"code":1}},
    {"trace_id":"3","span_id":"3","name":"k3","kind":3,"status":{"code":2}},
    {"trace_id":"4","span_id":"4","name":"k4","kind":4,"status":{"code":"STATUS_CODE_OK"}},
    {"trace_id":"5","span_id":"5","name":"k5","kind":5,"status":{"code":"STATUS_CODE_ERROR"}},
    {"trace_id":"6","span_id":"6","name":"k0","kind":0,"status":{"code":99}}
  ])";
  auto res_json = ProcessNativeTraces(L_, json, emit_idx_);
  EXPECT_TRUE(res_json.success) << res_json.error;
  EXPECT_EQ(res_json.emitted_count, 6);

  const char* verify = R"(
    local s1 = emitted[#emitted - 5].log
    assert(s1.SpanKind == "SPAN_KIND_INTERNAL")
    assert(s1.StatusCode == "STATUS_CODE_UNSET")

    local s2 = emitted[#emitted - 4].log
    assert(s2.SpanKind == "SPAN_KIND_SERVER")
    assert(s2.StatusCode == "STATUS_CODE_OK")

    local s3 = emitted[#emitted - 3].log
    assert(s3.SpanKind == "SPAN_KIND_CLIENT")
    assert(s3.StatusCode == "STATUS_CODE_ERROR")

    local s4 = emitted[#emitted - 2].log
    assert(s4.SpanKind == "SPAN_KIND_PRODUCER")
    assert(s4.StatusCode == "STATUS_CODE_OK")

    local s5 = emitted[#emitted - 1].log
    assert(s5.SpanKind == "SPAN_KIND_CONSUMER")
    assert(s5.StatusCode == "STATUS_CODE_ERROR")

    local s6 = emitted[#emitted].log
    assert(s6.SpanKind == "SPAN_KIND_UNSPECIFIED")
    assert(s6.StatusCode == "STATUS_CODE_UNSET")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, MetricsMergeTagsAndResourcesAttributesJsonAndStringBoundsTable) {
  // 1. JSON metric with tags AND resources/scope/attributes/service_name merging
  std::string json = R"({
    "name": "merged.metric",
    "service_name": "top-svc-metric",
    "tags": {
      "resource.k1": "v1",
      "scope.s1": "sv1",
      "attr1": "val1"
    },
    "resources": {
      "k2": "v2"
    },
    "scope": {
      "name": "scope-merged",
      "attributes": {
        "s2": "sv2"
      }
    },
    "attributes": {
      "attr2": "val2"
    },
    "counter": {"value": "99.9"}
  })";
  auto res_json = ProcessNativeMetrics(L_, json, emit_idx_);
  EXPECT_TRUE(res_json.success) << res_json.error;
  EXPECT_EQ(res_json.emitted_count, 1);

  // 2. Table metric with string explicit bounds and string counter
  const char* script_bounds = R"(
    return {
      name = "hist.str.bounds",
      histogram = {
        count = 10,
        sum = 25.0,
        bucket_counts = { 2, 8 },
        explicit_bounds = { "1.5", "10.0" }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, script_bounds), LUA_OK) << lua_tostring(L_, -1);
  auto res_tbl = ProcessNativeMetricsTable(L_, lua_gettop(L_), emit_idx_);
  lua_pop(L_, 1);
  EXPECT_TRUE(res_tbl.success) << res_tbl.error;
  EXPECT_EQ(res_tbl.emitted_count, 1);

  const char* verify = R"(
    local m1 = emitted[#emitted - 1].log
    assert(m1.ServiceName == "top-svc-metric")
    assert(m1.ResourceAttributes.k1 == "v1")
    assert(m1.ResourceAttributes.k2 == "v2")
    assert(m1.ScopeAttributes.s1 == "sv1")
    assert(m1.ScopeAttributes.s2 == "sv2")
    assert(m1.Attributes.attr1 == "val1")
    assert(m1.Attributes.attr2 == "val2")
    assert(math.abs(m1.Value - 99.9) < 1e-4)

    local m2 = emitted[#emitted].log
    assert(type(m2.Histogram) == "table")
    assert(#m2.Histogram.ExplicitBounds == 2)
    assert(m2.Histogram.ExplicitBounds[1] == 1.5)
    assert(m2.Histogram.ExplicitBounds[2] == 10.0)
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(NativeExtractorsTest, MetricTableAsDoubleAndAsIntVariants) {
  const char* script = R"(
    return {
      {
        name = "m.as_double",
        gauge = { as_double = 12.34 }
      },
      {
        name = "m.as_int",
        counter = { as_int = 5678 }
      },
      {
        name = "m.asDouble",
        gauge = { asDouble = "99.5" }
      },
      {
        name = "m.asInt",
        counter = { asInt = "4321" }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK) << lua_tostring(L_, -1);
  auto res = ProcessNativeMetricsTable(L_, lua_gettop(L_), emit_idx_);
  lua_pop(L_, 1);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 4);

  const char* verify = R"(
    assert(math.abs(emitted[#emitted - 3].log.Value - 12.34) < 1e-4)
    assert(emitted[#emitted - 2].log.Value == 5678)
    assert(math.abs(emitted[#emitted - 1].log.Value - 99.5) < 1e-4)
    assert(emitted[#emitted].log.Value == 4321)
  )";
  EXPECT_EQ(luaL_dostring(L_, verify), LUA_OK) << lua_tostring(L_, -1);
}

}  // namespace
}  // namespace native_extractors
}  // namespace vector_otel
