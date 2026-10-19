#include <gtest/gtest.h>

#include <string>
#include <vector>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include "vector_otel/core/table_reader.hpp"
#include "vector_otel/extractors/logs_extractor.hpp"
#include "vector_otel/extractors/metrics_extractor.hpp"
#include "vector_otel/extractors/traces_extractor.hpp"

namespace vector_otel {
namespace {

static int MockEmit(lua_State* L) {
  // Validate that an event table was passed
  if (lua_istable(L, 1)) {
    lua_getfield(L, 1, "log");
    if (lua_istable(L, -1)) {
      lua_getfield(L, -1, "ServiceName");
      if (lua_isstring(L, -1)) {
        // service name present
      }
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
  }
  return 0;
}

class TableModeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    L_ = luaL_newstate();
    luaL_openlibs(L_);
    lua_pushcfunction(L_, MockEmit);
    emit_idx_ = lua_gettop(L_);
  }

  void TearDown() override {
    if (L_) {
      lua_close(L_);
      L_ = nullptr;
    }
  }

  extractors::ExtractionResult RunLogsTable(const char* lua_input) {
    int rc = luaL_dostring(L_, R"(
      emitted = {}
      function test_collect_emit(ev)
        emitted[#emitted + 1] = ev
      end
    )");
    EXPECT_EQ(rc, LUA_OK);
    lua_getglobal(L_, "test_collect_emit");
    int emit_idx = lua_gettop(L_);

    rc = luaL_dostring(L_, lua_input);
    EXPECT_EQ(rc, LUA_OK);
    int table_idx = lua_gettop(L_);

    auto res = extractors::ProcessLogsTable(L_, table_idx, emit_idx);
    lua_pop(L_, 2);  // pop table and test_collect_emit
    return res;
  }

  lua_State* L_{nullptr};
  int emit_idx_{0};
};

TEST_F(TableModeTest, ProcessLogsTableSuccess) {
  // Construct Lua table representing resourceLogs
  const char* script = R"(
    return {
      resourceLogs = {
        {
          resource = {
            attributes = {
              { key = "service.name", value = { stringValue = "cart-service" } },
              { key = "env", value = { stringValue = "prod" } }
            }
          },
          scopeLogs = {
            {
              scope = { name = "cart.checkout", version = "2.1.0" },
              logRecords = {
                {
                  timeUnixNano = 1700000000123456789,
                  severityText = "INFO",
                  severityNumber = "SEVERITY_NUMBER_INFO",
                  body = { stringValue = "Item added to cart" },
                  traceId = "0123456789abcdef0123456789abcdef",
                  spanId = "0123456789abcdef",
                  flags = 1,
                  attributes = {
                    { key = "item.id", value = { intValue = 999 } }
                  }
                },
                {
                  timeUnixNano = 1700000000987654321,
                  severityText = "WARN",
                  severityNumber = 13,
                  body = "Cart size limit approaching",
                  flags = 0
                }
              }
            }
          }
        }
      }
    }
  )";

  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
  int table_idx = lua_gettop(L_);

  auto result = extractors::ProcessLogsTable(L_, table_idx, emit_idx_);
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.emitted_count, 2);
  EXPECT_TRUE(result.error.empty());
  lua_pop(L_, 1);
}

TEST_F(TableModeTest, ProcessTracesTableSuccess) {
  const char* script = R"(
    return {
      resourceSpans = {
        {
          resource = {
            attributes = {
              { key = "service.name", value = { stringValue = "order-service" } }
            }
          },
          scopeSpans = {
            {
              scope = { name = "order.tracer" },
              spans = {
                {
                  traceId = "11112222333344445555666677778888",
                  spanId = "aaaabbbbccccdddd",
                  parentSpanId = "0000111122223333",
                  name = "CreateOrder",
                  kind = "SPAN_KIND_SERVER",
                  startTimeUnixNano = 1700000000000000000,
                  endTimeUnixNano = 1700000000050000000,
                  status = { code = "STATUS_CODE_OK", message = "success" },
                  attributes = {
                    { key = "order.total", value = { doubleValue = 129.99 } }
                  }
                }
              }
            }
          }
        }
      }
    }
  )";

  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
  int table_idx = lua_gettop(L_);

  auto result = extractors::ProcessTracesTable(L_, table_idx, emit_idx_);
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.emitted_count, 1);
  EXPECT_TRUE(result.error.empty());
  lua_pop(L_, 1);
}

