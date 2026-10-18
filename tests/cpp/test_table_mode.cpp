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

TEST_F(TableModeTest,
       PushTableAttributesWithNumericKeysDoesNotCorruptIterator) {
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
      "return { intValue = true }",
      "return { intValue = {} }",
      "return { stringValue = true }",
      "return { bytesValue = {} }",
      "return { arrayValue = 123 }",
      "return { arrayValue = { values = 'not_a_table' } }",
      "return { kvlistValue = false }",
      "return { kvlistValue = { values = { 999, 'bad' } } }",
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

}  // namespace
}  // namespace vector_otel
