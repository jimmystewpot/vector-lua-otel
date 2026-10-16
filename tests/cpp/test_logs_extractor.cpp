#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "vector_otel/extractors/logs_extractor.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace vector_otel {
namespace extractors {
namespace {

class LogsExtractorTest : public ::testing::Test {
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

TEST_F(LogsExtractorTest, GoldenEdgeFixturesAllRecords) {
  // Read bench/.data/edge.ndjson if available, or use the 6 golden edge batches
  std::vector<std::string> lines;
  std::ifstream infile("bench/.data/edge.ndjson");
  if (infile.is_open()) {
    std::string line;
    while (std::getline(infile, line)) {
      if (!line.empty()) {
        lines.push_back(line);
      }
    }
  }

  // If file was not open or had wrong line count, fallback to exact golden edge
  // lines
  if (lines.size() != 6) {
    lines = {
        // Batch 1: AnyValue variants + int64 extremes + structured body
        R"({"resourceLogs":[{"resource":{"attributes":[{"key":"service.name","value":{"stringValue":"edge-svc"}}]},"scopeLogs":[{"scope":{"name":"edge","version":"1"},"logRecords":[{"timeUnixNano":"1293840000123456789","severityNumber":9,"severityText":"INFO","body":{"kvlistValue":{"values":[{"key":"k","value":{"stringValue":"v"}}]}},"attributes":[{"key":"s","value":{"stringValue":"str"}},{"key":"i","value":{"intValue":"9223372036854775807"}},{"key":"neg","value":{"intValue":"-9223372036854775808"}},{"key":"d","value":{"doubleValue":3.14159}},{"key":"b_true","value":{"boolValue":true}},{"key":"b_false","value":{"boolValue":false}},{"key":"arr","value":{"arrayValue":{"values":[{"stringValue":"a"},{"intValue":"2"}]}}},{"key":"map","value":{"kvlistValue":{"values":[{"key":"n","value":{"intValue":"1"}}]}}},{"key":"bytes","value":{"bytesValue":"AAEC"}},{"key":"unicode","value":{"stringValue":"h\u00e9llo \"quoted\" \\ \n tab\t \u2713"}}],"traceId":"5b8efff798038103d269b633813fc60c","spanId":"eee19b7ec3c1b174","flags":1}]}]}]})",
        // Batch 2: missing service.name, no scope, timeUnixNano absent ->
        // observed fallback, empty
        // ids
        R"({"resourceLogs":[{"resource":{"attributes":[]},"scopeLogs":[{"logRecords":[{"observedTimeUnixNano":"1293840000123456794","body":{"stringValue":""}}]}]}]})",
        // Batch 3: timeUnixNano "0" + numeric timestamp
        R"({"resourceLogs":[{"resource":{"attributes":[{"key":"service.name","value":{"stringValue":"ts-svc"}}]},"scopeLogs":[{"scope":{"name":"ts"},"logRecords":[{"timeUnixNano":"0","observedTimeUnixNano":"1293840000123456789","body":{"stringValue":"zero-ts"}},{"timeUnixNano":1293840000123456789,"body":{"stringValue":"numeric-ts"}}]}]}]})",
        // Batch 4: empty resourceLogs
        R"({"resourceLogs":[]})",
        // Batch 5: empty scopeLogs
        R"({"resourceLogs":[{"resource":{},"scopeLogs":[]}]})",
        // Batch 6: 2 resources x 2 scopes x 1 record = 4 events
        R"({"resourceLogs":[{"resource":{"attributes":[{"key":"service.name","value":{"stringValue":"r0"}}]},"scopeLogs":[{"scope":{"name":"s0"},"logRecords":[{"timeUnixNano":"1293840000123456789","body":{"stringValue":"r0s0"}}]},{"scope":{"name":"s1"},"logRecords":[{"timeUnixNano":"1293840000123456790","body":{"stringValue":"r0s1"}}]}]},{"resource":{"attributes":[{"key":"service.name","value":{"stringValue":"r1"}}]},"scopeLogs":[{"scope":{"name":"s0"},"logRecords":[{"timeUnixNano":"1293840000123456799","body":{"stringValue":"r1s0"}}]},{"scope":{"name":"s1"},"logRecords":[{"timeUnixNano":"1293840000123456800","body":{"stringValue":"r1s1"}}]}]}]})",
    };
  }

  ASSERT_EQ(lines.size(), 6u);

  int total_emitted = 0;
  for (const auto& line : lines) {
    auto res = ProcessLogs(L_, line, 1);
    EXPECT_TRUE(res.success) << "Error: " << res.error;
    EXPECT_EQ(res.error, "");
    total_emitted += res.emitted_count;
  }

  // Exactly 8 records expected across the 6 batches: 1 + 1 + 2 + 0 + 0 + 4 = 8
  EXPECT_EQ(total_emitted, 8);

  const char* verify_script = R"(
    assert(#emitted == 8, "Expected 8 emitted events, got " .. #emitted)

    -- Check Record 1 (from Batch 1)
    local r1 = emitted[1].log
    assert(r1 ~= nil, "emitted[1].log must exist")
    assert(r1.Timestamp == "2011-01-01T00:00:00.123456789Z", "r1.Timestamp mismatch: " .. tostring(r1.Timestamp))
    assert(r1.TraceId == "5b8efff798038103d269b633813fc60c", "r1.TraceId mismatch")
    assert(r1.SpanId == "eee19b7ec3c1b174", "r1.SpanId mismatch")
    assert(r1.TraceFlags == 1, "r1.TraceFlags mismatch")
    assert(r1.SeverityText == "INFO", "r1.SeverityText mismatch")
    assert(r1.SeverityNumber == 9, "r1.SeverityNumber mismatch")
    assert(r1.ServiceName == "edge-svc", "r1.ServiceName mismatch: " .. tostring(r1.ServiceName))
    assert(type(r1.Body) == "string", "r1.Body must be string")
    assert(string.find(r1.Body, "kvlistValue") ~= nil, "r1.Body must contain kvlistValue")
    assert(r1.ScopeName == "edge", "r1.ScopeName mismatch")
    assert(r1.ScopeVersion == "1", "r1.ScopeVersion mismatch")
    assert(type(r1.processed_at) == "string" and #r1.processed_at > 20, "r1.processed_at invalid")

    -- Attributes checks
    assert(type(r1.ResourceAttributes) == "table", "ResourceAttributes must be table")
    assert(r1.ResourceAttributes["service.name"] == "edge-svc", "ResourceAttributes.service.name mismatch")
    assert(type(r1.ScopeAttributes) == "table", "ScopeAttributes must be table")
    assert(type(r1.LogAttributes) == "table", "LogAttributes must be table")

    -- Record 1 LogAttributes AnyValue variants
    assert(r1.LogAttributes.s == "str", "LogAttributes.s mismatch")
    assert(r1.LogAttributes.i == 9223372036854775807, "LogAttributes.i int64 max mismatch")
    assert(r1.LogAttributes.neg == -9223372036854775808, "LogAttributes.neg int64 min mismatch")
    assert(math.abs(r1.LogAttributes.d - 3.14159) < 1e-5, "LogAttributes.d mismatch")
    assert(r1.LogAttributes.b_true == true, "LogAttributes.b_true mismatch")
    assert(r1.LogAttributes.b_false == false, "LogAttributes.b_false mismatch")
    assert(type(r1.LogAttributes.arr) == "string" and string.find(r1.LogAttributes.arr, "arrayValue") ~= nil, "LogAttributes.arr mismatch")
    assert(type(r1.LogAttributes.map) == "string" and string.find(r1.LogAttributes.map, "kvlistValue") ~= nil, "LogAttributes.map mismatch")
    assert(type(r1.LogAttributes.bytes) == "string" and string.find(r1.LogAttributes.bytes, "bytesValue") ~= nil, "LogAttributes.bytes mismatch")

    -- Check Record 2 (from Batch 2): missing service.name, fallback timestamp, empty ids
    local r2 = emitted[2].log
    assert(r2 ~= nil, "emitted[2].log must exist")
    assert(r2.ServiceName == "unknown_service", "r2.ServiceName must fallback to unknown_service")
    assert(r2.Timestamp == "2011-01-01T00:00:00.123456794Z", "r2.Timestamp mismatch fallback: " .. tostring(r2.Timestamp))
    assert(r2.TraceId == "", "r2.TraceId must be empty string")
    assert(r2.SpanId == "", "r2.SpanId must be empty string")
    assert(r2.TraceFlags == 0, "r2.TraceFlags must be 0")
    assert(r2.SeverityNumber == 0, "r2.SeverityNumber must be 0")
    assert(r2.SeverityText == "", "r2.SeverityText must be empty string")
    assert(r2.Body == "", "r2.Body must be empty string")
    assert(r2.ScopeName == "", "r2.ScopeName must be empty string")
    assert(r2.ScopeVersion == "", "r2.ScopeVersion must be empty string")

    -- Check Record 3 & 4 (from Batch 3): timeUnixNano "0" fallback and numeric timestamp
    local r3 = emitted[3].log
    assert(r3.ServiceName == "ts-svc", "r3.ServiceName mismatch")
    assert(r3.Timestamp == "2011-01-01T00:00:00.123456789Z", "r3.Timestamp zero fallback mismatch: " .. tostring(r3.Timestamp))
    assert(r3.Body == "zero-ts", "r3.Body mismatch")

    local r4 = emitted[4].log
    assert(r4.ServiceName == "ts-svc", "r4.ServiceName mismatch")
    assert(r4.Timestamp == "2011-01-01T00:00:00.123456789Z", "r4.Timestamp numeric mismatch: " .. tostring(r4.Timestamp))
    assert(r4.Body == "numeric-ts", "r4.Body mismatch")

    -- Check Batch 6 (4 records: r0s0, r0s1, r1s0, r1s1)
    local r5 = emitted[5].log
    assert(r5.ServiceName == "r0" and r5.ScopeName == "s0" and r5.Body == "r0s0")
    local r6 = emitted[6].log
    assert(r6.ServiceName == "r0" and r6.ScopeName == "s1" and r6.Body == "r0s1")
    local r7 = emitted[7].log
    assert(r7.ServiceName == "r1" and r7.ScopeName == "s0" and r7.Body == "r1s0")
    local r8 = emitted[8].log
    assert(r8.ServiceName == "r1" and r8.ScopeName == "s1" and r8.Body == "r1s1")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(LogsExtractorTest, Int64Preservation) {
  std::string json = R"({
    "resourceLogs": [{
      "resource": { "attributes": [{ "key": "service.name", "value": { "stringValue": "test" } }] },
      "scopeLogs": [{
        "logRecords": [{
          "timeUnixNano": "1293840000000000000",
          "attributes": [
            { "key": "max_val", "value": { "intValue": "9223372036854775807" } },
            { "key": "min_val", "value": { "intValue": "-9223372036854775808" } }
          ]
        }]
      }]
    }]
  })";

  auto res = ProcessLogs(L_, json, 1);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(res.emitted_count, 1);

  const char* verify_script = R"(
    local r = emitted[#emitted].log
    assert(r.LogAttributes.max_val == 9223372036854775807, "max_val mismatch")
    assert(r.LogAttributes.min_val == -9223372036854775808, "min_val mismatch")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(LogsExtractorTest, TimestampFallback) {
  // Case 1: timeUnixNano missing, observedTimeUnixNano present
  {
    std::string json = R"({
      "resourceLogs": [{
        "scopeLogs": [{
          "logRecords": [{
            "observedTimeUnixNano": "1293840000123456789"
          }]
        }]
      }]
    })";
    auto res = ProcessLogs(L_, json, 1);
    EXPECT_TRUE(res.success);
    EXPECT_EQ(res.emitted_count, 1);
  }

  // Case 2: timeUnixNano is "0", observedTimeUnixNano present
  {
    std::string json = R"({
      "resourceLogs": [{
        "scopeLogs": [{
          "logRecords": [{
            "timeUnixNano": "0",
            "observedTimeUnixNano": "1293840000123456789"
          }]
        }]
      }]
    })";
    auto res = ProcessLogs(L_, json, 1);
    EXPECT_TRUE(res.success);
    EXPECT_EQ(res.emitted_count, 1);
  }

  // Case 3: timeUnixNano is numeric 0, observedTimeUnixNano present
  {
    std::string json = R"({
      "resourceLogs": [{
        "scopeLogs": [{
          "logRecords": [{
            "timeUnixNano": 0,
            "observedTimeUnixNano": 1293840000123456789
          }]
        }]
      }]
    })";
    auto res = ProcessLogs(L_, json, 1);
    EXPECT_TRUE(res.success);
    EXPECT_EQ(res.emitted_count, 1);
  }

  const char* verify_script = R"(
    assert(#emitted == 3)
    for i = 1, 3 do
      assert(emitted[i].log.Timestamp == "2011-01-01T00:00:00.123456789Z",
             "Failed timestamp check for item " .. i .. ": " .. tostring(emitted[i].log.Timestamp))
    end
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(LogsExtractorTest, MissingServiceNameBecomesUnknownService) {
  std::string json = R"({
    "resourceLogs": [{
      "resource": {
        "attributes": [
          { "key": "other.key", "value": { "stringValue": "val" } }
        ]
      },
      "scopeLogs": [{
        "logRecords": [{
          "timeUnixNano": "1293840000000000000"
        }]
      }]
    }]
  })";

  auto res = ProcessLogs(L_, json, 1);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(res.emitted_count, 1);

  const char* verify_script = R"(
    local r = emitted[#emitted].log
    assert(r.ServiceName == "unknown_service", "ServiceName must be unknown_service, got: " .. tostring(r.ServiceName))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(LogsExtractorTest, UnorderedKeysHandledCleanly) {
  // schemaUrl appearing after logRecords; scope appearing after logRecords;
  // resource appearing after scopeLogs
  std::string json = R"({
    "resourceLogs": [
      {
        "scopeLogs": [
          {
            "logRecords": [
              {
                "flags": 4,
                "body": { "stringValue": "unordered_body" },
                "timeUnixNano": "1293840000123456789"
              }
            ],
            "schemaUrl": "https://opentelemetry.io/scope_schema",
            "scope": {
              "name": "unordered_scope_name",
              "version": "v1.2.3",
              "attributes": [
                { "key": "scope_k", "value": { "stringValue": "scope_v" } }
              ]
            }
          }
        ],
        "schemaUrl": "https://opentelemetry.io/res_schema",
        "resource": {
          "attributes": [
            { "key": "service.name", "value": { "stringValue": "unordered_svc" } },
            { "key": "res_k", "value": { "stringValue": "res_v" } }
          ]
        }
      }
    ]
  })";

  auto res = ProcessLogs(L_, json, 1);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(res.emitted_count, 1);

  const char* verify_script = R"(
    local r = emitted[#emitted].log
    assert(r ~= nil)
    assert(r.ServiceName == "unordered_svc", "ServiceName mismatch: " .. tostring(r.ServiceName))
    assert(r.ResourceSchemaUrl == "https://opentelemetry.io/res_schema", "ResourceSchemaUrl mismatch: " .. tostring(r.ResourceSchemaUrl))
    assert(r.ScopeSchemaUrl == "https://opentelemetry.io/scope_schema", "ScopeSchemaUrl mismatch: " .. tostring(r.ScopeSchemaUrl))
    assert(r.ScopeName == "unordered_scope_name", "ScopeName mismatch: " .. tostring(r.ScopeName))
    assert(r.ScopeVersion == "v1.2.3", "ScopeVersion mismatch: " .. tostring(r.ScopeVersion))
    assert(r.Body == "unordered_body", "Body mismatch: " .. tostring(r.Body))
    assert(r.TraceFlags == 4, "TraceFlags mismatch: " .. tostring(r.TraceFlags))
    assert(r.ResourceAttributes.res_k == "res_v", "ResourceAttributes mismatch")
    assert(r.ScopeAttributes.scope_k == "scope_v", "ScopeAttributes mismatch")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(LogsExtractorTest, OutOfOrderFieldsExtractedCorrectly) {
  const std::string json =
      R"({"resourceLogs": [{"resource": {}, "scopeLogs": [{"scope": {}, "logRecords": [{"attributes": [{"key": "app", "value": {"stringValue": "demo"}}], "body": {"stringValue": "hello"}, "timeUnixNano": "1700000000000000000"}]}]}]})";
  auto res = ProcessLogs(L_, json, 1);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(res.emitted_count, 1);

  const char* verify_script = R"(
    local r = emitted[#emitted].log
    assert(r ~= nil)
    assert(r.Timestamp == "2023-11-14T22:13:20.000000000Z", "Timestamp mismatch: " .. tostring(r.Timestamp))
    assert(r.Body == "hello", "Body mismatch: " .. tostring(r.Body))
    assert(r.LogAttributes.app == "demo", "LogAttributes.app mismatch")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(LogsExtractorTest, MalformedJsonReturnsError) {
  std::string_view bad_json = "{\"resourceLogs\": [ truncated";
  auto res = ProcessLogs(L_, bad_json, 1);
  EXPECT_FALSE(res.success);
  EXPECT_FALSE(res.error.empty());
  EXPECT_EQ(res.emitted_count, 0);
}

TEST_F(LogsExtractorTest, EmptyBatches) {
  // Completely empty JSON object
  {
    auto res = ProcessLogs(L_, "{}", 1);
    EXPECT_TRUE(res.success);
    EXPECT_EQ(res.emitted_count, 0);
  }

  // Empty resourceLogs
  {
    auto res = ProcessLogs(L_, "{\"resourceLogs\": []}", 1);
    EXPECT_TRUE(res.success);
    EXPECT_EQ(res.emitted_count, 0);
  }

  // Resource with empty scopeLogs
  {
    auto res = ProcessLogs(L_, "{\"resourceLogs\": [{\"resource\": {}, \"scopeLogs\": []}]}", 1);
    EXPECT_TRUE(res.success);
    EXPECT_EQ(res.emitted_count, 0);
  }
}

TEST_F(LogsExtractorTest, ProcessLogsTablePreservesLuaStackDepthOnNestedWrapperAndError) {
  // 1. { log = { resourceLogs = { ... } } }
  {
    const char* setup_nested = R"(
      return {
        log = {
          resourceLogs = {
            {
              resource = {
                attributes = {
                  { key = "service.name", value = { stringValue = "nested-svc" } }
                }
              },
              scopeLogs = {
                {
                  logRecords = {
                    { timeUnixNano = "1700000000000000000", body = { stringValue = "hi" } }
                  }
                }
              }
            }
          }
        }
      }
    )";
    ASSERT_EQ(luaL_dostring(L_, setup_nested), LUA_OK);
    int payload_idx = lua_gettop(L_);
    int initial_top = lua_gettop(L_);
    auto res = ProcessLogsTable(L_, payload_idx, 1);
    EXPECT_TRUE(res.success);
    EXPECT_EQ(res.emitted_count, 1);
    EXPECT_EQ(lua_gettop(L_), initial_top);
    lua_pop(L_, 1);
  }

  // 2. { log = { unrelated = 1 } }
  {
    const char* setup_unrelated = R"(
      return {
        log = {
          unrelated = 1
        }
      }
    )";
    ASSERT_EQ(luaL_dostring(L_, setup_unrelated), LUA_OK);
    int payload_idx = lua_gettop(L_);
    int initial_top = lua_gettop(L_);
    auto res = ProcessLogsTable(L_, payload_idx, 1);
    EXPECT_TRUE(res.success);
    EXPECT_EQ(res.emitted_count, 0);
    EXPECT_EQ(lua_gettop(L_), initial_top);
    lua_pop(L_, 1);
  }

  // 3. Emit callback raises a Lua error
  {
    const char* setup_err = R"(
      function err_emit(ev)
        error("boom from emit")
      end
      return {
        log = {
          resourceLogs = {
            {
              scopeLogs = {
                {
                  logRecords = {
                    { timeUnixNano = "1700000000000000000", body = { stringValue = "err" } }
                  }
                }
              }
            }
          }
        }
      }
    )";
    ASSERT_EQ(luaL_dostring(L_, setup_err), LUA_OK);
    int payload_idx = lua_gettop(L_);
    lua_getglobal(L_, "err_emit");
    int err_emit_idx = lua_gettop(L_);
    int initial_top = lua_gettop(L_);
    auto res = ProcessLogsTable(L_, payload_idx, err_emit_idx);
    EXPECT_FALSE(res.success);
    EXPECT_NE(res.error.find("boom from emit"), std::string::npos);
    EXPECT_EQ(lua_gettop(L_), initial_top);
    lua_pop(L_, 2);
  }

  // 4. ProcessLogs extracts ServiceName and ResourceAttributes when "attributes" appears
  // before or after other fields in "resource".
  {
    std::string json_before = R"({
      "resourceLogs": [{
        "resource": {
          "attributes": [
            { "key": "service.name", "value": { "stringValue": "svc-before" } },
            { "key": "env", "value": { "stringValue": "prod" } }
          ],
          "droppedAttributesCount": 2
        },
        "scopeLogs": [{
          "logRecords": [{ "timeUnixNano": "1700000000000000000", "body": { "stringValue": "b1" } }]
        }]
      }, {
        "resource": {
          "droppedAttributesCount": 3,
          "attributes": [
            { "key": "region", "value": { "stringValue": "us-east" } },
            { "key": "service.name", "value": { "stringValue": "svc-after" } }
          ]
        },
        "scopeLogs": [{
          "logRecords": [{ "timeUnixNano": "1700000000000000000", "body": { "stringValue": "b2" } }]
        }]
      }]
    })";
    auto res = ProcessLogs(L_, json_before, 1);
    EXPECT_TRUE(res.success);
    EXPECT_EQ(res.emitted_count, 2);

    const char* verify_script = R"(
      local r_before = emitted[#emitted - 1].log
      local r_after = emitted[#emitted].log
      assert(r_before.ServiceName == "svc-before", "svc-before mismatch: " .. tostring(r_before.ServiceName))
      assert(r_before.ResourceAttributes["service.name"] == "svc-before")
      assert(r_before.ResourceAttributes["env"] == "prod")
      assert(r_after.ServiceName == "svc-after", "svc-after mismatch: " .. tostring(r_after.ServiceName))
      assert(r_after.ResourceAttributes["service.name"] == "svc-after")
      assert(r_after.ResourceAttributes["region"] == "us-east")
    )";
    EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
  }
}

}  // namespace
}  // namespace extractors
}  // namespace vector_otel
