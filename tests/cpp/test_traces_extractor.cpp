#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

#include "vector_otel/core/timestamp.hpp"
#include "vector_otel/extractors/traces_extractor.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace vector_otel {
namespace extractors {
namespace {

class TracesExtractorTest : public ::testing::Test {
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

  std::string LoadFixture(const std::string& rel_path) {
    std::ifstream f(rel_path);
    if (!f.is_open()) {
      f.open("../" + rel_path);
    }
    if (!f.is_open()) {
      f.open("../../" + rel_path);
    }
    EXPECT_TRUE(f.is_open()) << "Failed to open fixture: " << rel_path;
    std::stringstream buffer;
    buffer << f.rdbuf();
    return buffer.str();
  }

  lua_State* L_{nullptr};
  int emit_idx_{0};
};

TEST_F(TracesExtractorTest, SampleFixtureParentAndChildSpans) {
  std::string json = LoadFixture("tests/fixtures/traces_sample.json");
  ASSERT_FALSE(json.empty());

  int initial_top = lua_gettop(L_);
  auto result = ProcessTraces(L_, json, emit_idx_);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.error, "");
  EXPECT_EQ(result.emitted_count, 2);
  EXPECT_EQ(lua_gettop(L_), initial_top);

  const char* verify_script = R"(
    assert(#emitted == 2, "Expected 2 emitted spans, got " .. #emitted)

    -- Span 1: Parent Root Span
    local r1 = emitted[1].log
    assert(r1 ~= nil, "emitted[1].log must exist")
    assert(r1.TraceId == "4bf92f3577b34da6a3ce929d0e0e4736", "r1.TraceId mismatch: " .. tostring(r1.TraceId))
    assert(r1.SpanId == "00f067aa0ba902b7", "r1.SpanId mismatch: " .. tostring(r1.SpanId))
    assert(r1.ParentSpanId == "", "r1.ParentSpanId must be empty for root span, got: " .. tostring(r1.ParentSpanId))
    assert(r1.TraceState == "rojo=1,congo=2", "r1.TraceState mismatch: " .. tostring(r1.TraceState))
    assert(r1.SpanName == "HTTP GET /checkout", "r1.SpanName mismatch: " .. tostring(r1.SpanName))
    assert(r1.SpanKind == "SPAN_KIND_SERVER", "r1.SpanKind mismatch: " .. tostring(r1.SpanKind))
    assert(r1.ServiceName == "cart-service", "r1.ServiceName mismatch: " .. tostring(r1.ServiceName))
    assert(r1.ScopeName == "opentelemetry.instrumentation.http", "r1.ScopeName mismatch: " .. tostring(r1.ScopeName))
    assert(r1.ScopeVersion == "1.25.0", "r1.ScopeVersion mismatch: " .. tostring(r1.ScopeVersion))
    assert(r1.StatusCode == "STATUS_CODE_OK", "r1.StatusCode mismatch: " .. tostring(r1.StatusCode))
    assert(r1.StatusMessage == "OK", "r1.StatusMessage mismatch: " .. tostring(r1.StatusMessage))

    -- Timestamp & Duration
    assert(r1.Timestamp == "2024-03-09T16:00:00.000000000Z", "r1.Timestamp mismatch: " .. tostring(r1.Timestamp))
    -- Duration = 1710000000500000000 - 1710000000000000000 = 500000000 ns
    assert(r1.Duration == 500000000, "r1.Duration mismatch: " .. tostring(r1.Duration))

    -- ResourceAttributes
    assert(type(r1.ResourceAttributes) == "table", "r1.ResourceAttributes must be a table")
    assert(r1.ResourceAttributes["service.name"] == "cart-service")
    assert(r1.ResourceAttributes["deployment.environment"] == "production")
    assert(r1.ResourceAttributes["host.id"] == 12345)

    -- SpanAttributes
    assert(type(r1.SpanAttributes) == "table", "r1.SpanAttributes must be a table")
    assert(r1.SpanAttributes["http.method"] == "GET")
    assert(r1.SpanAttributes["http.status_code"] == 200)
    assert(r1.SpanAttributes["http.route"] == "/checkout")

    -- Events (JSON-stringified array)
    assert(type(r1.Events) == "string", "r1.Events must be a JSON string")
    assert(r1.Events:sub(1, 1) == "[", "r1.Events must start with [")
    assert(string.find(r1.Events, "cart_loaded") ~= nil, "r1.Events missing cart_loaded")
    assert(string.find(r1.Events, "2024%-03%-09T16:00:00.100000000Z") ~= nil, "r1.Events missing formatted Timestamp")
    assert(string.find(r1.Events, "item_count") ~= nil, "r1.Events missing item_count")

    -- Links (JSON-stringified array)
    assert(type(r1.Links) == "string", "r1.Links must be a JSON string")
    assert(r1.Links:sub(1, 1) == "[", "r1.Links must start with [")
    assert(string.find(r1.Links, "7bf92f3577b34da6a3ce929d0e0e4736") ~= nil, "r1.Links missing TraceId")
    assert(string.find(r1.Links, "10f067aa0ba902b7") ~= nil, "r1.Links missing SpanId")
    assert(string.find(r1.Links, "state1=val1") ~= nil, "r1.Links missing TraceState")
    assert(string.find(r1.Links, "related_order") ~= nil, "r1.Links missing related_order")

    -- processed_at
    assert(type(r1.processed_at) == "string", "r1.processed_at must be string")
    assert(string.find(r1.processed_at, "Z$") ~= nil, "r1.processed_at must be ISO-8601 UTC")

    -- Span 2: Child Span
    local r2 = emitted[2].log
    assert(r2 ~= nil, "emitted[2].log must exist")
    assert(r2.TraceId == "4bf92f3577b34da6a3ce929d0e0e4736", "r2.TraceId mismatch")
    assert(r2.SpanId == "5fb397be34d23b0f", "r2.SpanId mismatch")
    assert(r2.ParentSpanId == "00f067aa0ba902b7", "r2.ParentSpanId mismatch: " .. tostring(r2.ParentSpanId))
    assert(r2.TraceState == "", "r2.TraceState must be empty string")
    assert(r2.SpanName == "SELECT * FROM cart_items", "r2.SpanName mismatch")
    assert(r2.SpanKind == "SPAN_KIND_CLIENT", "r2.SpanKind mismatch")
    assert(r2.ServiceName == "cart-service", "r2.ServiceName mismatch")
    assert(r2.StatusCode == "STATUS_CODE_ERROR", "r2.StatusCode mismatch")
    assert(r2.StatusMessage == "Connection timeout", "r2.StatusMessage mismatch")
    -- Duration = 1710000000250000000 - 1710000000150000000 = 100000000 ns
    assert(r2.Duration == 100000000, "r2.Duration mismatch: " .. tostring(r2.Duration))
    assert(r2.SpanAttributes["db.system"] == "postgresql")
    assert(r2.Events == "[]", "r2.Events empty array mismatch: " .. tostring(r2.Events))
    assert(r2.Links == "[]", "r2.Links empty array mismatch: " .. tostring(r2.Links))
  )";

  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(TracesExtractorTest, UnorderedKeysHandling) {
  // Scramble keys in resourceSpans, scopeSpans, and spans
  const char* unordered_json = R"({
    "resourceSpans": [
      {
        "scopeSpans": [
          {
            "spans": [
              {
                "status": {
                  "message": "done",
                  "code": 1
                },
                "endTimeUnixNano": "2000000000",
                "events": [],
                "name": "unordered_op",
                "links": [],
                "kind": "SPAN_KIND_PRODUCER",
                "traceId": "11112222333344445555666677778888",
                "startTimeUnixNano": "1000000000",
                "attributes": [
                  {"key": "tag1", "value": {"stringValue": "val1"}}
                ],
                "spanId": "aaaabbbbccccdddd"
              }
            ],
            "scope": {
              "version": "0.9.0",
              "name": "unordered_scope"
            }
          }
        ],
        "resource": {
          "attributes": [
            {"key": "service.name", "value": {"stringValue": "unordered-svc"}}
          ]
        }
      }
    ]
  })";

