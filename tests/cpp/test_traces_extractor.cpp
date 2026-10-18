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
  auto r3 = ProcessTraces(
      L_, "{\"resourceSpans\": [{\"resource\": {}, \"scopeSpans\": []}]}",
      emit_idx_);
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
  EXPECT_EQ(luaL_dostring(L_, verify_json_script), LUA_OK)
      << lua_tostring(L_, -1);

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
  EXPECT_EQ(luaL_dostring(L_, verify_table_script), LUA_OK)
      << lua_tostring(L_, -1);
}

TEST_F(TracesExtractorTest,
       ProcessTracesTablePreservesLuaStackDepthOnNestedWrapperAndError) {
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

}  // namespace
}  // namespace extractors
}  // namespace vector_otel
