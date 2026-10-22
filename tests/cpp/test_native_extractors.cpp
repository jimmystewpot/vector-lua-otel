#include <gtest/gtest.h>

#include <string>
#include <string_view>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include "vector_otel/native_extractors/native_logs_extractor.hpp"
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

}  // namespace
}  // namespace native_extractors
}  // namespace vector_otel