TEST_F(TableModeTest, ProcessMetricsTableSuccess) {
  const char* script = R"(
    return {
      resourceMetrics = {
        {
          resource = {
            attributes = {
              { key = "service.name", value = { stringValue = "billing-service" } }
            }
          },
          scopeMetrics = {
            {
              scope = { name = "billing.metrics" },
              metrics = {
                {
                  name = "http.server.duration",
                  description = "Server response time",
                  unit = "ms",
                  gauge = {
                    dataPoints = {
                      {
                        timeUnixNano = 1700000000000000000,
                        asDouble = 45.2,
                        attributes = {
                          { key = "http.route", value = { stringValue = "/checkout" } }
                        }
                      }
                    }
                  }
                },
                {
                  name = "http.requests.total",
                  description = "Total request count",
                  unit = "1",
                  sum = {
                    dataPoints = {
                      {
                        timeUnixNano = 1700000000000000000,
                        asInt = 1500
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  )";

  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
  int table_idx = lua_gettop(L_);

  auto result = extractors::ProcessMetricsTable(L_, table_idx, emit_idx_);
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.emitted_count, 2);
  EXPECT_TRUE(result.error.empty());
  lua_pop(L_, 1);
}

TEST_F(TableModeTest, EmptyOrNonTablePayload) {
  lua_pushstring(L_, "not a table");
  int not_table_idx = lua_gettop(L_);
  auto res1 = extractors::ProcessLogsTable(L_, not_table_idx, emit_idx_);
  EXPECT_FALSE(res1.success);
  lua_pop(L_, 1);

  lua_newtable(L_);
  int empty_table_idx = lua_gettop(L_);
  auto res2 = extractors::ProcessLogsTable(L_, empty_table_idx, emit_idx_);
  EXPECT_TRUE(res2.success);
  EXPECT_EQ(res2.emitted_count, 0);
  lua_pop(L_, 1);
}

TEST_F(TableModeTest, SerializeLuaValueToJsonMaxDepthProtection) {
  const char* script = R"(
    local root = {}
    local curr = root
    for i = 1, 100 do
      curr.nested = {}
      curr = curr.nested
    end
    return root
  )";
  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
  core::FastJsonWriter writer;
  core::SerializeLuaValueToJson(L_, -1, writer);
  std::string_view json = writer.Str();
  EXPECT_TRUE(json.find("[max_depth_exceeded]") != std::string_view::npos);
  lua_pop(L_, 1);
}

TEST_F(TableModeTest, SerializeTableWithNumericKeysDoesNotCorruptIterator) {
  lua_newtable(L_);
  lua_pushstring(L_, "alpha");
  lua_rawseti(L_, -2, 1);
  lua_pushstring(L_, "beta");
  lua_rawseti(L_, -2, 42);

  core::FastJsonWriter writer;
  EXPECT_NO_THROW(core::SerializeLuaValueToJson(L_, -1, writer));
  std::string_view json = writer.Str();
  EXPECT_NE(json.find("\"1\":"), std::string_view::npos);
  EXPECT_NE(json.find("\"42\":"), std::string_view::npos);
  lua_pop(L_, 1);
}

TEST_F(TableModeTest, PushTableAttributesWithNumericKeysDoesNotCorruptIterator) {
  lua_newtable(L_);
  lua_pushstring(L_, "alpha");
  lua_rawseti(L_, -2, 1);
  lua_pushstring(L_, "beta");
  lua_rawseti(L_, -2, 42);

  core::FastJsonWriter writer;
  EXPECT_NO_THROW(core::PushTableAttributes(L_, -1, writer));
  int out_idx = lua_gettop(L_);
  ASSERT_TRUE(lua_istable(L_, out_idx));

  lua_getfield(L_, out_idx, "1");
  EXPECT_TRUE(lua_isstring(L_, -1));
  EXPECT_STREQ(lua_tostring(L_, -1), "alpha");
  lua_pop(L_, 1);

  lua_getfield(L_, out_idx, "42");
  EXPECT_TRUE(lua_isstring(L_, -1));
  EXPECT_STREQ(lua_tostring(L_, -1), "beta");
  lua_pop(L_, 1);

  lua_pop(L_, 2);  // pop out_idx and input table
}

TEST_F(TableModeTest, SerializeSequentialArrayPreservedAsArray) {
  lua_newtable(L_);
  lua_pushstring(L_, "first");
  lua_rawseti(L_, -2, 1);
  lua_pushstring(L_, "second");
  lua_rawseti(L_, -2, 2);

  core::FastJsonWriter writer;
  EXPECT_NO_THROW(core::SerializeLuaValueToJson(L_, -1, writer));
  std::string_view json = writer.Str();
  EXPECT_EQ(json, "[\"first\",\"second\"]");
  lua_pop(L_, 1);
}

TEST_F(TableModeTest, MalformedAnyValueTypesDoNotCrashOrPanic) {
  const char* cases[] = {
      "return { intValue = true }",     "return { intValue = {} }",
      "return { stringValue = true }",  "return { bytesValue = {} }",
      "return { arrayValue = 123 }",    "return { arrayValue = { values = 'not_a_table' } }",
      "return { kvlistValue = false }", "return { kvlistValue = { values = { 999, 'bad' } } }",
  };

  core::FastJsonWriter writer;
  for (const char* expr : cases) {
    int top_before = lua_gettop(L_);
    ASSERT_EQ(luaL_dostring(L_, expr), LUA_OK);
    int val_idx = lua_gettop(L_);

    writer.Reset();
    EXPECT_NO_THROW(core::SerializeLuaValueToJson(L_, val_idx, writer));
    EXPECT_FALSE(writer.Str().empty());

    // Also test inside PushTableAttributes (map mode and KV-array mode)
    lua_newtable(L_);
    lua_pushvalue(L_, val_idx);
    lua_setfield(L_, -2, "attr_key");
    EXPECT_NO_THROW(core::PushTableAttributes(L_, -1, writer));
    EXPECT_TRUE(lua_istable(L_, -1));
    lua_pop(L_, 2);  // pop out table and map table

    lua_newtable(L_);
    lua_newtable(L_);
    lua_pushstring(L_, "attr_key");
    lua_setfield(L_, -2, "key");
    lua_pushvalue(L_, val_idx);
    lua_setfield(L_, -2, "value");
    lua_rawseti(L_, -2, 1);
    EXPECT_NO_THROW(core::PushTableAttributes(L_, -1, writer));
    EXPECT_TRUE(lua_istable(L_, -1));
    lua_pop(L_, 2);  // pop out table and kv-array table

    lua_pop(L_, 1);  // pop val_idx
    EXPECT_EQ(lua_gettop(L_), top_before);
  }

  // Also verify PushTableSpanEvents and PushTableSpanLinks with valid and
  // non-string fields
  const char* events_script = R"(
    return {
      { timeUnixNano = 1700000000000000000, name = "ev_ok", attributes = { k = "v" } },
      { timeUnixNano = 1700000000000000000, name = true }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, events_script), LUA_OK);
  core::PushTableSpanEvents(L_, -1, writer);
  ASSERT_TRUE(lua_istable(L_, -1));
  lua_rawgeti(L_, -1, 1);
  lua_getfield(L_, -1, "Name");
  EXPECT_STREQ(lua_tostring(L_, -1), "ev_ok");
  lua_pop(L_, 2);
  lua_rawgeti(L_, -1, 2);
  lua_getfield(L_, -1, "Name");
  EXPECT_STREQ(lua_tostring(L_, -1), "");
  lua_pop(L_, 4);  // pop Name, item 2, events out, events in

  const char* links_script = R"(
    return {
      { traceId = "t1", spanId = "s1", traceState = "ts1" },
      { traceId = {}, spanId = false, traceState = 123 }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, links_script), LUA_OK);
  core::PushTableSpanLinks(L_, -1, writer);
  ASSERT_TRUE(lua_istable(L_, -1));
  lua_rawgeti(L_, -1, 1);
  lua_getfield(L_, -1, "TraceId");
  EXPECT_STREQ(lua_tostring(L_, -1), "t1");
  lua_pop(L_, 2);
  lua_rawgeti(L_, -1, 2);
  lua_getfield(L_, -1, "TraceId");
  EXPECT_STREQ(lua_tostring(L_, -1), "");
  lua_pop(L_, 4);  // pop TraceId, item 2, links out, links in
}

TEST_F(TableModeTest, PushTableAttributesPreservesEmbeddedNullBytesInKeys) {
  core::FastJsonWriter writer;
  const char kKeyWithNull[] = {'a', '\0', 'b'};

  // 1. Map mode with embedded null byte in key
  lua_newtable(L_);
  lua_pushlstring(L_, kKeyWithNull, sizeof(kKeyWithNull));
  lua_pushstring(L_, "map_val");
  lua_rawset(L_, -3);

  core::PushTableAttributes(L_, -1, writer);
  int out_map_idx = lua_gettop(L_);
  ASSERT_TRUE(lua_istable(L_, out_map_idx));

  lua_pushlstring(L_, kKeyWithNull, sizeof(kKeyWithNull));
  lua_rawget(L_, out_map_idx);
  EXPECT_EQ(lua_type(L_, -1), LUA_TSTRING);
  EXPECT_STREQ(lua_tostring(L_, -1), "map_val");
  lua_pop(L_, 3);  // pop field value, out_map_idx, input map

  // 2. KV-array mode with embedded null byte in key
  lua_newtable(L_);
  lua_newtable(L_);
  lua_pushlstring(L_, kKeyWithNull, sizeof(kKeyWithNull));
  lua_setfield(L_, -2, "key");
  lua_pushstring(L_, "kv_val");
  lua_setfield(L_, -2, "value");
  lua_rawseti(L_, -2, 1);

  core::PushTableAttributes(L_, -1, writer);
  int out_kv_idx = lua_gettop(L_);
  ASSERT_TRUE(lua_istable(L_, out_kv_idx));

  lua_pushlstring(L_, kKeyWithNull, sizeof(kKeyWithNull));
  lua_rawget(L_, out_kv_idx);
  EXPECT_EQ(lua_type(L_, -1), LUA_TSTRING);
  EXPECT_STREQ(lua_tostring(L_, -1), "kv_val");
  lua_pop(L_, 3);  // pop field value, out_kv_idx, input kv-array
}

TEST_F(TableModeTest, BytesValueAnyValueBodyAndAttributes) {
  // Test bytesValue in log body and attributes (array-of-KV and map modes)
  const char* script = R"(
    return {
      resourceLogs = {
        {
          resource = {
            attributes = {
              { key = "service.name", value = { stringValue = "bytes-service" } }
            }
          },
          scopeLogs = {
            {
              scope = { name = "bytes.test" },
              logRecords = {
                {
                  timeUnixNano = 1700000000100000000,
                  body = { bytesValue = "SGVsbG8gV29ybGQ=" },
                  attributes = {
                    { key = "raw.data", value = { bytesValue = "cGF5bG9hZA==" } },
                    { key = "bad.bytes", value = { bytesValue = 12345 } }
                  }
                },
                {
                  timeUnixNano = 1700000000200000000,
                  body = { bytes_value = "c25ha2VfYnl0ZXM=" },
                  attributes = {
                    ["map.bytes"] = { bytes_value = "bWFwX2J5dGVz" }
                  }
                },
                {
                  timeUnixNano = 1700000000300000000,
                  body = { bytesValue = 99999 },
                  attributes = {}
                }
              }
            }
          }
        }
      }
    }
  )";

  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
  auto result = extractors::ProcessLogsTable(L_, -1, emit_idx_);
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.emitted_count, 3);
  EXPECT_TRUE(result.error.empty());
  lua_pop(L_, 1);

  // Direct check of ReadTableBodyString with bytesValue and bytes_value
  core::FastJsonWriter writer;
  ASSERT_EQ(luaL_dostring(L_, "return { body = { bytesValue = 'SGVsbG8=' } }"), LUA_OK);
  std::string body1 = core::ReadTableBodyString(L_, -1, writer);
  EXPECT_NE(body1.find("SGVsbG8="), std::string::npos);
  lua_pop(L_, 1);

  ASSERT_EQ(luaL_dostring(L_, "return { body = { bytes_value = 'V29ybGQ=' } }"), LUA_OK);
  std::string body2 = core::ReadTableBodyString(L_, -1, writer);
  EXPECT_NE(body2.find("V29ybGQ="), std::string::npos);
  lua_pop(L_, 1);

  ASSERT_EQ(luaL_dostring(L_, "return { body = { bytesValue = 42 } }"), LUA_OK);
  std::string body3 = core::ReadTableBodyString(L_, -1, writer);
  EXPECT_EQ(body3, "\"\"");
  lua_pop(L_, 1);
}

TEST_F(TableModeTest, BooleanAndDoubleArraysInAttributes) {
  // Test boolean arrays and double arrays via OTLP arrayValue and Lua
  // sequential arrays
  const char* script = R"(
    return {
      { key = "bool_arr", value = { arrayValue = { values = { { boolValue = true }, { boolValue = false } } } } },
      { key = "lua_bools", value = { true, false, true } },
      { key = "double_arr", value = { arrayValue = { values = { { doubleValue = 1.25 }, { doubleValue = 2.5 } } } } },
      { key = "lua_doubles", value = { 10.5, 20.25, 30.125 } }
    }
  )";

  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
  core::FastJsonWriter writer;
  core::PushTableAttributes(L_, -1, writer);
  int out_idx = lua_gettop(L_);
  ASSERT_TRUE(lua_istable(L_, out_idx));

  lua_getfield(L_, out_idx, "bool_arr");
  ASSERT_TRUE(lua_isstring(L_, -1));
  EXPECT_STREQ(lua_tostring(L_, -1), "[true,false]");
  lua_pop(L_, 1);

  lua_getfield(L_, out_idx, "lua_bools");
  ASSERT_TRUE(lua_isstring(L_, -1));
  EXPECT_STREQ(lua_tostring(L_, -1), "[true,false,true]");
  lua_pop(L_, 1);

  lua_getfield(L_, out_idx, "double_arr");
  ASSERT_TRUE(lua_isstring(L_, -1));
  EXPECT_STREQ(lua_tostring(L_, -1), "[1.25,2.5]");
  lua_pop(L_, 1);

  lua_getfield(L_, out_idx, "lua_doubles");
  ASSERT_TRUE(lua_isstring(L_, -1));
  EXPECT_STREQ(lua_tostring(L_, -1), "[10.5,20.25,30.125]");
  lua_pop(L_, 1);

  lua_pop(L_, 2);  // pop out_idx and input table

  // Direct map mode boolean and double arrays
  const char* map_script = R"(
    return {
      bool_map = { false, true },
      double_map = { 0.5, 1.5, 2.5 }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, map_script), LUA_OK);
  core::PushTableAttributes(L_, -1, writer);
  int map_out_idx = lua_gettop(L_);
  ASSERT_TRUE(lua_istable(L_, map_out_idx));

  lua_getfield(L_, map_out_idx, "bool_map");
  ASSERT_TRUE(lua_isstring(L_, -1));
  EXPECT_STREQ(lua_tostring(L_, -1), "[false,true]");
  lua_pop(L_, 1);

  lua_getfield(L_, map_out_idx, "double_map");
  ASSERT_TRUE(lua_isstring(L_, -1));
  EXPECT_STREQ(lua_tostring(L_, -1), "[0.5,1.5,2.5]");
  lua_pop(L_, 1);

  lua_pop(L_, 2);  // pop map_out_idx and map input table
}

TEST_F(TableModeTest, EmptyAttributesResourceLogsAndScopes) {
  // Empty resourceLogs
  ASSERT_EQ(luaL_dostring(L_, "return { resourceLogs = {} }"), LUA_OK);
  auto res_logs1 = extractors::ProcessLogsTable(L_, -1, emit_idx_);
  EXPECT_TRUE(res_logs1.success);
  EXPECT_EQ(res_logs1.emitted_count, 0);
  lua_pop(L_, 1);

  // Empty scopeLogs
  ASSERT_EQ(luaL_dostring(L_,
                          "return { resourceLogs = { { resource = { attributes = {} }, "
                          "scopeLogs = {} } } }"),
            LUA_OK);
  auto res_logs2 = extractors::ProcessLogsTable(L_, -1, emit_idx_);
  EXPECT_TRUE(res_logs2.success);
  EXPECT_EQ(res_logs2.emitted_count, 0);
  lua_pop(L_, 1);

  // Empty logRecords
  ASSERT_EQ(luaL_dostring(L_,
                          "return { resourceLogs = { { scopeLogs = { { scope = { attributes = "
                          "{} }, logRecords = {} } } } } }"),
            LUA_OK);
  auto res_logs3 = extractors::ProcessLogsTable(L_, -1, emit_idx_);
  EXPECT_TRUE(res_logs3.success);
  EXPECT_EQ(res_logs3.emitted_count, 0);
  lua_pop(L_, 1);

  // Log record with empty attributes and missing resource/scope
  ASSERT_EQ(luaL_dostring(L_,
                          "return { resourceLogs = { { scopeLogs = { { logRecords = { { "
                          "body = 'no res/scope', attributes = {} } } } } } } }"),
            LUA_OK);
  auto res_logs4 = extractors::ProcessLogsTable(L_, -1, emit_idx_);
  EXPECT_TRUE(res_logs4.success);
  EXPECT_EQ(res_logs4.emitted_count, 1);
  lua_pop(L_, 1);

  // Traces: empty resourceSpans, empty scopeSpans, empty spans, span with empty
  // attributes
  ASSERT_EQ(luaL_dostring(L_, "return { resourceSpans = {} }"), LUA_OK);
  auto res_traces1 = extractors::ProcessTracesTable(L_, -1, emit_idx_);
  EXPECT_TRUE(res_traces1.success);
  EXPECT_EQ(res_traces1.emitted_count, 0);
  lua_pop(L_, 1);

  ASSERT_EQ(luaL_dostring(L_,
                          "return { resourceSpans = { { resource = { attributes = {} }, "
                          "scopeSpans = {} } } }"),
            LUA_OK);
  auto res_traces2 = extractors::ProcessTracesTable(L_, -1, emit_idx_);
  EXPECT_TRUE(res_traces2.success);
  EXPECT_EQ(res_traces2.emitted_count, 0);
  lua_pop(L_, 1);

  ASSERT_EQ(luaL_dostring(L_,
                          "return { resourceSpans = { { scopeSpans = { { scope = { "
                          "attributes = {} }, spans = {} } } } } }"),
            LUA_OK);
  auto res_traces3 = extractors::ProcessTracesTable(L_, -1, emit_idx_);
  EXPECT_TRUE(res_traces3.success);
  EXPECT_EQ(res_traces3.emitted_count, 0);
  lua_pop(L_, 1);

  ASSERT_EQ(luaL_dostring(L_,
                          "return { resourceSpans = { { scopeSpans = { { spans = { { "
                          "name = 'empty_span', attributes = {} } } } } } } }"),
            LUA_OK);
  auto res_traces4 = extractors::ProcessTracesTable(L_, -1, emit_idx_);
  EXPECT_TRUE(res_traces4.success);
  EXPECT_EQ(res_traces4.emitted_count, 1);
  lua_pop(L_, 1);

  // Metrics: empty resourceMetrics, empty scopeMetrics, empty metrics, empty
  // dataPoints
  ASSERT_EQ(luaL_dostring(L_, "return { resourceMetrics = {} }"), LUA_OK);
  auto res_metrics1 = extractors::ProcessMetricsTable(L_, -1, emit_idx_);
  EXPECT_TRUE(res_metrics1.success);
  EXPECT_EQ(res_metrics1.emitted_count, 0);
  lua_pop(L_, 1);

  ASSERT_EQ(luaL_dostring(L_,
                          "return { resourceMetrics = { { resource = { attributes = {} }, "
                          "scopeMetrics = {} } } }"),
            LUA_OK);
  auto res_metrics2 = extractors::ProcessMetricsTable(L_, -1, emit_idx_);
  EXPECT_TRUE(res_metrics2.success);
  EXPECT_EQ(res_metrics2.emitted_count, 0);
  lua_pop(L_, 1);

  ASSERT_EQ(luaL_dostring(L_,
                          "return { resourceMetrics = { { scopeMetrics = { { scope = { "
                          "attributes = {} }, metrics = {} } } } } }"),
            LUA_OK);
  auto res_metrics3 = extractors::ProcessMetricsTable(L_, -1, emit_idx_);
  EXPECT_TRUE(res_metrics3.success);
  EXPECT_EQ(res_metrics3.emitted_count, 0);
  lua_pop(L_, 1);

  ASSERT_EQ(luaL_dostring(L_,
                          "return { resourceMetrics = { { scopeMetrics = { { metrics = { { "
                          "name = 'empty_m', gauge = { dataPoints = {} } } } } } } } }"),
            LUA_OK);
  auto res_metrics4 = extractors::ProcessMetricsTable(L_, -1, emit_idx_);
  EXPECT_TRUE(res_metrics4.success);
  EXPECT_EQ(res_metrics4.emitted_count, 0);
  lua_pop(L_, 1);

  ASSERT_EQ(luaL_dostring(L_,
                          "return { resourceMetrics = { { scopeMetrics = { { metrics = { { "
                          "name = 'm_with_empty_attrs', gauge = { dataPoints = { { asInt = 1, "
                          "attributes = {} } } } } } } } } } }"),
            LUA_OK);
  auto res_metrics5 = extractors::ProcessMetricsTable(L_, -1, emit_idx_);
  EXPECT_TRUE(res_metrics5.success);
  EXPECT_EQ(res_metrics5.emitted_count, 1);
  lua_pop(L_, 1);
}

TEST_F(TableModeTest, MalformedTableTypesAndNonTableAttributes) {
  // 1. Logs with non-table attributes, non-table array elements
  const char* logs_script = R"(
    return {
      resourceLogs = {
        "not_a_resource_log",
        12345,
        false,
        {
          resource = { attributes = "non_table_attributes" },
          scopeLogs = {
            "not_a_scope_log",
            {
              scope = { attributes = 42 },
              logRecords = {
                "not_a_log_record",
                {
                  body = true,
                  attributes = "bad_attrs",
                  severityNumber = { bad = true }
                },
                {
                  body = 12345,
                  attributes = false
                },
                {
                  body = nil
                }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, logs_script), LUA_OK);
  auto res_logs = extractors::ProcessLogsTable(L_, -1, emit_idx_);
  EXPECT_TRUE(res_logs.success);
  EXPECT_EQ(res_logs.emitted_count, 3);
  lua_pop(L_, 1);

  // 2. Traces with non-table elements and non-table fields
  const char* traces_script = R"(
    return {
      resourceSpans = {
        "not_a_span",
        {
          resource = { attributes = false },
          scopeSpans = {
            123,
            {
              scope = { attributes = "bad_scope_attrs" },
              spans = {
                true,
                {
                  name = "span_with_bad_fields",
                  attributes = 999,
                  status = { code = false, message = 123 },
                  events = "not_events_table",
                  links = 456
                }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, traces_script), LUA_OK);
  auto res_traces = extractors::ProcessTracesTable(L_, -1, emit_idx_);
  EXPECT_TRUE(res_traces.success);
  EXPECT_EQ(res_traces.emitted_count, 1);
  lua_pop(L_, 1);

  // 3. Metrics with non-table elements and malformed metric types
  const char* metrics_script = R"(
    return {
      resourceMetrics = {
        42,
        {
          resource = { attributes = "bad_resource_attrs" },
          scopeMetrics = {
            false,
            {
              scope = { attributes = 123 },
              metrics = {
                "bad_metric",
                { name = "unknown_type_metric", somethingElse = {} },
                { name = "bad_gauge", gauge = "not_a_table" },
                {
                  name = "valid_metric_with_bad_dp",
                  gauge = {
                    dataPoints = {
                      "not_a_dp",
                      { asInt = 10, attributes = "not_table_attrs" }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, metrics_script), LUA_OK);
  auto res_metrics = extractors::ProcessMetricsTable(L_, -1, emit_idx_);
  EXPECT_TRUE(res_metrics.success);
  EXPECT_EQ(res_metrics.emitted_count, 1);
  lua_pop(L_, 1);

  // 4. PushTableAttributes with non-table input and invalid KV entries
  core::FastJsonWriter writer;
  lua_pushstring(L_, "not_a_table");
  core::PushTableAttributes(L_, -1, writer);
  ASSERT_TRUE(lua_istable(L_, -1));
  EXPECT_EQ(luaL_len(L_, -1), 0);
  lua_pop(L_, 2);

  const char* invalid_kv_script = R"(
    return {
      "string_in_kv_array",
      { key = "", value = "empty_key" },
      { key = 123, value = "non_string_key" },
      { key = "missing_value" },
      { key = "k_bad_str", value = { stringValue = 999 } },
      { key = "k_bad_int", value = { intValue = "not_number" } },
      { key = "k_bad_double", value = { doubleValue = "not_double" } },
      { key = "k_bool_truthy", value = { boolValue = "truthy" } },
      { key = "k_bad_bytes", value = { bytesValue = false } },
      { key = "k_bad_arr", value = { arrayValue = { values = "not_table" } } },
      { key = "k_bad_kv", value = { kvlistValue = { values = { "bad_item", { key = false, value = 1 } } } } }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, invalid_kv_script), LUA_OK);
  core::PushTableAttributes(L_, -1, writer);
  ASSERT_TRUE(lua_istable(L_, -1));
  lua_pop(L_, 2);
}

TEST_F(TableModeTest, SpanEventsTableModeCustomAttributesTimestampAndDroppedCount) {
  core::FastJsonWriter writer;

  // Direct test of PushTableSpanEvents
  const char* events_script = R"(
    return {
      {
        timeUnixNano = 1700000000123456789,
        name = "cache_miss",
        attributes = {
          { key = "cache.key", value = { stringValue = "user:42" } },
          { key = "cache.cost_ms", value = { doubleValue = 12.5 } }
        },
        droppedAttributesCount = 3
      },
      {
        time_unix_nano = 1700000000987654321,
        name = "db_hit",
        attributes = {
          table_name = "users",
          rows = 1
        },
        dropped_attributes_count = 0
      },
      {
        name = "no_attrs_event"
      },
      "not_a_table_event"
    }
  )";

  ASSERT_EQ(luaL_dostring(L_, events_script), LUA_OK);
  core::PushTableSpanEvents(L_, -1, writer);
  int out_idx = lua_gettop(L_);
  ASSERT_TRUE(lua_istable(L_, out_idx));
  EXPECT_EQ(luaL_len(L_, out_idx), 3);

  // Check event 1
  lua_rawgeti(L_, out_idx, 1);
  ASSERT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, -1, "Name");
  EXPECT_STREQ(lua_tostring(L_, -1), "cache_miss");
  lua_pop(L_, 1);
  lua_getfield(L_, -1, "Timestamp");
  EXPECT_TRUE(lua_isstring(L_, -1));
  EXPECT_NE(std::string(lua_tostring(L_, -1)).find("2023-"), std::string::npos);
  lua_pop(L_, 1);
  lua_getfield(L_, -1, "Attributes");
  ASSERT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, -1, "cache.key");
  EXPECT_STREQ(lua_tostring(L_, -1), "user:42");
  lua_pop(L_, 1);
  lua_getfield(L_, -1, "cache.cost_ms");
  EXPECT_DOUBLE_EQ(lua_tonumber(L_, -1), 12.5);
  lua_pop(L_, 2);  // pop Attributes, event 1

  // Check event 2
  lua_rawgeti(L_, out_idx, 2);
  ASSERT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, -1, "Name");
  EXPECT_STREQ(lua_tostring(L_, -1), "db_hit");
  lua_pop(L_, 1);
  lua_getfield(L_, -1, "Attributes");
  ASSERT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, -1, "table_name");
  EXPECT_STREQ(lua_tostring(L_, -1), "users");
  lua_pop(L_, 2);  // pop Attributes, event 2

  // Check event 3 (no attributes -> empty table)
  lua_rawgeti(L_, out_idx, 3);
  ASSERT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, -1, "Name");
  EXPECT_STREQ(lua_tostring(L_, -1), "no_attrs_event");
  lua_pop(L_, 1);
  lua_getfield(L_, -1, "Attributes");
  ASSERT_TRUE(lua_istable(L_, -1));
  EXPECT_EQ(luaL_len(L_, -1), 0);
  lua_pop(L_, 2);  // pop Attributes, event 3

  lua_pop(L_, 2);  // pop out_idx and events input table

  // Test through ProcessTracesTable
  const char* traces_script = R"(
    return {
      resourceSpans = {
        {
          resource = {
            attributes = { { key = "service.name", value = { stringValue = "tracer" } } }
          },
          scopeSpans = {
            {
              spans = {
                {
                  traceId = "0123456789abcdef0123456789abcdef",
                  spanId = "0123456789abcdef",
                  name = "span_with_events",
                  events = {
                    {
                      timeUnixNano = 1700000000100000000,
                      name = "annotated_event",
                      attributes = { { key = "status", value = { stringValue = "ok" } } },
                      droppedAttributesCount = 2
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, traces_script), LUA_OK);
  auto result = extractors::ProcessTracesTable(L_, -1, emit_idx_);
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.emitted_count, 1);
  lua_pop(L_, 1);
}

TEST_F(TableModeTest, SpanLinksTableModeAttributesAndTraceState) {
  core::FastJsonWriter writer;

  // Direct test of PushTableSpanLinks
  const char* links_script = R"(
    return {
      {
        traceId = "0123456789abcdef0123456789abcdef",
        spanId = "0123456789abcdef",
        traceState = "congo=t61rcWkgMzE,rojo=00f067aa0ba902b7",
        attributes = {
          { key = "link.type", value = { stringValue = "parent" } },
          { key = "link.priority", value = { intValue = 1 } }
        },
        droppedAttributesCount = 5
      },
      {
        trace_id = "fedcba9876543210fedcba9876543210",
        span_id = "fedcba9876543210",
        trace_state = "rojo=00f067aa0ba902b7",
        attributes = {
          direct_key = "direct_val"
        }
      },
      {
        traceId = "1111",
        spanId = "2222"
      },
      99999
    }
  )";

  ASSERT_EQ(luaL_dostring(L_, links_script), LUA_OK);
  core::PushTableSpanLinks(L_, -1, writer);
  int out_idx = lua_gettop(L_);
  ASSERT_TRUE(lua_istable(L_, out_idx));
  EXPECT_EQ(luaL_len(L_, out_idx), 3);

  // Link 1
  lua_rawgeti(L_, out_idx, 1);
  ASSERT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, -1, "TraceId");
  EXPECT_STREQ(lua_tostring(L_, -1), "0123456789abcdef0123456789abcdef");
  lua_pop(L_, 1);
  lua_getfield(L_, -1, "SpanId");
  EXPECT_STREQ(lua_tostring(L_, -1), "0123456789abcdef");
  lua_pop(L_, 1);
  lua_getfield(L_, -1, "TraceState");
  EXPECT_STREQ(lua_tostring(L_, -1), "congo=t61rcWkgMzE,rojo=00f067aa0ba902b7");
  lua_pop(L_, 1);
  lua_getfield(L_, -1, "Attributes");
  ASSERT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, -1, "link.type");
  EXPECT_STREQ(lua_tostring(L_, -1), "parent");
  lua_pop(L_, 1);
  lua_getfield(L_, -1, "link.priority");
  EXPECT_EQ(lua_tointeger(L_, -1), 1);
  lua_pop(L_, 2);  // pop Attributes, link 1

  // Link 2
  lua_rawgeti(L_, out_idx, 2);
  ASSERT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, -1, "TraceId");
  EXPECT_STREQ(lua_tostring(L_, -1), "fedcba9876543210fedcba9876543210");
  lua_pop(L_, 1);
  lua_getfield(L_, -1, "SpanId");
  EXPECT_STREQ(lua_tostring(L_, -1), "fedcba9876543210");
  lua_pop(L_, 1);
  lua_getfield(L_, -1, "TraceState");
  EXPECT_STREQ(lua_tostring(L_, -1), "rojo=00f067aa0ba902b7");
  lua_pop(L_, 1);
  lua_getfield(L_, -1, "Attributes");
  ASSERT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, -1, "direct_key");
  EXPECT_STREQ(lua_tostring(L_, -1), "direct_val");
  lua_pop(L_, 2);  // pop Attributes, link 2

  // Link 3
  lua_rawgeti(L_, out_idx, 3);
  ASSERT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, -1, "TraceState");
  EXPECT_STREQ(lua_tostring(L_, -1), "");
  lua_pop(L_, 1);
  lua_getfield(L_, -1, "Attributes");
  ASSERT_TRUE(lua_istable(L_, -1));
  EXPECT_EQ(luaL_len(L_, -1), 0);
  lua_pop(L_, 2);  // pop Attributes, link 3

  lua_pop(L_, 2);  // pop out_idx and links input table

  // Test through ProcessTracesTable
  const char* traces_script = R"(
    return {
      resourceSpans = {
        {
          resource = {
            attributes = { { key = "service.name", value = { stringValue = "tracer" } } }
          },
          scopeSpans = {
            {
              spans = {
                {
                  traceId = "11112222333344445555666677778888",
                  spanId = "aaaabbbbccccdddd",
                  name = "span_with_links",
                  links = {
                    {
                      traceId = "00000000000000001111111111111111",
                      spanId = "2222222222222222",
                      traceState = "state=active",
                      attributes = { { key = "kind", value = { stringValue = "follow" } } }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, traces_script), LUA_OK);
  auto result = extractors::ProcessTracesTable(L_, -1, emit_idx_);
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.emitted_count, 1);
  lua_pop(L_, 1);
}

TEST_F(TableModeTest, MetricsHistogramAndSummaryDataPoints) {
  const char* script = R"(
    return {
      resourceMetrics = {
        {
          resource = {
            attributes = { { key = "service.name", value = { stringValue = "metrics-svc" } } }
          },
          scopeMetrics = {
            {
              scope = { name = "telemetry", version = "1.0.0" },
              metrics = {
                {
                  name = "http.server.request.duration",
                  description = "Duration of requests",
                  unit = "ms",
                  histogram = {
                    dataPoints = {
                      {
                        startTimeUnixNano = 1700000000000000000,
                        timeUnixNano = 1700000000500000000,
                        count = 100,
                        sum = 5250.0,
                        min = 1.2,
                        max = 250.0,
                        bucketCounts = { 10, 30, 40, 15, 5 },
                        explicitBounds = { 5.0, 25.0, 50.0, 100.0 },
                        attributes = { { key = "http.status_code", value = { intValue = 200 } } },
                        flags = 0,
                        exemplars = {
                          {
                            timeUnixNano = 1700000000100000000,
                            value = 45.2,
                            traceId = "0123456789abcdef0123456789abcdef",
                            spanId = "0123456789abcdef"
                          }
                        }
                      },
                      {
                        start_time_unix_nano = 1700000000000000000,
                        time_unix_nano = 1700000000600000000,
                        count = 50,
                        sum = 1200.0,
                        bucket_counts = { 10, 40 },
                        explicit_bounds = { 50.0 }
                      }
                    }
                  }
                },
                {
                  name = "rpc.server.duration",
                  description = "RPC duration summary",
                  unit = "ms",
                  summary = {
                    dataPoints = {
                      {
                        startTimeUnixNano = 1700000000000000000,
                        timeUnixNano = 1700000000700000000,
                        count = 42,
                        sum = 840.5,
                        attributes = { { key = "rpc.method", value = { stringValue = "GetUser" } } },
                        flags = 1
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  )";

  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK);
  auto result = extractors::ProcessMetricsTable(L_, -1, emit_idx_);
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.emitted_count, 3);
  EXPECT_TRUE(result.error.empty());
  lua_pop(L_, 1);
}

TEST_F(TableModeTest, TableReaderPrimitivesAndSeverityNumberCoverage) {
  lua_newtable(L_);
  lua_pushinteger(L_, 100);
  lua_setfield(L_, -2, "f_int");
  lua_pushnumber(L_, 200.5);
  lua_setfield(L_, -2, "f_float");
  lua_pushstring(L_, "300");
  lua_setfield(L_, -2, "f_str_num");
  lua_pushstring(L_, "not_num");
  lua_setfield(L_, -2, "f_bad_str");
  lua_pushstring(L_, "hello");
  lua_setfield(L_, -2, "f_str");
  lua_pushboolean(L_, 1);
  lua_setfield(L_, -2, "f_bool");

  int t_idx = lua_gettop(L_);

  EXPECT_EQ(core::ReadTableInt64(L_, t_idx, "f_int"), 100);
  EXPECT_EQ(core::ReadTableInt64(L_, t_idx, "f_float"), 200);
  EXPECT_EQ(core::ReadTableInt64(L_, t_idx, "f_str_num"), 300);
  EXPECT_EQ(core::ReadTableInt64(L_, t_idx, "f_bad_str", nullptr, -1), -1);
  EXPECT_EQ(core::ReadTableInt64(L_, t_idx, "f_missing", nullptr, -42), -42);
  EXPECT_EQ(core::ReadTableInt64(L_, t_idx, "f_missing", "f_int"), 100);

  EXPECT_EQ(core::ReadTableUint64(L_, t_idx, "f_int"), 100ULL);
  EXPECT_EQ(core::ReadTableUint64(L_, t_idx, "f_float"), 200ULL);
  EXPECT_EQ(core::ReadTableUint64(L_, t_idx, "f_str_num"), 300ULL);
  EXPECT_EQ(core::ReadTableUint64(L_, t_idx, "f_bad_str", nullptr, 0ULL), 0ULL);
  EXPECT_EQ(core::ReadTableUint64(L_, t_idx, "f_missing", nullptr, 999ULL), 999ULL);

  EXPECT_DOUBLE_EQ(core::ReadTableDouble(L_, t_idx, "f_float"), 200.5);
  EXPECT_DOUBLE_EQ(core::ReadTableDouble(L_, t_idx, "f_str_num"), 300.0);
  EXPECT_DOUBLE_EQ(core::ReadTableDouble(L_, t_idx, "f_bad_str", nullptr, 0.0), 0.0);
  EXPECT_DOUBLE_EQ(core::ReadTableDouble(L_, t_idx, "f_missing", nullptr, 1.5), 1.5);

  EXPECT_EQ(core::ReadTableString(L_, t_idx, "f_str"), "hello");
  EXPECT_EQ(core::ReadTableString(L_, t_idx, "f_missing", "f_str"), "hello");
  EXPECT_EQ(core::ReadTableString(L_, t_idx, "f_bool", nullptr, "def"), "def");
  EXPECT_EQ(core::ReadTableString(L_, t_idx, "f_missing", nullptr, "def"), "def");

  EXPECT_TRUE(core::GetTableField(L_, t_idx, "f_str"));
  lua_pop(L_, 1);
  EXPECT_TRUE(core::GetTableField(L_, t_idx, "f_missing", "f_str"));
  lua_pop(L_, 1);
  EXPECT_FALSE(core::GetTableField(L_, t_idx, "f_missing", "f_also_missing"));

  lua_pop(L_, 1);  // pop table

  // Severity Number test for all string enum names
  const std::pair<const char*, int64_t> severities[] = {{"SEVERITY_NUMBER_TRACE", 1},
                                                        {"SEVERITY_NUMBER_TRACE2", 2},
                                                        {"SEVERITY_NUMBER_TRACE3", 3},
                                                        {"SEVERITY_NUMBER_TRACE4", 4},
                                                        {"SEVERITY_NUMBER_DEBUG", 5},
                                                        {"SEVERITY_NUMBER_DEBUG2", 6},
                                                        {"SEVERITY_NUMBER_DEBUG3", 7},
                                                        {"SEVERITY_NUMBER_DEBUG4", 8},
                                                        {"SEVERITY_NUMBER_INFO", 9},
                                                        {"SEVERITY_NUMBER_INFO2", 10},
                                                        {"SEVERITY_NUMBER_INFO3", 11},
                                                        {"SEVERITY_NUMBER_INFO4", 12},
                                                        {"SEVERITY_NUMBER_WARN", 13},
                                                        {"SEVERITY_NUMBER_WARN2", 14},
                                                        {"SEVERITY_NUMBER_WARN3", 15},
                                                        {"SEVERITY_NUMBER_WARN4", 16},
                                                        {"SEVERITY_NUMBER_ERROR", 17},
                                                        {"SEVERITY_NUMBER_ERROR2", 18},
                                                        {"SEVERITY_NUMBER_ERROR3", 19},
                                                        {"SEVERITY_NUMBER_ERROR4", 20},
                                                        {"SEVERITY_NUMBER_FATAL", 21},
                                                        {"SEVERITY_NUMBER_FATAL2", 22},
                                                        {"SEVERITY_NUMBER_FATAL3", 23},
                                                        {"SEVERITY_NUMBER_FATAL4", 24},
                                                        {"17", 17},
                                                        {"99", 99},
                                                        {"NOT_A_SEV", 0}};

  for (const auto& [name, expected] : severities) {
    lua_pushstring(L_, name);
    EXPECT_EQ(core::ParseTableSeverityNumber(L_, -1), expected);
    lua_pop(L_, 1);
  }

  lua_pushinteger(L_, 9);
  EXPECT_EQ(core::ParseTableSeverityNumber(L_, -1), 9);
  lua_pop(L_, 1);

  lua_pushnumber(L_, 13.0);
  EXPECT_EQ(core::ParseTableSeverityNumber(L_, -1), 13);
  lua_pop(L_, 1);

  lua_pushboolean(L_, 1);
  EXPECT_EQ(core::ParseTableSeverityNumber(L_, -1), 0);
  lua_pop(L_, 1);

  // Timestamp parsing
  lua_newtable(L_);
  lua_pushinteger(L_, 1700000000123456789ULL);
  lua_setfield(L_, -2, "timeUnixNano");
  EXPECT_EQ(core::ReadTableTimestampNano(L_, -1), 1700000000123456789ULL);
  lua_pop(L_, 1);

  lua_newtable(L_);
  lua_pushinteger(L_, 1700000000987654321ULL);
  lua_setfield(L_, -2, "time_unix_nano");
  EXPECT_EQ(core::ReadTableTimestampNano(L_, -1), 1700000000987654321ULL);
  lua_pop(L_, 1);

  lua_newtable(L_);
  lua_pushinteger(L_, 1700000000111111111ULL);
  lua_setfield(L_, -2, "observedTimeUnixNano");
  EXPECT_EQ(core::ReadTableTimestampNano(L_, -1), 1700000000111111111ULL);
  lua_pop(L_, 1);

  lua_newtable(L_);
  lua_pushinteger(L_, 1700000000222222222ULL);
  lua_setfield(L_, -2, "observed_time_unix_nano");
  EXPECT_EQ(core::ReadTableTimestampNano(L_, -1), 1700000000222222222ULL);
  lua_pop(L_, 1);

  lua_newtable(L_);
  EXPECT_EQ(core::ReadTableTimestampNano(L_, -1), 0ULL);
  lua_pop(L_, 1);
}

TEST_F(TableModeTest, TableReaderServiceNameAndBodyCoverage) {
  // Service name extraction
  lua_newtable(L_);
  EXPECT_EQ(core::ExtractTableServiceName(L_, -1), "unknown_service");
  lua_pop(L_, 1);

  lua_newtable(L_);
  lua_pushstring(L_, "bad_attrs");
  lua_setfield(L_, -2, "attributes");
  EXPECT_EQ(core::ExtractTableServiceName(L_, -1), "unknown_service");
  lua_pop(L_, 1);

  const char* srv1_script = R"(
    return {
      attributes = {
        { key = "other", value = "val" },
        { key = "service.name", value = "srv_str" }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, srv1_script), LUA_OK);
  EXPECT_EQ(core::ExtractTableServiceName(L_, -1), "srv_str");
  lua_pop(L_, 1);

  const char* srv2_script = R"(
    return {
      attributes = {
        { key = "service_name", value = { stringValue = "srv_obj" } }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, srv2_script), LUA_OK);
  EXPECT_EQ(core::ExtractTableServiceName(L_, -1), "srv_obj");
  lua_pop(L_, 1);

  const char* srv3_script = R"(
    return {
      attributes = {
        { key = "service.name", value = { string_value = "srv_snake" } }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, srv3_script), LUA_OK);
  EXPECT_EQ(core::ExtractTableServiceName(L_, -1), "srv_snake");
  lua_pop(L_, 1);

  const char* srv4_script = R"(
    return {
      attributes = {
        "not_a_table",
        { key = "env", value = "prod" }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, srv4_script), LUA_OK);
  EXPECT_EQ(core::ExtractTableServiceName(L_, -1), "unknown_service");
  lua_pop(L_, 1);

  const char* srv5_script = R"(
    return {
      attributes = {
        ["service.name"] = "map_srv"
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, srv5_script), LUA_OK);
  EXPECT_EQ(core::ExtractTableServiceName(L_, -1), "map_srv");
  lua_pop(L_, 1);

  const char* srv6_script = R"(
    return {
      attributes = {
        ["service_name"] = { string_value = "map_snake_srv" }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, srv6_script), LUA_OK);
  EXPECT_EQ(core::ExtractTableServiceName(L_, -1), "map_snake_srv");
  lua_pop(L_, 1);

  const char* srv7_script = R"(
    return {
      attributes = {
        env = "prod"
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, srv7_script), LUA_OK);
  EXPECT_EQ(core::ExtractTableServiceName(L_, -1), "unknown_service");
  lua_pop(L_, 1);

  // ReadTableBodyString coverage
  core::FastJsonWriter writer;

  ASSERT_EQ(luaL_dostring(L_, "return { body = 'plain text' }"), LUA_OK);
  EXPECT_EQ(core::ReadTableBodyString(L_, -1, writer), "plain text");
  lua_pop(L_, 1);

  ASSERT_EQ(luaL_dostring(L_, "return { body = { stringValue = 'otlp text' } }"), LUA_OK);
  EXPECT_EQ(core::ReadTableBodyString(L_, -1, writer), "otlp text");
  lua_pop(L_, 1);

  ASSERT_EQ(luaL_dostring(L_, "return { body = { string_value = 'snake text' } }"), LUA_OK);
  EXPECT_EQ(core::ReadTableBodyString(L_, -1, writer), "snake text");
  lua_pop(L_, 1);

  ASSERT_EQ(luaL_dostring(L_, "return { body = { stringValue = 123 } }"), LUA_OK);
  EXPECT_EQ(core::ReadTableBodyString(L_, -1, writer), "\"\"");
  lua_pop(L_, 1);

  ASSERT_EQ(luaL_dostring(L_, "return { body = { intValue = 123 } }"), LUA_OK);
  EXPECT_EQ(core::ReadTableBodyString(L_, -1, writer), "123");
  lua_pop(L_, 1);

  ASSERT_EQ(luaL_dostring(L_,
                          "return { body = { message = 'json payload', code = "
                          "200 } }"),
            LUA_OK);
  std::string struct_body = core::ReadTableBodyString(L_, -1, writer);
  EXPECT_NE(struct_body.find("\"message\":\"json payload\""), std::string::npos);
  EXPECT_NE(struct_body.find("\"code\":200"), std::string::npos);
  lua_pop(L_, 1);

  ASSERT_EQ(luaL_dostring(L_, "return { body = nil }"), LUA_OK);
  EXPECT_EQ(core::ReadTableBodyString(L_, -1, writer), "");
  lua_pop(L_, 1);

  ASSERT_EQ(luaL_dostring(L_, "return { body = true }"), LUA_OK);
  EXPECT_EQ(core::ReadTableBodyString(L_, -1, writer), "true");
  lua_pop(L_, 1);

  ASSERT_EQ(luaL_dostring(L_, "return { body = 42 }"), LUA_OK);
  EXPECT_EQ(core::ReadTableBodyString(L_, -1, writer), "42");
  lua_pop(L_, 1);

  ASSERT_EQ(luaL_dostring(L_, "return {}"), LUA_OK);
  EXPECT_EQ(core::ReadTableBodyString(L_, -1, writer), "");
  lua_pop(L_, 1);
}

TEST_F(TableModeTest, AlternativeRootKeysAndSpanKindStatusVariations) {
  // Logs with alternative keys
  const char* logs_alt_script = R"(
    return {
      log = {
        resource_logs = {
          {
            schema_url = "https://opentelemetry.io/schemas/1.24.0",
            resource = {
              attributes = { { key = "service_name", value = "alt_service" } }
            },
            scope_logs = {
              {
                scope = { name = "alt.scope", version = "1.0" },
                log_records = {
                  {
                    observed_time_unix_nano = 1700000000555555555,
                    body = "alternative root key log"
                  }
                }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, logs_alt_script), LUA_OK);
  auto res_logs = extractors::ProcessLogsTable(L_, -1, emit_idx_);
  EXPECT_TRUE(res_logs.success);
  EXPECT_EQ(res_logs.emitted_count, 1);
  lua_pop(L_, 1);

  // Traces with alternative keys and various span kinds and statuses
  const char* traces_alt_script = R"(
    return {
      trace = {
        resource_spans = {
          {
            resource = { attributes = { service_name = "alt_trace_service" } },
            scope_spans = {
              {
                spans = {
                  {
                    name = "span_kind_int_1",
                    kind = 1,
                    status = { code = 1, message = "ok status" },
                    startTimeUnixNano = 1700000000000000000,
                    endTimeUnixNano = 1700000000010000000
                  },
                  {
                    name = "span_kind_int_3",
                    kind = 3,
                    status = { code = 2, message = "error status" },
                    startTimeUnixNano = 0,
                    endTimeUnixNano = 1700000000010000000
                  },
                  {
                    name = "span_kind_int_4",
                    kind = 4,
                    status = { code = 0 },
                    startTimeUnixNano = 1700000000020000000,
                    endTimeUnixNano = 1700000000010000000 -- end before start
                  },
                  {
                    name = "span_kind_int_5",
                    kind = 5,
                    status = { code = "OK" }
                  },
                  {
                    name = "span_kind_int_unknown",
                    kind = 99,
                    status = { code = "STATUS_CODE_ERROR" }
                  },
                  {
                    name = "span_kind_str_short",
                    kind = "SERVER",
                    status = { code = "UNSET" }
                  }
                }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, traces_alt_script), LUA_OK);
  auto res_traces = extractors::ProcessTracesTable(L_, -1, emit_idx_);
  EXPECT_TRUE(res_traces.success);
  EXPECT_EQ(res_traces.emitted_count, 6);
  lua_pop(L_, 1);

  // Metrics with alternative keys
  const char* metrics_alt_script = R"(
    return {
      metric = {
        resource_metrics = {
          {
            resource = { attributes = { service_name = "alt_metric_service" } },
            scope_metrics = {
              {
                metrics = {
                  {
                    name = "alt_gauge_int",
                    gauge = {
                      data_points = {
                        { asInt = 50 },
                        { asDouble = 75.5 },
                        { value = 100 },
                        { value = 125.5 }
                      }
                    }
                  },
                  {
                    name = "alt_sum_val",
                    sum = {
                      data_points = {
                        { asDouble = 99.9 }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, metrics_alt_script), LUA_OK);
  auto res_metrics = extractors::ProcessMetricsTable(L_, -1, emit_idx_);
  EXPECT_TRUE(res_metrics.success);
  EXPECT_EQ(res_metrics.emitted_count, 5);
  lua_pop(L_, 1);
}

TEST_F(TableModeTest, NonFiniteAndOutOfRangeNumbersFallBackToDefaults) {
  const char* script = R"(
    return {
      resourceLogs = {
        {
          scopeLogs = {
            {
              logRecords = {
                {
                  timeUnixNano = 1e30,
                  severityNumber = 0/0,
                  flags = math.huge
                },
                {
                  timeUnixNano = -5.0,
                  observedTimeUnixNano = -1,
                  severityNumber = -1e30,
                  flags = -1e30
                },
                {
                  timeUnixNano = -math.huge
                }
              }
            }
          }
        }
      }
    }
  )";

  auto res = RunLogsTable(script);
  ASSERT_TRUE(res.success);
  ASSERT_EQ(res.emitted_count, 3);

  lua_getglobal(L_, "emitted");
  ASSERT_TRUE(lua_istable(L_, -1));
  int emitted_idx = lua_gettop(L_);

  for (int i = 1; i <= 3; ++i) {
    lua_rawgeti(L_, emitted_idx, i);
    ASSERT_TRUE(lua_istable(L_, -1));
    lua_getfield(L_, -1, "log");
    ASSERT_TRUE(lua_istable(L_, -1));

    lua_getfield(L_, -1, "Timestamp");
    EXPECT_STREQ(lua_tostring(L_, -1), "1970-01-01T00:00:00.000000000Z");
    lua_pop(L_, 1);

    lua_getfield(L_, -1, "SeverityNumber");
    EXPECT_EQ(lua_tointeger(L_, -1), 0);
    lua_pop(L_, 1);

    lua_getfield(L_, -1, "TraceFlags");
    EXPECT_EQ(lua_tointeger(L_, -1), 0);
    lua_pop(L_, 1);

    lua_pop(L_, 2);  // pop log and emitted[i]
  }
  lua_pop(L_, 1);  // pop emitted

  // Also directly verify ReadTableUint64 rejects negative integer (-1)
  lua_newtable(L_);
  lua_pushinteger(L_, -1);
  lua_setfield(L_, -2, "flags");
  EXPECT_EQ(core::ReadTableUint64(L_, -1, "flags", nullptr, 0ULL), 0ULL);
  lua_pop(L_, 1);
}

TEST_F(TableModeTest, NumericStringRules) {
  const char* script = R"(
    return {
      resourceLogs = {
        {
          scopeLogs = {
            {
              logRecords = {
                { timeUnixNano = "12abc" },
                { timeUnixNano = "99999999999999999999" },
                { severityNumber = "inf" },
                { severityNumber = "nan" },
                { timeUnixNano = "+5" },
                { timeUnixNano = "  7  " },
                { timeUnixNano = "7\0009" }
              }
            }
          }
        }
      }
    }
  )";

  auto res = RunLogsTable(script);
  ASSERT_TRUE(res.success);
  ASSERT_EQ(res.emitted_count, 7);

  lua_getglobal(L_, "emitted");
  ASSERT_TRUE(lua_istable(L_, -1));
  int emitted_idx = lua_gettop(L_);

  auto get_log_str = [&](int idx, const char* key) -> std::string {
    lua_rawgeti(L_, emitted_idx, idx);
    lua_getfield(L_, -1, "log");
    lua_getfield(L_, -1, key);
    std::string val = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "";
    lua_pop(L_, 3);
    return val;
  };
  auto get_log_int = [&](int idx, const char* key) -> int64_t {
    lua_rawgeti(L_, emitted_idx, idx);
    lua_getfield(L_, -1, "log");
    lua_getfield(L_, -1, key);
    int64_t val = lua_tointeger(L_, -1);
    lua_pop(L_, 3);
    return val;
  };

  EXPECT_EQ(get_log_str(1, "Timestamp"), "1970-01-01T00:00:00.000000000Z");
  EXPECT_EQ(get_log_str(2, "Timestamp"), "1970-01-01T00:00:00.000000000Z");
  EXPECT_EQ(get_log_int(3, "SeverityNumber"), 0);
  EXPECT_EQ(get_log_int(4, "SeverityNumber"), 0);
  EXPECT_EQ(get_log_str(5, "Timestamp"), "1970-01-01T00:00:00.000000005Z");
  EXPECT_EQ(get_log_str(6, "Timestamp"), "1970-01-01T00:00:00.000000007Z");
  EXPECT_EQ(get_log_str(7, "Timestamp"), "1970-01-01T00:00:00.000000000Z");

  lua_pop(L_, 1);  // pop emitted
}

TEST_F(TableModeTest, AttributeIntAndDoubleValueStrictness) {
  const char* script = R"(
    return {
      resourceLogs = {
        {
          scopeLogs = {
            {
              logRecords = {
                {
                  attributes = {
                    { key = "a", value = { intValue = "7x" } },
                    { key = "b", value = { intValue = "7" } },
                    { key = "c", value = { intValue = 1e30 } },
                    { key = "d", value = { doubleValue = "3.14pi" } },
                    { key = "e", value = { doubleValue = " 2.5 " } },
                    {
                      key = "arr",
                      value = {
                        arrayValue = {
                          values = {
                            { intValue = "7x" },
                            { intValue = "7" },
                            { intValue = 1e30 },
                            { doubleValue = "3.14pi" },
                            { doubleValue = " 2.5 " }
                          }
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  )";

  auto res = RunLogsTable(script);
  ASSERT_TRUE(res.success);
  ASSERT_EQ(res.emitted_count, 1);

  lua_getglobal(L_, "emitted");
  lua_rawgeti(L_, -1, 1);
  lua_getfield(L_, -1, "log");
  lua_getfield(L_, -1, "LogAttributes");
  ASSERT_TRUE(lua_istable(L_, -1));
  int attrs_idx = lua_gettop(L_);

  lua_getfield(L_, attrs_idx, "a");
  EXPECT_EQ(lua_tointeger(L_, -1), 0);
  lua_pop(L_, 1);

  lua_getfield(L_, attrs_idx, "b");
  EXPECT_EQ(lua_tointeger(L_, -1), 7);
  lua_pop(L_, 1);

  lua_getfield(L_, attrs_idx, "c");
  EXPECT_EQ(lua_tointeger(L_, -1), 0);
  lua_pop(L_, 1);

  lua_getfield(L_, attrs_idx, "d");
  EXPECT_DOUBLE_EQ(lua_tonumber(L_, -1), 0.0);
  lua_pop(L_, 1);

  lua_getfield(L_, attrs_idx, "e");
  EXPECT_DOUBLE_EQ(lua_tonumber(L_, -1), 2.5);
  lua_pop(L_, 1);

  lua_getfield(L_, attrs_idx, "arr");
  EXPECT_STREQ(lua_tostring(L_, -1), "[0,7,0,0,2.5]");
  lua_pop(L_, 1);

  lua_pop(L_, 4);  // pop LogAttributes, log, emitted[1], emitted
}

TEST_F(TableModeTest, SeverityEnumStringWithEmbeddedNulIsNotMatched) {
  const char* script = R"(
    return {
      resourceLogs = {
        {
          scopeLogs = {
            {
              logRecords = {
                {
                  severityNumber = "SEVERITY_NUMBER_INFO\0x"
                }
              }
            }
          }
        }
      }
    }
  )";

  auto res = RunLogsTable(script);
  ASSERT_TRUE(res.success);
  ASSERT_EQ(res.emitted_count, 1);

  lua_getglobal(L_, "emitted");
  lua_rawgeti(L_, -1, 1);
  lua_getfield(L_, -1, "log");
  lua_getfield(L_, -1, "SeverityNumber");
  EXPECT_EQ(lua_tointeger(L_, -1), 0);
  lua_pop(L_, 4);
}

TEST_F(TableModeTest, ReadTableStringViewReturnsZeroCopyView) {
  ASSERT_EQ(luaL_dostring(L_, "return { name = 'zero_copy_payload' }"), LUA_OK);
  int top_before = lua_gettop(L_);

  lua_getfield(L_, -1, "name");
  const char* expected_ptr = lua_tolstring(L_, -1, nullptr);
  lua_pop(L_, 1);

  std::string scratch;
  std::string_view sv = core::ReadTableStringView(L_, -1, "name", nullptr, scratch, "default");
  EXPECT_EQ(sv, "zero_copy_payload");
  EXPECT_EQ(sv.data(), expected_ptr);
  EXPECT_TRUE(scratch.empty());
  EXPECT_EQ(lua_gettop(L_), top_before);

  lua_pop(L_, 1);
}

TEST_F(TableModeTest, ReadTableStringViewCoercesNumbersViaScratch) {
  ASSERT_EQ(luaL_dostring(L_, "return { name = 999 }"), LUA_OK);
  int top_before = lua_gettop(L_);

  std::string scratch;
  std::string_view sv = core::ReadTableStringView(L_, -1, "name", nullptr, scratch, "default");
  EXPECT_EQ(sv, "999");
  EXPECT_EQ(sv.data(), scratch.data());
  EXPECT_EQ(lua_gettop(L_), top_before);

  // Verify the table field itself was not mutated into a string
  lua_pushstring(L_, "name");
  lua_rawget(L_, -2);
  EXPECT_EQ(lua_type(L_, -1), LUA_TNUMBER);
  EXPECT_EQ(lua_tointeger(L_, -1), 999);
  lua_pop(L_, 2);
}

TEST_F(TableModeTest, ReadTableStringViewFallsBackToSecondName) {
  ASSERT_EQ(luaL_dostring(L_, "return { snake_key = 'fallback_val' }"), LUA_OK);
  std::string scratch;
  std::string_view sv =
      core::ReadTableStringView(L_, -1, "camelKey", "snake_key", scratch, "default_val");
  EXPECT_EQ(sv, "fallback_val");

  std::string_view missing_sv =
      core::ReadTableStringView(L_, -1, "missing1", "missing2", scratch, "default_val");
  EXPECT_EQ(missing_sv, "default_val");
  lua_pop(L_, 1);
}

TEST_F(TableModeTest, ReadTableStringViewIgnoresMetamethods) {
  ASSERT_EQ(luaL_dostring(L_, "return setmetatable({}, {__index = function() error('boom') end})"),
            LUA_OK);
  int top_before = lua_gettop(L_);

  std::string scratch;
  std::string_view sv =
      core::ReadTableStringView(L_, -1, "any_key", "other_key", scratch, "safe_default");
  EXPECT_EQ(sv, "safe_default");
  EXPECT_EQ(lua_gettop(L_), top_before);
  lua_pop(L_, 1);
}

TEST_F(TableModeTest, ReadTableStringViewSurvivesGcWhileTableAnchored) {
  const std::string long_str(80, 'x');
  lua_newtable(L_);
  lua_pushlstring(L_, long_str.data(), long_str.size());
  lua_setfield(L_, -2, "long_field");

  std::string scratch;
  std::string_view sv = core::ReadTableStringView(L_, -1, "long_field", nullptr, scratch);
  lua_gc(L_, LUA_GCCOLLECT, 0);
  EXPECT_EQ(sv, long_str);
  lua_pop(L_, 1);
}

TEST_F(TableModeTest, ExtractTableServiceNameViewMatchesStringVariant) {
  const char* cases[] = {
      // Array of KV with stringValue wrapper
      R"(return { attributes = { { key = "service.name", value = { stringValue = "svc-kv-wrap" } } } })",
      // Array of KV with string_value wrapper and service_name key
      R"(return { attributes = { { key = "service_name", value = { string_value = "svc-kv-snake" } } } })",
      // Array of KV with direct string and first-wins semantics
      R"(return { attributes = {
        { key = "env", value = "prod" },
        { key = "service.name", value = "first-svc" },
        { key = "service.name", value = "second-svc" }
      } })",
      // Direct map with direct string
      R"(return { attributes = { ["service.name"] = "svc-map-direct" } })",
      // Direct map with service_name and stringValue wrapper
      R"(return { attributes = { service_name = { stringValue = "svc-map-wrap" } } })",
      // Missing attributes or missing service.name
      R"(return {})",
      R"(return { attributes = "not_a_table" })",
      R"(return { attributes = { { key = "other", value = "val" } } })",
      // Numeric cases
      R"(return { attributes = { { key = "service.name", value = { stringValue = 12345 } } } })",
      R"(return { attributes = { { key = "service.name", value = 12345 } } })",
      R"(return { attributes = { ["service.name"] = { string_value = 67890 } } })",
      R"(return { attributes = { ["service.name"] = 67890 } })",
  };

  for (const char* expr : cases) {
    ASSERT_EQ(luaL_dostring(L_, expr), LUA_OK) << "Failed on: " << expr;
    int top_before = lua_gettop(L_);

    std::string scratch;
    std::string_view view_res = core::ExtractTableServiceNameView(L_, -1, scratch);
    std::string str_res = core::ExtractTableServiceName(L_, -1);

    EXPECT_EQ(view_res, str_res) << "Mismatch on case: " << expr;
    EXPECT_EQ(lua_gettop(L_), top_before);
    lua_pop(L_, 1);
  }
}

}  // namespace
}  // namespace vector_otel