  auto result = ProcessTraces(L_, unordered_json, emit_idx_);
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.emitted_count, 1);

  const char* verify_script = R"(
    assert(#emitted == 1, "Expected 1 emitted span")
    local r = emitted[1].log
    assert(r.SpanName == "unordered_op", "SpanName mismatch: " .. tostring(r.SpanName))
    assert(r.TraceId == "11112222333344445555666677778888", "TraceId mismatch")
    assert(r.SpanId == "aaaabbbbccccdddd", "SpanId mismatch")
    assert(r.ServiceName == "unordered-svc", "ServiceName mismatch")
    assert(r.ScopeName == "unordered_scope", "ScopeName mismatch")
    assert(r.ScopeVersion == "0.9.0", "ScopeVersion mismatch")
    assert(r.SpanKind == "SPAN_KIND_PRODUCER", "SpanKind mismatch")
    assert(r.StatusCode == "STATUS_CODE_OK", "StatusCode mismatch")
    assert(r.StatusMessage == "done", "StatusMessage mismatch")
    assert(r.Duration == 1000000000, "Duration mismatch")
    assert(r.SpanAttributes["tag1"] == "val1", "SpanAttributes mismatch")
    assert(r.Events == "[]")
    assert(r.Links == "[]")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(TracesExtractorTest, OutOfOrderFieldsExtractedCorrectly) {
  const std::string json = R"({
    "resourceSpans": [{
      "resource": {},
      "scopeSpans": [{
        "scope": {},
        "spans": [{
          "attributes": [{"key": "app", "value": {"stringValue": "demo"}}],
          "status": {"message": "ok", "code": 1},
          "events": [{"timeUnixNano": "1700000000100000000", "name": "ev1"}],
          "links": [{"traceId": "0123456789abcdef0123456789abcdef", "spanId": "0123456789abcdef"}],
          "name": "span_op",
          "endTimeUnixNano": "1700000001000000000",
          "startTimeUnixNano": "1700000000000000000",
          "spanId": "0123456789abcdef",
          "traceId": "0123456789abcdef0123456789abcdef"
        }]
      }]
    }]
  })";
  auto res = ProcessTraces(L_, json, emit_idx_);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(res.emitted_count, 1);

  const char* verify_script = R"(
    local r = emitted[#emitted].log
    assert(r ~= nil)
    assert(r.SpanName == "span_op")
    assert(r.SpanAttributes.app == "demo")
    assert(r.StatusCode == "STATUS_CODE_OK")
    assert(r.StatusMessage == "ok")
    assert(r.Duration == 1000000000)
    assert(r.Events:find("ev1") ~= nil)
    assert(r.Links:find("0123456789abcdef") ~= nil)
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(TracesExtractorTest, EmptyAndMissingBatches) {
  // 1. Empty JSON object
  auto r1 = ProcessTraces(L_, "{}", emit_idx_);
  EXPECT_TRUE(r1.success);
  EXPECT_EQ(r1.emitted_count, 0);

  // 2. Empty resourceSpans array
  auto r2 = ProcessTraces(L_, "{\"resourceSpans\": []}", emit_idx_);
  EXPECT_TRUE(r2.success);
  EXPECT_EQ(r2.emitted_count, 0);

  // 3. ResourceSpans with empty scopeSpans
  auto r3 =
      ProcessTraces(L_, "{\"resourceSpans\": [{\"resource\": {}, \"scopeSpans\": []}]}", emit_idx_);
  EXPECT_TRUE(r3.success);
  EXPECT_EQ(r3.emitted_count, 0);

  // 4. Missing optional fields inside span
  const char* minimal_json = R"({
    "resourceSpans": [{
      "resource": {},
      "scopeSpans": [{
        "spans": [{
          "traceId": "0123456789abcdef0123456789abcdef",
          "spanId": "0123456789abcdef"
        }]
      }]
    }]
  })";
  auto r4 = ProcessTraces(L_, minimal_json, emit_idx_);
  EXPECT_TRUE(r4.success);
  EXPECT_EQ(r4.emitted_count, 1);

  const char* verify_script = R"(
    assert(#emitted == 1, "Expected 1 emitted span")
    local r = emitted[1].log
    assert(r.TraceId == "0123456789abcdef0123456789abcdef")
    assert(r.SpanId == "0123456789abcdef")
    assert(r.ParentSpanId == "")
    assert(r.TraceState == "")
    assert(r.SpanName == "")
    assert(r.SpanKind == "SPAN_KIND_UNSPECIFIED")
    assert(r.ServiceName == "unknown_service")
    assert(r.ScopeName == "")
    assert(r.ScopeVersion == "")
    assert(r.StatusCode == "STATUS_CODE_UNSET")
    assert(r.StatusMessage == "")
    assert(r.Duration == 0)
    assert(r.Events == "[]")
    assert(r.Links == "[]")
    assert(type(r.ResourceAttributes) == "table")
    assert(type(r.SpanAttributes) == "table")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(TracesExtractorTest, SpanKindAndStatusCodeEnumsAndStrings) {
  // Test numeric kind 0..5 and string kinds, numeric status 0..2 and string
  // status
  const char* enum_json = R"({
    "resourceSpans": [{
      "resource": {},
      "scopeSpans": [{
        "spans": [
          {"traceId": "1", "spanId": "1", "kind": 0, "status": {"code": 0}},
          {"traceId": "2", "spanId": "2", "kind": 1, "status": {"code": "OK"}},
          {"traceId": "3", "spanId": "3", "kind": 4, "status": {"code": "STATUS_CODE_ERROR"}},
          {"traceId": "4", "spanId": "4", "kind": 5, "status": {"code": 2}},
          {"traceId": "5", "spanId": "5", "kind": "CONSUMER", "status": {"code": "ERROR"}}
        ]
      }]
    }]
  })";

  auto res = ProcessTraces(L_, enum_json, emit_idx_);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(res.emitted_count, 5);

  const char* verify_script = R"(
    assert(#emitted == 5)
    assert(emitted[1].log.SpanKind == "SPAN_KIND_UNSPECIFIED")
    assert(emitted[1].log.StatusCode == "STATUS_CODE_UNSET")

    assert(emitted[2].log.SpanKind == "SPAN_KIND_INTERNAL")
    assert(emitted[2].log.StatusCode == "STATUS_CODE_OK")

    assert(emitted[3].log.SpanKind == "SPAN_KIND_PRODUCER")
    assert(emitted[3].log.StatusCode == "STATUS_CODE_ERROR")

    assert(emitted[4].log.SpanKind == "SPAN_KIND_CONSUMER")
    assert(emitted[4].log.StatusCode == "STATUS_CODE_ERROR")

    assert(emitted[5].log.SpanKind == "SPAN_KIND_CONSUMER")
    assert(emitted[5].log.StatusCode == "STATUS_CODE_ERROR")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(TracesExtractorTest, MalformedJsonHaltAndError) {
  auto res = ProcessTraces(L_, "{\"resourceSpans\": [unclosed", emit_idx_);
  EXPECT_FALSE(res.success);
  EXPECT_FALSE(res.error.empty());
  EXPECT_EQ(res.emitted_count, 0);
}

TEST_F(TracesExtractorTest, DurationInt64Preservation) {
  // Test large timestamps and int64 duration
  const char* duration_json = R"({
    "resourceSpans": [{
      "resource": {},
      "scopeSpans": [{
        "spans": [{
          "traceId": "1",
          "spanId": "1",
          "startTimeUnixNano": "1700000000000000000",
          "endTimeUnixNano": "1700000005000000000"
        }]
      }]
    }]
  })";
  auto res = ProcessTraces(L_, duration_json, emit_idx_);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(res.emitted_count, 1);

  const char* verify_script = R"(
    assert(emitted[1].log.Duration == 5000000000)
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(TracesExtractorTest, DurationInt64OverflowClamped) {
  // Test JSON mode with start_time = 1 and end_time = UINT64_MAX
  // (18446744073709551615)
  const char* overflow_json = R"({
    "resourceSpans": [{
      "resource": {},
      "scopeSpans": [{
        "spans": [{
          "traceId": "1",
          "spanId": "1",
          "startTimeUnixNano": "1",
          "endTimeUnixNano": "18446744073709551615"
        }]
      }]
    }]
  })";
  auto res = ProcessTraces(L_, overflow_json, emit_idx_);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(res.emitted_count, 1);

  const char* verify_json_script = R"(
    assert(#emitted == 1, "Expected 1 emitted record, got " .. #emitted)
    local r = emitted[1].log
    assert(r.Duration > 0, "Duration must not underflow to negative: " .. tostring(r.Duration))
    assert(r.Duration == math.maxinteger, "Duration must be clamped to INT64_MAX: " .. tostring(r.Duration))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_json_script), LUA_OK) << lua_tostring(L_, -1);

  // Clear emitted table for table mode verification
  ASSERT_EQ(luaL_dostring(L_, "emitted = {}"), LUA_OK);

  // Test table mode with start_time = 1 and end_time = UINT64_MAX
  const char* table_script = R"(
    return {
      resourceSpans = {
        {
          scopeSpans = {
            {
              spans = {
                {
                  traceId = "2",
                  spanId = "2",
                  startTimeUnixNano = 1,
                  endTimeUnixNano = "18446744073709551615"
                }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, table_script), LUA_OK);
  int table_idx = lua_gettop(L_);

  auto table_res = ProcessTracesTable(L_, table_idx, emit_idx_);
  EXPECT_TRUE(table_res.success);
  EXPECT_EQ(table_res.emitted_count, 1);
  lua_pop(L_, 1);  // pop return table

  const char* verify_table_script = R"(
    assert(#emitted == 1, "Expected 1 emitted record in table mode, got " .. #emitted)
    local r = emitted[1].log
    assert(r.Duration > 0, "Duration must not underflow to negative: " .. tostring(r.Duration))
    assert(r.Duration == math.maxinteger, "Duration must be clamped to INT64_MAX: " .. tostring(r.Duration))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_table_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(TracesExtractorTest, ProcessTracesTablePreservesLuaStackDepthOnNestedWrapperAndError) {
  // 1. Nested { trace = { resourceSpans = { ... } } }
  const char* trace_wrapper_script = R"(
    return {
      trace = {
        resourceSpans = {
          {
            resource = {
              attributes = {
                { key = "service.name", value = { stringValue = "nested-trace-svc" } }
              }
            },
            scopeSpans = {
              {
                spans = {
                  {
                    traceId = "11112222333344445555666677778888",
                    spanId = "1111222233334444",
                    name = "nested-span"
                  }
                }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, trace_wrapper_script), LUA_OK);
  int table_idx = lua_gettop(L_);
  int initial_top = lua_gettop(L_);

  auto res1 = ProcessTracesTable(L_, table_idx, emit_idx_);
  EXPECT_TRUE(res1.success);
  EXPECT_EQ(res1.emitted_count, 1);
  EXPECT_EQ(lua_gettop(L_), initial_top);
  lua_pop(L_, 1);

  // 2. Nested { log = { resourceSpans = { ... } } }
  const char* log_wrapper_script = R"(
    return {
      log = {
        resourceSpans = {
          {
            scopeSpans = {
              {
                spans = {
                  {
                    traceId = "22223333444455556666777788889999",
                    spanId = "2222333344445555"
                  }
                }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, log_wrapper_script), LUA_OK);
  table_idx = lua_gettop(L_);
  initial_top = lua_gettop(L_);

  auto res2 = ProcessTracesTable(L_, table_idx, emit_idx_);
  EXPECT_TRUE(res2.success);
  EXPECT_EQ(res2.emitted_count, 1);
  EXPECT_EQ(lua_gettop(L_), initial_top);
  lua_pop(L_, 1);

  // 3. { trace = { unrelated = 1 } }
  const char* unrelated_script = R"(
    return {
      trace = {
        unrelated = 1
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, unrelated_script), LUA_OK);
  table_idx = lua_gettop(L_);
  initial_top = lua_gettop(L_);

  auto res3 = ProcessTracesTable(L_, table_idx, emit_idx_);
  EXPECT_TRUE(res3.success);
  EXPECT_EQ(res3.emitted_count, 0);
  EXPECT_EQ(lua_gettop(L_), initial_top);
  lua_pop(L_, 1);

  // 4. Error in emit callback during nested { trace = { resourceSpans = ... } }
  const char* err_emit_script = R"(
    function err_emit(event)
      error("simulated emit failure")
    end
  )";
  ASSERT_EQ(luaL_dostring(L_, err_emit_script), LUA_OK);
  lua_getglobal(L_, "err_emit");
  int err_emit_idx = lua_gettop(L_);

  ASSERT_EQ(luaL_dostring(L_, trace_wrapper_script), LUA_OK);
  table_idx = lua_gettop(L_);
  initial_top = lua_gettop(L_);

  auto res4 = ProcessTracesTable(L_, table_idx, err_emit_idx);
  EXPECT_FALSE(res4.success);
  EXPECT_EQ(lua_gettop(L_), initial_top);
  lua_pop(L_, 2);  // pop table and err_emit
}

TEST_F(TracesExtractorTest, SpanLinksParsingComprehensive) {
  const char* json = R"({
    "resourceSpans": [{
      "resource": {
        "attributes": [{"key": "service.name", "value": {"stringValue": "link-svc"}}]
      },
      "scopeSpans": [{
        "scope": {"name": "link-scope", "version": "1.0"},
        "spans": [{
          "traceId": "4bf92f3577b34da6a3ce929d0e0e4736",
          "spanId": "00f067aa0ba902b7",
          "name": "span-with-links",
          "startTimeUnixNano": "1700000000000000000",
          "endTimeUnixNano": "1700000001000000000",
          "links": [
            {
              "traceId": "11112222333344445555666677778888",
              "spanId": "1111222233334444",
              "traceState": "rojo=1,congo=2",
              "flags": 1,
              "droppedAttributesCount": 2,
              "attributes": [
                {"key": "str_attr", "value": {"stringValue": "val1"}},
                {"key": "int_attr", "value": {"intValue": "42"}},
                {"key": "double_attr", "value": {"doubleValue": 3.14}},
                {"key": "bool_attr_t", "value": {"boolValue": true}},
                {"key": "bool_attr_f", "value": {"boolValue": false}},
                {"key": "arr_attr", "value": {"arrayValue": [1, 2]}},
                {"key": "kv_attr", "value": {"kvlistValue": {"k": "v"}}},
                {"key": "bytes_attr", "value": {"bytesValue": "aGVsbG8="}},
                {"key": "raw_str", "value": "direct_str"},
                {"key": "raw_num", "value": 99},
                {"key": "raw_bool", "value": true},
                {"key": "null_attr", "value": {"unknownType": {}}}
              ]
            },
            {
              "traceId": "22223333444455556666777788889999",
              "spanId": "2222333344445555",
              "traceState": "state=link2",
              "flags": 0,
              "attributes": {
                "peer.service": {"stringValue": "payment"},
                "peer.port": {"intValue": 8080}
              }
            },
            {
              "traceId": "33334444555566667777888899990000",
              "spanId": "3333444455556666",
              "traceState": "",
              "flags": 256
            },
            null,
            "invalid_non_object_link",
            12345
          ]
        }]
      }]
    }]
  })";

  auto res = ProcessTraces(L_, json, emit_idx_);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(res.emitted_count, 1);

  const char* verify_script = R"(
    assert(#emitted == 1, "Expected 1 emitted record")
    local r = emitted[1].log
    assert(type(r.Links) == "string", "Links must be a string")
    assert(r.Links:sub(1, 1) == "[", "Links must start with [")
    assert(r.Links:sub(-1, -1) == "]", "Links must end with ]")

    -- Check first link
    assert(r.Links:find('"TraceId":"11112222333344445555666677778888"') ~= nil, "Link 1 TraceId missing")
    assert(r.Links:find('"SpanId":"1111222233334444"') ~= nil, "Link 1 SpanId missing")
    assert(r.Links:find('"TraceState":"rojo=1,congo=2"') ~= nil, "Link 1 TraceState missing")
    assert(r.Links:find('"str_attr":"val1"') ~= nil, "str_attr missing")
    assert(r.Links:find('"int_attr":42') ~= nil, "int_attr missing")
    assert(r.Links:find('"bool_attr_t":true') ~= nil, "bool_attr_t missing")
    assert(r.Links:find('"bool_attr_f":false') ~= nil, "bool_attr_f missing")
    assert(r.Links:find('"raw_str":"direct_str"') ~= nil, "raw_str missing")
    assert(r.Links:find('"raw_num":99') ~= nil, "raw_num missing")
    assert(r.Links:find('"null_attr":null') ~= nil, "null_attr missing")

    -- Check second link with object attributes
    assert(r.Links:find('"TraceId":"22223333444455556666777788889999"') ~= nil, "Link 2 TraceId missing")
    assert(r.Links:find('"peer.service":"payment"') ~= nil, "peer.service missing")
    assert(r.Links:find('"peer.port":8080') ~= nil, "peer.port missing")

    -- Check third link with empty attributes
    assert(r.Links:find('"TraceId":"33334444555566667777888899990000"') ~= nil, "Link 3 TraceId missing")
    assert(r.Links:find('"SpanId":"3333444455556666"') ~= nil, "Link 3 SpanId missing")
    assert(r.Links:find('"Attributes":{}') ~= nil, "Link 3 Attributes:{} missing")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(TracesExtractorTest, SpanEventsParsingComprehensive) {
  const char* json = R"({
    "resourceSpans": [{
      "resource": {},
      "scopeSpans": [{
        "scope": {"name": "event-scope"},
        "spans": [{
          "traceId": "0123456789abcdef0123456789abcdef",
          "spanId": "0123456789abcdef",
          "name": "span-with-events",
          "startTimeUnixNano": "1710000000000000000",
          "endTimeUnixNano": "1710000001000000000",
          "events": [
            {
              "timeUnixNano": "1710000000123456789",
              "name": "cache_lookup",
              "droppedAttributesCount": 3,
              "attributes": [
                {"key": "hit", "value": {"boolValue": true}},
                {"key": "key", "value": {"stringValue": "user:123"}},
                {"key": "latency_ms", "value": {"doubleValue": 1.25}}
              ]
            },
            {
              "timeUnixNano": 1710000000500000000,
              "name": "db_query",
              "attributes": {
                "query": {"stringValue": "SELECT * FROM users"},
                "rows": {"intValue": 1}
              }
            },
            {
              "name": "event_without_time_and_attrs"
            },
            null,
            "non_object_event",
            42
          ]
        }]
      }]
    }]
  })";

  auto res = ProcessTraces(L_, json, emit_idx_);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(res.emitted_count, 1);

  const char* verify_script = R"(
    assert(#emitted == 1, "Expected 1 emitted record")
    local r = emitted[1].log
    assert(type(r.Events) == "string", "Events must be a string")
    assert(r.Events:sub(1, 1) == "[", "Events must start with [")
    assert(r.Events:sub(-1, -1) == "]", "Events must end with ]")

    -- Event 1
    assert(r.Events:find('"Timestamp":"2024%-03%-09T16:00:00.123456789Z"') ~= nil, "Event 1 Timestamp mismatch")
    assert(r.Events:find('"Name":"cache_lookup"') ~= nil, "Event 1 Name mismatch")
    assert(r.Events:find('"hit":true') ~= nil, "Event 1 hit missing")
    assert(r.Events:find('"key":"user:123"') ~= nil, "Event 1 key missing")
    assert(r.Events:find('"latency_ms":1.25') ~= nil, "Event 1 latency_ms missing")

    -- Event 2 (numeric timestamp and object attributes)
    assert(r.Events:find('"Timestamp":"2024%-03%-09T16:00:00.500000000Z"') ~= nil, "Event 2 Timestamp mismatch")
    assert(r.Events:find('"Name":"db_query"') ~= nil, "Event 2 Name mismatch")
    assert(r.Events:find('"query":"SELECT %* FROM users"') ~= nil, "Event 2 query missing")
    assert(r.Events:find('"rows":1') ~= nil, "Event 2 rows missing")

    -- Event 3 (missing time -> 1970 and empty attributes)
    assert(r.Events:find('"Timestamp":"1970%-01%-01T00:00:00.000000000Z"') ~= nil, "Event 3 Timestamp mismatch")
    assert(r.Events:find('"Name":"event_without_time_and_attrs"') ~= nil, "Event 3 Name mismatch")
    assert(r.Events:find('"Attributes":{}') ~= nil, "Event 3 Attributes:{} missing")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(TracesExtractorTest, SpanStatusCodesAndMessagesComprehensive) {
  const char* json = R"({
    "resourceSpans": [{
      "resource": {},
      "scopeSpans": [{
        "spans": [
          {"traceId": "1", "spanId": "1", "status": {"code": 1, "message": "all good"}},
          {"traceId": "2", "spanId": "2", "status": {"code": 2, "message": "failure"}},
          {"traceId": "3", "spanId": "3", "status": {"code": 0, "message": "unset msg"}},
          {"traceId": "4", "spanId": "4", "status": {"code": 99, "message": "unknown num"}},
          {"traceId": "5", "spanId": "5", "status": {"code": "STATUS_CODE_OK", "message": "msg5"}},
          {"traceId": "6", "spanId": "6", "status": {"code": "STATUS_CODE_ERROR", "message": "msg6"}},
          {"traceId": "7", "spanId": "7", "status": {"code": "STATUS_CODE_UNSET", "message": "msg7"}},
          {"traceId": "8", "spanId": "8", "status": {"code": "OK", "message": "msg8"}},
          {"traceId": "9", "spanId": "9", "status": {"code": "ERROR", "message": "msg9"}},
          {"traceId": "10", "spanId": "10", "status": {"code": "UNSET", "message": "msg10"}},
          {"traceId": "11", "spanId": "11", "status": {"code": "CUSTOM_STATUS", "message": "custom"}},
          {"traceId": "12", "spanId": "12", "status": {"message": "only msg"}},
          {"traceId": "13", "spanId": "13"}
        ]
      }]
    }]
  })";

  auto res = ProcessTraces(L_, json, emit_idx_);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(res.emitted_count, 13);

  const char* verify_script = R"(
    assert(#emitted == 13, "Expected 13 spans")
    assert(emitted[1].log.StatusCode == "STATUS_CODE_OK" and emitted[1].log.StatusMessage == "all good")
    assert(emitted[2].log.StatusCode == "STATUS_CODE_ERROR" and emitted[2].log.StatusMessage == "failure")
    assert(emitted[3].log.StatusCode == "STATUS_CODE_UNSET" and emitted[3].log.StatusMessage == "unset msg")
    assert(emitted[4].log.StatusCode == "STATUS_CODE_UNSET" and emitted[4].log.StatusMessage == "unknown num")
    assert(emitted[5].log.StatusCode == "STATUS_CODE_OK" and emitted[5].log.StatusMessage == "msg5")
    assert(emitted[6].log.StatusCode == "STATUS_CODE_ERROR" and emitted[6].log.StatusMessage == "msg6")
    assert(emitted[7].log.StatusCode == "STATUS_CODE_UNSET" and emitted[7].log.StatusMessage == "msg7")
    assert(emitted[8].log.StatusCode == "STATUS_CODE_OK" and emitted[8].log.StatusMessage == "msg8")
    assert(emitted[9].log.StatusCode == "STATUS_CODE_ERROR" and emitted[9].log.StatusMessage == "msg9")
    assert(emitted[10].log.StatusCode == "STATUS_CODE_UNSET" and emitted[10].log.StatusMessage == "msg10")
    assert(emitted[11].log.StatusCode == "CUSTOM_STATUS" and emitted[11].log.StatusMessage == "custom")
    assert(emitted[12].log.StatusCode == "STATUS_CODE_UNSET" and emitted[12].log.StatusMessage == "only msg")
    assert(emitted[13].log.StatusCode == "STATUS_CODE_UNSET" and emitted[13].log.StatusMessage == "")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(TracesExtractorTest, SpansWithParentSpanIdTraceStateFlagsAndSnakeCase) {
  const char* json = R"({
    "resourceSpans": [{
      "resource": {},
      "scopeSpans": [{
        "spans": [
          {
            "traceId": "4bf92f3577b34da6a3ce929d0e0e4736",
            "spanId": "00f067aa0ba902b7",
            "parentSpanId": "5fb397be34d23b0f",
            "traceState": "rojo=1,congo=2",
            "flags": 257,
            "name": "child-camel"
          },
          {
            "trace_id": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            "span_id": "bbbbbbbbbbbbbbbb",
            "parent_span_id": "cccccccccccccccc",
            "trace_state": "vendor=secret",
            "start_time_unix_nano": "1700000000000000000",
            "end_time_unix_nano": "1700000000500000000",
            "name": "child-snake"
          },
          {
            "traceId": "dddddddddddddddddddddddddddddddd",
            "spanId": "eeeeeeeeeeeeeeee",
            "name": "root-span"
          }
        ]
      }]
    }]
  })";

  auto res = ProcessTraces(L_, json, emit_idx_);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(res.emitted_count, 3);

  const char* verify_script = R"(
    assert(#emitted == 3, "Expected 3 spans")

    -- Span 1: camelCase child
    local s1 = emitted[1].log
    assert(s1.TraceId == "4bf92f3577b34da6a3ce929d0e0e4736")
    assert(s1.SpanId == "00f067aa0ba902b7")
    assert(s1.ParentSpanId == "5fb397be34d23b0f")
    assert(s1.TraceState == "rojo=1,congo=2")
    assert(s1.SpanName == "child-camel")

    -- Span 2: snake_case child
    local s2 = emitted[2].log
    assert(s2.TraceId == "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
    assert(s2.SpanId == "bbbbbbbbbbbbbbbb")
    assert(s2.ParentSpanId == "cccccccccccccccc")
    assert(s2.TraceState == "vendor=secret")
    assert(s2.SpanName == "child-snake")
    assert(s2.Duration == 500000000)

    -- Span 3: root span
    local s3 = emitted[3].log
    assert(s3.TraceId == "dddddddddddddddddddddddddddddddd")
    assert(s3.SpanId == "eeeeeeeeeeeeeeee")
    assert(s3.ParentSpanId == "")
    assert(s3.TraceState == "")
    assert(s3.SpanName == "root-span")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(TracesExtractorTest, SpanDurationCalculationEdgeCases) {
  const char* json = R"({
    "resourceSpans": [{
      "resource": {},
      "scopeSpans": [{
        "spans": [
          {"traceId": "1", "spanId": "1", "startTimeUnixNano": "2000000000", "endTimeUnixNano": "1000000000"},
          {"traceId": "2", "spanId": "2", "startTimeUnixNano": "0", "endTimeUnixNano": "1000000000"},
          {"traceId": "3", "spanId": "3", "startTimeUnixNano": "0", "endTimeUnixNano": "0"},
          {"traceId": "4", "spanId": "4", "startTimeUnixNano": "1500000000", "endTimeUnixNano": "1500000000"},
          {"traceId": "5", "spanId": "5", "endTimeUnixNano": "1000000000"},
          {"traceId": "6", "spanId": "6", "startTimeUnixNano": "1000000000"},
          {"traceId": "7", "spanId": "7", "start_time_unix_nano": "3000000000", "end_time_unix_nano": "2000000000"},
          {"traceId": "8", "spanId": "8", "startTimeUnixNano": "1000000000", "endTimeUnixNano": "1000002500"}
        ]
      }]
    }]
  })";

  auto res = ProcessTraces(L_, json, emit_idx_);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(res.emitted_count, 8);

  const char* verify_script = R"(
    assert(#emitted == 8)
    assert(emitted[1].log.Duration == 0, "start > end must yield Duration 0")
    assert(emitted[2].log.Duration == 0, "start == 0 must yield Duration 0")
    assert(emitted[3].log.Duration == 0, "both 0 must yield Duration 0")
    assert(emitted[4].log.Duration == 0, "start == end must yield Duration 0")
    assert(emitted[5].log.Duration == 0, "missing start must yield Duration 0")
    assert(emitted[6].log.Duration == 0, "missing end must yield Duration 0")
    assert(emitted[7].log.Duration == 0, "snake_case start > end must yield Duration 0")
    assert(emitted[8].log.Duration == 2500, "normal delta mismatch: " .. tostring(emitted[8].log.Duration))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);

  ASSERT_EQ(luaL_dostring(L_, "emitted = {}"), LUA_OK);

  const char* table_script = R"(
    return {
      resourceSpans = {
        {
          scopeSpans = {
            {
              spans = {
                { traceId = "t1", spanId = "t1", startTimeUnixNano = 2000, endTimeUnixNano = 1000 },
                { traceId = "t2", spanId = "t2", startTimeUnixNano = 0, endTimeUnixNano = 1000 },
                { traceId = "t3", spanId = "t3", startTimeUnixNano = 1000, endTimeUnixNano = 1000 },
                { traceId = "t4", spanId = "t4", startTimeUnixNano = 1000, endTimeUnixNano = 1500 }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, table_script), LUA_OK);
  int table_idx = lua_gettop(L_);

  auto table_res = ProcessTracesTable(L_, table_idx, emit_idx_);
  EXPECT_TRUE(table_res.success);
  EXPECT_EQ(table_res.emitted_count, 4);
  lua_pop(L_, 1);

  const char* verify_table_script = R"(
    assert(#emitted == 4)
    assert(emitted[1].log.Duration == 0, "Table mode start > end must yield Duration 0")
    assert(emitted[2].log.Duration == 0, "Table mode start == 0 must yield Duration 0")
    assert(emitted[3].log.Duration == 0, "Table mode start == end must yield Duration 0")
    assert(emitted[4].log.Duration == 500, "Table mode normal delta mismatch")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_table_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(TracesExtractorTest, MalformedJsonAndInvalidSchemaFieldHandling) {
  // 1. Root JSON is not an object
  auto r1 = ProcessTraces(L_, "[1, 2, 3]", emit_idx_);
  EXPECT_FALSE(r1.success);
  EXPECT_FALSE(r1.error.empty());
  EXPECT_EQ(r1.emitted_count, 0);

  auto r1b = ProcessTraces(L_, "\"a plain string\"", emit_idx_);
  EXPECT_FALSE(r1b.success);
  EXPECT_FALSE(r1b.error.empty());
  EXPECT_EQ(r1b.emitted_count, 0);

  // 2. resourceSpans is not an array
  auto r2 = ProcessTraces(L_, "{\"resourceSpans\": \"not_an_array\"}", emit_idx_);
  EXPECT_FALSE(r2.success);
  EXPECT_FALSE(r2.error.empty());

  auto r2b = ProcessTraces(L_, "{\"resourceSpans\": 12345}", emit_idx_);
  EXPECT_FALSE(r2b.success);
  EXPECT_FALSE(r2b.error.empty());

  // 3. resourceSpans array contains non-object items (should gracefully skip
  // them)
  const char* r3_json = R"({
    "resourceSpans": [
      null,
      123,
      "text",
      {
        "scopeSpans": [{
          "spans": [{"traceId": "1", "spanId": "1"}]
        }]
      }
    ]
  })";
  auto r3 = ProcessTraces(L_, r3_json, emit_idx_);
  EXPECT_TRUE(r3.success);
  EXPECT_EQ(r3.emitted_count, 1);

  // 4. scopeSpans is not an array
  auto r4 = ProcessTraces(L_, "{\"resourceSpans\": [{\"scopeSpans\": 999}]}", emit_idx_);
  EXPECT_FALSE(r4.success);
  EXPECT_FALSE(r4.error.empty());

  // 5. scopeSpans contains non-object items (should gracefully skip them)
  const char* r5_json = R"({
    "resourceSpans": [{
      "scopeSpans": [
        null,
        "bad_scope",
        42,
        {
          "spans": [{"traceId": "2", "spanId": "2"}]
        }
      ]
    }]
  })";
  auto r5 = ProcessTraces(L_, r5_json, emit_idx_);
  EXPECT_TRUE(r5.success);
  EXPECT_EQ(r5.emitted_count, 1);

  // 6. spans is not an array
  auto r6 = ProcessTraces(L_, "{\"resourceSpans\": [{\"scopeSpans\": [{\"spans\": \"invalid\"}]}]}",
                          emit_idx_);
  EXPECT_FALSE(r6.success);
  EXPECT_FALSE(r6.error.empty());

  // 7. spans contains non-object items (should gracefully skip them)
  const char* r7_json = R"({
    "resourceSpans": [{
      "scopeSpans": [{
        "spans": [
          null,
          "bad_span",
          123,
          {"traceId": "3", "spanId": "3"}
        ]
      }]
    }]
  })";
  auto r7 = ProcessTraces(L_, r7_json, emit_idx_);
  EXPECT_TRUE(r7.success);
  EXPECT_EQ(r7.emitted_count, 1);

  // 8. Broken JSON in the middle of a batch
  auto r8 = ProcessTraces(L_,
                          "{\"resourceSpans\": [{\"scopeSpans\": [{\"spans\": "
                          "[{\"traceId\": \"4\"}, {BROKEN",
                          emit_idx_);
  EXPECT_FALSE(r8.success);

  // 9. Table mode with non-table payload
  lua_pushnumber(L_, 42.5);
  int num_idx = lua_gettop(L_);
  auto r9 = ProcessTracesTable(L_, num_idx, emit_idx_);
  EXPECT_FALSE(r9.success);
  EXPECT_EQ(r9.error, "payload must be a table");
  EXPECT_EQ(r9.emitted_count, 0);
  lua_pop(L_, 1);

  // 10. Lua emit failure during ProcessTraces
  const char* err_script = R"(
    function fail_emit(event)
      error("emit callback error")
    end
  )";
  ASSERT_EQ(luaL_dostring(L_, err_script), LUA_OK);
  lua_getglobal(L_, "fail_emit");
  int fail_emit_idx = lua_gettop(L_);

  const char* single_span_json = R"({
    "resourceSpans": [{
      "scopeSpans": [{
        "spans": [{"traceId": "5", "spanId": "5"}]
      }]
    }]
  })";
  auto r10 = ProcessTraces(L_, single_span_json, fail_emit_idx);
  EXPECT_FALSE(r10.success);
  EXPECT_FALSE(r10.error.empty());
  lua_pop(L_, 1);
}

TEST_F(TracesExtractorTest, AnyValueSnakeCaseAndComplexVariantsInAttributes) {
  const char* json = R"({
    "resourceSpans": [{
      "resource": {
        "attributes": [
          {"key": "service.name", "value": {"string_value": "anyvalue-svc"}},
          {"key": "str_snake", "value": {"string_value": "hello_snake"}},
          {"key": "int_snake", "value": {"int_value": 42}},
          {"key": "double_snake", "value": {"double_value": 2.718}},
          {"key": "bool_t_snake", "value": {"bool_value": true}},
          {"key": "bool_f_snake", "value": {"bool_value": false}},
          {"key": "arr_snake", "value": {"array_value": [10, 20]}},
          {"key": "kv_snake", "value": {"kvlist_value": {"k2": "v2"}}},
          {"key": "bytes_snake", "value": {"bytes_value": "Ynl0ZXM="}}
        ]
      },
      "scopeSpans": [{
        "spans": [{
          "traceId": "abc",
          "spanId": "def",
          "attributes": [
            {"key": "s_str", "value": {"string_value": "span_str"}},
            {"key": "s_int", "value": {"int_value": 100}},
            {"key": "s_double", "value": {"double_value": 1.414}},
            {"key": "s_bool", "value": {"bool_value": true}}
          ]
        }]
      }]
    }]
  })";

  auto res = ProcessTraces(L_, json, emit_idx_);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(res.emitted_count, 1);

  const char* verify_script = R"(
    assert(#emitted == 1)
    local r = emitted[1].log
    assert(r.ServiceName == "anyvalue-svc")
    assert(r.ResourceAttributes.str_snake == "hello_snake")
    assert(r.ResourceAttributes.int_snake == 42)
    assert(r.ResourceAttributes.double_snake == 2.718)
    assert(r.ResourceAttributes.bool_t_snake == true)
    assert(r.ResourceAttributes.bool_f_snake == false)
    assert(r.SpanAttributes.s_str == "span_str")
    assert(r.SpanAttributes.s_int == 100)
    assert(r.SpanAttributes.s_double == 1.414)
    assert(r.SpanAttributes.s_bool == true)
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(TracesExtractorTest, SpanKindVariantsAndTableModeComprehensive) {
  // Test short-form kinds: INTERNAL, SERVER, CLIENT, PRODUCER, CONSUMER,
  // UNSPECIFIED, CUSTOM
  const char* json = R"({
    "resourceSpans": [{
      "resource": {},
      "scopeSpans": [{
        "spans": [
          {"traceId": "1", "spanId": "1", "kind": "INTERNAL"},
          {"traceId": "2", "spanId": "2", "kind": "SERVER"},
          {"traceId": "3", "spanId": "3", "kind": "CLIENT"},
          {"traceId": "4", "spanId": "4", "kind": "PRODUCER"},
          {"traceId": "5", "spanId": "5", "kind": "CONSUMER"},
          {"traceId": "6", "spanId": "6", "kind": "UNSPECIFIED"},
          {"traceId": "7", "spanId": "7", "kind": "CUSTOM_PROXY"}
        ]
      }]
    }]
  })";

  auto res = ProcessTraces(L_, json, emit_idx_);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(res.emitted_count, 7);

  const char* verify_script = R"(
    assert(#emitted == 7)
    assert(emitted[1].log.SpanKind == "SPAN_KIND_INTERNAL")
    assert(emitted[2].log.SpanKind == "SPAN_KIND_SERVER")
    assert(emitted[3].log.SpanKind == "SPAN_KIND_CLIENT")
    assert(emitted[4].log.SpanKind == "SPAN_KIND_PRODUCER")
    assert(emitted[5].log.SpanKind == "SPAN_KIND_CONSUMER")
    assert(emitted[6].log.SpanKind == "SPAN_KIND_UNSPECIFIED")
    assert(emitted[7].log.SpanKind == "CUSTOM_PROXY")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);

  // Table mode comprehensive test with snake_case fields and direct array
  ASSERT_EQ(luaL_dostring(L_, "emitted = {}"), LUA_OK);
  const char* table_script = R"(
    return {
      resource_spans = {
        {
          resource = {
            attributes = {
              { key = "service.name", value = { stringValue = "tbl-svc" } }
            }
          },
          scope_spans = {
            {
              scope = { name = "tbl-scope", version = "2.0" },
              spans = {
                {
                  trace_id = "1111",
                  span_id = "2222",
                  parent_span_id = "3333",
                  trace_state = "vendor=tbl",
                  name = "table-span",
                  kind = "SERVER",
                  status = { code = "OK", message = "table ok" },
                  start_time_unix_nano = 1000000000,
                  end_time_unix_nano = 1000000500,
                  attributes = {
                    { key = "env", value = { stringValue = "stage" } },
                    { key = "port", value = { intValue = 9000 } }
                  },
                  events = {
                    { name = "ev_tbl", timeUnixNano = 1000000100 }
                  },
                  links = {
                    { traceId = "linked_tr", spanId = "linked_sp" }
                  }
                },
                {
                  traceId = "4444",
                  spanId = "5555",
                  kind = 1,
                  status = { code = 2, message = "table err" }
                }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, table_script), LUA_OK);
  int table_idx = lua_gettop(L_);

  auto table_res = ProcessTracesTable(L_, table_idx, emit_idx_);
  EXPECT_TRUE(table_res.success);
  EXPECT_EQ(table_res.emitted_count, 2);
  lua_pop(L_, 1);

  const char* verify_table_script = R"(
    assert(#emitted == 2)
    local s1 = emitted[1].log
    assert(s1.ServiceName == "tbl-svc")
    assert(s1.ScopeName == "tbl-scope")
    assert(s1.ScopeVersion == "2.0")
    assert(s1.TraceId == "1111")
    assert(s1.SpanId == "2222")
    assert(s1.ParentSpanId == "3333")
    assert(s1.TraceState == "vendor=tbl")
    assert(s1.SpanName == "table-span")
    assert(s1.SpanKind == "SPAN_KIND_SERVER")
    assert(s1.StatusCode == "STATUS_CODE_OK")
    assert(s1.StatusMessage == "table ok")
    assert(s1.Duration == 500)
    assert(s1.SpanAttributes.env == "stage")
    assert(s1.SpanAttributes.port == 9000)
    assert(s1.Events:find("ev_tbl") ~= nil)
    assert(s1.Links:find("linked_tr") ~= nil)

    local s2 = emitted[2].log
    assert(s2.TraceId == "4444")
    assert(s2.SpanId == "5555")
    assert(s2.SpanKind == "SPAN_KIND_INTERNAL")
    assert(s2.StatusCode == "STATUS_CODE_ERROR")
    assert(s2.StatusMessage == "table err")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_table_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(TracesExtractorTest, EventsFieldOrderDoesNotChangeOutput) {
  const char* json = R"({
    "resourceSpans": [{
      "resource": {},
      "scopeSpans": [{
        "spans": [
          {
            "traceId": "11112222333344445555666677778888",
            "spanId": "1111222233334444",
            "events": [
              {
                "timeUnixNano": "1000",
                "name": "ev_order_test",
                "attributes": [
                  {"key": "k1", "value": {"stringValue": "v1"}},
                  {"key": "k2", "value": {"intValue": "42"}}
                ]
              }
            ]
          },
          {
            "traceId": "11112222333344445555666677778888",
            "spanId": "2222333344445555",
            "events": [
              {
                "attributes": [
                  {"key": "k1", "value": {"stringValue": "v1"}},
                  {"key": "k2", "value": {"intValue": "42"}}
                ],
                "name": "ev_order_test",
                "timeUnixNano": "1000"
              }
            ]
          }
        ]
      }]
    }]
  })";

  auto res = ProcessTraces(L_, json, emit_idx_);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(res.emitted_count, 2);

  const char* verify_script = R"(
    assert(#emitted == 2, "Expected 2 spans")
    local r_a = emitted[1].log
    local r_b = emitted[2].log
    assert(r_a.Events == r_b.Events,
           "Events mismatch across field orders: " .. tostring(r_a.Events) .. " vs " .. tostring(r_b.Events))
    assert(r_a.Events:find("ev_order_test", 1, true) ~= nil, "Missing event name")
    assert(r_a.Events:find("1970-01-01T00:00:00.000001000Z", 1, true) ~= nil, "Missing formatted timestamp")
    assert(r_a.Events == '[{"Timestamp":"1970-01-01T00:00:00.000001000Z","Name":"ev_order_test","Attributes":{"k1":"v1","k2":42}}]',
           "Unexpected Events JSON: " .. tostring(r_a.Events))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(TracesExtractorTest, LinksFieldOrderDoesNotChangeOutput) {
  const char* json = R"({
    "resourceSpans": [{
      "resource": {},
      "scopeSpans": [{
        "spans": [
          {
            "traceId": "11112222333344445555666677778888",
            "spanId": "1111222233334444",
            "links": [
              {
                "traceId": "4bf92f3577b34da6a3ce929d0e0e4736",
                "spanId": "00f067aa0ba902b7",
                "traceState": "rojo=1",
                "attributes": [
                  {"key": "lk", "value": {"stringValue": "lv"}}
                ]
              }
            ]
          },
          {
            "traceId": "11112222333344445555666677778888",
            "spanId": "2222333344445555",
            "links": [
              {
                "attributes": [
                  {"key": "lk", "value": {"stringValue": "lv"}}
                ],
                "traceState": "rojo=1",
                "spanId": "00f067aa0ba902b7",
                "traceId": "4bf92f3577b34da6a3ce929d0e0e4736"
              }
            ]
          }
        ]
      }]
    }]
  })";

  auto res = ProcessTraces(L_, json, emit_idx_);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(res.emitted_count, 2);

  const char* verify_script = R"(
    assert(#emitted == 2, "Expected 2 spans")
    local r_a = emitted[1].log
    local r_b = emitted[2].log
    assert(r_a.Links == r_b.Links,
           "Links mismatch across field orders: " .. tostring(r_a.Links) .. " vs " .. tostring(r_b.Links))
    assert(r_a.Links == '[{"TraceId":"4bf92f3577b34da6a3ce929d0e0e4736","SpanId":"00f067aa0ba902b7","TraceState":"rojo=1","Attributes":{"lk":"lv"}}]',
           "Unexpected Links JSON: " .. tostring(r_a.Links))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(TracesExtractorTest, EventsMissingFieldsUseDefaults) {
  const char* json = R"({
    "resourceSpans": [{
      "resource": {},
      "scopeSpans": [{
        "spans": [
          {
            "traceId": "11112222333344445555666677778888",
            "spanId": "1111222233334444",
            "events": [
              {"name": "only_name"}
            ],
            "links": [
              {"traceId": "only_trace_id"}
            ]
          }
        ]
      }]
    }]
  })";

  auto res = ProcessTraces(L_, json, emit_idx_);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(res.emitted_count, 1);

  const char* verify_script = R"(
    assert(#emitted == 1, "Expected 1 span")
    local r = emitted[1].log
    assert(r.Events:find('"Attributes":{}', 1, true) ~= nil, "Missing default Attributes in Events: " .. tostring(r.Events))
    assert(r.Events == '[{"Timestamp":"1970-01-01T00:00:00.000000000Z","Name":"only_name","Attributes":{}}]',
           "Unexpected Events default output: " .. tostring(r.Events))
    assert(r.Links:find('"Attributes":{}', 1, true) ~= nil, "Missing default Attributes in Links: " .. tostring(r.Links))
    assert(r.Links == '[{"TraceId":"only_trace_id","SpanId":"","TraceState":"","Attributes":{}}]',
           "Unexpected Links default output: " .. tostring(r.Links))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(TracesExtractorTest, EventsAndLinksMissingFieldsUseDefaults) {
  const char* json = R"({
    "resourceSpans": [{
      "resource": {},
      "scopeSpans": [{
        "spans": [
          {
            "traceId": "11112222333344445555666677778888",
            "spanId": "1111222233334444",
            "events": [
              {"name": "only_name"},
              {}
            ],
            "links": [
              {"traceId": "only_trace_id"},
              {}
            ]
          }
        ]
      }]
    }]
  })";

  auto res = ProcessTraces(L_, json, emit_idx_);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(res.emitted_count, 1);

  const char* verify_script = R"(
    assert(#emitted == 1, "Expected 1 span")
    local r = emitted[1].log
    assert(r.Events == '[{"Timestamp":"1970-01-01T00:00:00.000000000Z","Name":"only_name","Attributes":{}},{"Timestamp":"1970-01-01T00:00:00.000000000Z","Name":"","Attributes":{}}]',
           "Unexpected Events default output: " .. tostring(r.Events))
    assert(r.Links == '[{"TraceId":"only_trace_id","SpanId":"","TraceState":"","Attributes":{}},{"TraceId":"","SpanId":"","TraceState":"","Attributes":{}}]',
           "Unexpected Links default output: " .. tostring(r.Links))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(TracesExtractorTest, TableModeNumericScopeMetadataStillCoerced) {
  const char* table_script = R"(
    return {
      resourceSpans = {
        {
          scopeSpans = {
            {
              scope = { name = 999, version = 888 },
              spans = {
                {
                  traceId = "0123456789abcdef0123456789abcdef",
                  spanId = "0123456789abcdef",
                  name = "numeric-scope-span"
                }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, table_script), LUA_OK);
  int table_idx = lua_gettop(L_);

  auto res = ProcessTracesTable(L_, table_idx, emit_idx_);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(res.emitted_count, 1);
  lua_pop(L_, 1);

  const char* verify_script = R"(
    assert(#emitted == 1, "Expected 1 span")
    local r = emitted[1].log
    assert(r.ScopeName == "999", "Expected ScopeName '999', got: " .. tostring(r.ScopeName))
    assert(r.ScopeVersion == "888", "Expected ScopeVersion '888', got: " .. tostring(r.ScopeVersion))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(TracesExtractorTest, TableModeLongMetadataPreserved) {
  const char* table_script = R"(
    local long_service = string.rep("s", 80)
    local long_scope_name = string.rep("n", 80)
    local long_scope_ver = string.rep("v", 80)
    local long_status_msg = string.rep("m", 80)
    return {
      resourceSpans = {
        {
          resource = {
            attributes = {
              { key = "service.name", value = { stringValue = long_service } }
            }
          },
          scopeSpans = {
            {
              scope = { name = long_scope_name, version = long_scope_ver },
              spans = {
                {
                  traceId = "0123456789abcdef0123456789abcdef",
                  spanId = "0123456789abcdef",
                  name = "long-meta-span",
                  status = { code = "ERROR", message = long_status_msg }
                }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, table_script), LUA_OK);
  int table_idx = lua_gettop(L_);

  auto res = ProcessTracesTable(L_, table_idx, emit_idx_);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(res.emitted_count, 1);
  lua_pop(L_, 1);

  const char* verify_script = R"(
    assert(#emitted == 1, "Expected 1 span")
    local r = emitted[1].log
    assert(r.ServiceName == string.rep("s", 80), "ServiceName mismatch: " .. tostring(r.ServiceName))
    assert(r.ScopeName == string.rep("n", 80), "ScopeName mismatch: " .. tostring(r.ScopeName))
    assert(r.ScopeVersion == string.rep("v", 80), "ScopeVersion mismatch: " .. tostring(r.ScopeVersion))
    assert(r.StatusMessage == string.rep("m", 80), "StatusMessage mismatch: " .. tostring(r.StatusMessage))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(TracesExtractorTest, JsonModeDuplicateServiceNameSurvivesGcInEmit) {
  const char* gc_emit_script = R"(
    gc_emitted = {}
    function gc_emit(event)
      table.insert(gc_emitted, event)
      collectgarbage("collect")
    end
  )";
  ASSERT_EQ(luaL_dostring(L_, gc_emit_script), LUA_OK);
  lua_getglobal(L_, "gc_emit");
  int gc_emit_idx = lua_gettop(L_);

  const std::string long_service(80, 'a');
  const std::string json =
      R"({"resourceSpans":[{"resource":{"attributes":[)"
      R"({"key":"service.name","value":{"stringValue":")" +
      long_service +
      R"("}},{"key":"service.name","value":{"intValue":"42"}})"
      R"(]},"scopeSpans":[{"spans":[)"
      R"({"traceId":"11112222333344445555666677778888","spanId":"1111222233334444","name":"s1"},)"
      R"({"traceId":"11112222333344445555666677778888","spanId":"2222333344445555","name":"s2"})"
      R"(]}]}]})";

  auto res = ProcessTraces(L_, json, gc_emit_idx);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(res.emitted_count, 2);
  lua_pop(L_, 1);  // pop gc_emit

  const char* verify_script = R"(
    assert(#gc_emitted == 2, "Expected 2 spans")
    for i = 1, 2 do
      local r = gc_emitted[i].log
      assert(r.ServiceName == "unknown_service",
             "Expected unknown_service on span " .. i .. ", got: " .. tostring(r.ServiceName))
      assert(r.ResourceAttributes["service.name"] == 42,
             "Expected ResourceAttributes['service.name'] == 42 on span " .. i)
    end
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(TracesExtractorTest, DuplicateNonStringServiceNameDoesNotUseAfterFreeUnderGc) {
  const char* gc_emit_script = R"(
    uaf_emitted = {}
    function uaf_gc_emit(event)
      table.insert(uaf_emitted, event)
      collectgarbage("collect")
    end
  )";
  ASSERT_EQ(luaL_dostring(L_, gc_emit_script), LUA_OK);
  lua_getglobal(L_, "uaf_gc_emit");
  int gc_emit_idx = lua_gettop(L_);

  const std::string long_service(64, 'z');
  const std::string json =
      R"({"resourceSpans":[{"resource":{"attributes":[)"
      R"({"key":"service.name","value":{"stringValue":")" +
      long_service +
      R"("}},{"key":"service.name","value":{"intValue":"42"}})"
      R"(]},"scopeSpans":[{"spans":[)"
      R"({"traceId":"aaaa1111222233334444555566667777","spanId":"1111222233334444","name":"span1"},)"
      R"({"traceId":"aaaa1111222233334444555566667777","spanId":"2222333344445555","name":"span2"},)"
      R"({"traceId":"aaaa1111222233334444555566667777","spanId":"3333444455556666","name":"span3"})"
      R"(]}]}]})";

  auto res = ProcessTraces(L_, json, gc_emit_idx);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(res.emitted_count, 3);
  lua_pop(L_, 1);  // pop uaf_gc_emit

  const char* verify_script = R"(
    assert(#uaf_emitted == 3, "Expected 3 spans")
    for i = 1, 3 do
      local r = uaf_emitted[i].log
      assert(r.ServiceName == "unknown_service",
             "Expected unknown_service on span " .. i .. ", got: " .. tostring(r.ServiceName))
      assert(r.ResourceAttributes["service.name"] == 42,
             "Expected ResourceAttributes['service.name'] == 42 on span " .. i)
    end
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(TracesExtractorTest, JsonModeServiceNameSurvivesCallbackRemovingResourceAttribute) {
  const char* uaf_script = R"(
    uaf_emitted = {}
    function uaf_cb(event)
      table.insert(uaf_emitted, event)
      event.log.ResourceAttributes["service.name"] = nil
      collectgarbage("collect")
      collectgarbage("collect")
      for i = 1, 200 do
        local _ = string.rep("x", 80) .. i
      end
    end
  )";
  ASSERT_EQ(luaL_dostring(L_, uaf_script), LUA_OK) << lua_tostring(L_, -1);
  lua_getglobal(L_, "uaf_cb");
  const int cb_idx = lua_gettop(L_);
  const std::string svc(80, 'Z');

  const std::string json =
      R"({"resourceSpans":[{"resource":{"attributes":[{"key":"service.name","value":{"stringValue":")" +
      svc +
      R"("}}]},"scopeSpans":[{"spans":[)"
      R"({"traceId":"0123456789abcdef0123456789abcdef","spanId":"0123456789abcde1"},)"
      R"({"traceId":"0123456789abcdef0123456789abcdef","spanId":"0123456789abcde2"},)"
      R"({"traceId":"0123456789abcdef0123456789abcdef","spanId":"0123456789abcde3"})"
      R"(]}]}]})";

  auto res = ProcessTraces(L_, json, cb_idx);
  lua_pop(L_, 1);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 3);

  const char* verify_script = R"(
    assert(#uaf_emitted == 3, "expected 3 events, got " .. #uaf_emitted)
    for i = 1, 3 do
      local name = uaf_emitted[i].log.ServiceName
      assert(name == string.rep("Z", 80), "event " .. i .. " ServiceName: " .. tostring(name))
    end
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

}  // namespace
}  // namespace extractors
}  // namespace vector_otel
