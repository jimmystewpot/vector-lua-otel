#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "simdjson.h"
#include "vector_otel/core/fast_json_writer.hpp"
#include "vector_otel/core/otlp_common.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace vector_otel::core {
namespace {

class OtlpCommonTest : public ::testing::Test {
 protected:
  void SetUp() override {
    L_ = luaL_newstate();
    ASSERT_NE(L_, nullptr);
    luaL_openlibs(L_);
  }

  void TearDown() override {
    if (L_ != nullptr) {
      lua_close(L_);
      L_ = nullptr;
    }
  }

  lua_State* L_{nullptr};
  FastJsonWriter writer_;
  simdjson::ondemand::parser parser_;
};

TEST_F(OtlpCommonTest, ParseInt64FromNumber) {
  auto json = R"({
    "pos": 42,
    "neg": -9999,
    "zero": 0,
    "max": 9223372036854775807,
    "min": -9223372036854775808
  })"_padded;

  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();

  auto v_pos = obj["pos"].value();
  EXPECT_EQ(ParseInt64(v_pos), 42);

  auto v_neg = obj["neg"].value();
  EXPECT_EQ(ParseInt64(v_neg), -9999);

  auto v_zero = obj["zero"].value();
  EXPECT_EQ(ParseInt64(v_zero), 0);

  auto v_max = obj["max"].value();
  EXPECT_EQ(ParseInt64(v_max), INT64_MAX);

  auto v_min = obj["min"].value();
  EXPECT_EQ(ParseInt64(v_min), INT64_MIN);
}

TEST_F(OtlpCommonTest, ParseInt64FromString) {
  auto json = R"({
    "pos": "42",
    "neg": "-9999",
    "zero": "0",
    "max": "9223372036854775807",
    "min": "-9223372036854775808",
    "padded": "  12345  ",
    "newlines": "\r\n  12345 \n\t\r",
    "invalid": "not_a_number"
  })"_padded;

  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();

  auto v_pos = obj["pos"].value();
  EXPECT_EQ(ParseInt64(v_pos), 42);

  auto v_neg = obj["neg"].value();
  EXPECT_EQ(ParseInt64(v_neg), -9999);

  auto v_zero = obj["zero"].value();
  EXPECT_EQ(ParseInt64(v_zero), 0);

  auto v_max = obj["max"].value();
  EXPECT_EQ(ParseInt64(v_max), INT64_MAX);

  auto v_min = obj["min"].value();
  EXPECT_EQ(ParseInt64(v_min), INT64_MIN);

  auto v_padded = obj["padded"].value();
  EXPECT_EQ(ParseInt64(v_padded), 12345);

  auto v_newlines = obj["newlines"].value();
  EXPECT_EQ(ParseInt64(v_newlines), 12345);

  auto v_invalid = obj["invalid"].value();
  EXPECT_EQ(ParseInt64(v_invalid), 0);
}

TEST_F(OtlpCommonTest, ParseUint64) {
  auto json = R"({
    "num": 18446744073709551615,
    "str": "18446744073709551615",
    "newlines": "\r\n  18446744073709551615 \n\t\r",
    "zero": 0,
    "str_zero": "0"
  })"_padded;

  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();

  auto v_num = obj["num"].value();
  EXPECT_EQ(ParseUint64(v_num), UINT64_MAX);

  auto v_str = obj["str"].value();
  EXPECT_EQ(ParseUint64(v_str), UINT64_MAX);

  auto v_newlines = obj["newlines"].value();
  EXPECT_EQ(ParseUint64(v_newlines), UINT64_MAX);

  auto v_zero = obj["zero"].value();
  EXPECT_EQ(ParseUint64(v_zero), 0ULL);

  auto v_str_zero = obj["str_zero"].value();
  EXPECT_EQ(ParseUint64(v_str_zero), 0ULL);
}

TEST_F(OtlpCommonTest, ExtractStringField) {
  auto json = R"({
    "name": "vector-service",
    "empty": "",
    "number": 123
  })"_padded;

  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();

  EXPECT_EQ(ExtractStringField(obj, "name"), "vector-service");
  EXPECT_EQ(ExtractStringField(obj, "empty"), "");
  EXPECT_EQ(ExtractStringField(obj, "missing", "default_val"), "default_val");
  EXPECT_EQ(ExtractStringField(obj, "number", "default_val"), "default_val");
}

TEST_F(OtlpCommonTest, ExtractServiceNamePresent) {
  auto json = R"({
    "attributes": [
      {
        "key": "service.name",
        "value": {
          "stringValue": "my-payment-service"
        }
      },
      {
        "key": "env",
        "value": {
          "stringValue": "prod"
        }
      }
    ]
  })"_padded;

  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();

  EXPECT_EQ(ExtractServiceName(obj), "my-payment-service");
}

TEST_F(OtlpCommonTest, ExtractServiceNameSnakeCase) {
  auto json = R"({
    "attributes": [
      {
        "key": "service.name",
        "value": {
          "string_value": "snake-case-service"
        }
      }
    ]
  })"_padded;

  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();

  EXPECT_EQ(ExtractServiceName(obj), "snake-case-service");
}

TEST_F(OtlpCommonTest, ExtractServiceNameUnorderedFields) {
  auto json = R"({
    "attributes": [
      {
        "value": {
          "stringValue": "unordered-service"
        },
        "key": "service.name"
      }
    ]
  })"_padded;

  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();

  EXPECT_EQ(ExtractServiceName(obj), "unordered-service");
}

TEST_F(OtlpCommonTest, ExtractServiceNameMissing) {
  auto json_no_svc = R"({
    "attributes": [
      {
        "key": "host.name",
        "value": {
          "stringValue": "server-1"
        }
      }
    ]
  })"_padded;

  auto doc1 = parser_.iterate(json_no_svc);
  auto obj1 = doc1.get_object().value();
  EXPECT_EQ(ExtractServiceName(obj1), "unknown_service");

  auto json_empty_attrs = R"({ "attributes": [] })"_padded;
  auto doc2 = parser_.iterate(json_empty_attrs);
  auto obj2 = doc2.get_object().value();
  EXPECT_EQ(ExtractServiceName(obj2), "unknown_service");

  auto json_no_attrs = R"({})"_padded;
  auto doc3 = parser_.iterate(json_no_attrs);
  auto obj3 = doc3.get_object().value();
  EXPECT_EQ(ExtractServiceName(obj3), "unknown_service");
}

TEST_F(OtlpCommonTest, ExtractServiceNameMapFallbackDirectString) {
  auto json = R"({
    "attributes": {
      "service.name": "map-direct-service",
      "env": "prod"
    }
  })"_padded;

  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();
  EXPECT_EQ(ExtractServiceName(obj), "map-direct-service");
}

TEST_F(OtlpCommonTest, ExtractServiceNameMapFallbackNestedObject) {
  auto json = R"({
    "attributes": {
      "service.name": {
        "stringValue": "map-nested-service"
      }
    }
  })"_padded;

  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();
  EXPECT_EQ(ExtractServiceName(obj), "map-nested-service");
}

TEST_F(OtlpCommonTest, ExtractServiceNameMapFallbackSnakeCase) {
  auto json = R"({
    "attributes": {
      "service.name": {
        "string_value": "map-snake-service"
      }
    }
  })"_padded;

  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();
  EXPECT_EQ(ExtractServiceName(obj), "map-snake-service");
}

TEST_F(OtlpCommonTest, PushAnyValuePrimitives) {
  auto json = R"({
    "s": { "stringValue": "sample text" },
    "empty_s": { "stringValue": "" },
    "b_true": { "boolValue": true },
    "b_false": { "boolValue": false },
    "i_num": { "intValue": 12345 },
    "i_zero": { "intValue": 0 },
    "i_max": { "intValue": "9223372036854775807" },
    "i_min": { "intValue": "-9223372036854775808" },
    "d": { "doubleValue": 2.71828 }
  })"_padded;

  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();

  // Test string
  {
    auto val = obj["s"].value();
    int top = lua_gettop(L_);
    PushAnyValue(L_, val, writer_);
    EXPECT_EQ(lua_gettop(L_), top + 1);
    EXPECT_TRUE(lua_isstring(L_, -1));
    EXPECT_STREQ(lua_tostring(L_, -1), "sample text");
    lua_pop(L_, 1);
  }

  // Test empty string
  {
    auto val = obj["empty_s"].value();
    int top = lua_gettop(L_);
    PushAnyValue(L_, val, writer_);
    EXPECT_EQ(lua_gettop(L_), top + 1);
    EXPECT_TRUE(lua_isstring(L_, -1));
    EXPECT_STREQ(lua_tostring(L_, -1), "");
    lua_pop(L_, 1);
  }

  // Test bool true
  {
    auto val = obj["b_true"].value();
    int top = lua_gettop(L_);
    PushAnyValue(L_, val, writer_);
    EXPECT_EQ(lua_gettop(L_), top + 1);
    EXPECT_TRUE(lua_isboolean(L_, -1));
    EXPECT_EQ(lua_toboolean(L_, -1), 1);
    lua_pop(L_, 1);
  }

  // Test bool false
  {
    auto val = obj["b_false"].value();
    int top = lua_gettop(L_);
    PushAnyValue(L_, val, writer_);
    EXPECT_EQ(lua_gettop(L_), top + 1);
    EXPECT_TRUE(lua_isboolean(L_, -1));
    EXPECT_EQ(lua_toboolean(L_, -1), 0);
    lua_pop(L_, 1);
  }

  // Test int num
  {
    auto val = obj["i_num"].value();
    int top = lua_gettop(L_);
    PushAnyValue(L_, val, writer_);
    EXPECT_EQ(lua_gettop(L_), top + 1);
    EXPECT_TRUE(lua_isinteger(L_, -1));
    EXPECT_EQ(lua_tointeger(L_, -1), 12345);
    lua_pop(L_, 1);
  }

  // Test int zero
  {
    auto val = obj["i_zero"].value();
    int top = lua_gettop(L_);
    PushAnyValue(L_, val, writer_);
    EXPECT_EQ(lua_gettop(L_), top + 1);
    EXPECT_TRUE(lua_isinteger(L_, -1));
    EXPECT_EQ(lua_tointeger(L_, -1), 0);
    lua_pop(L_, 1);
  }

  // Test int max string
  {
    auto val = obj["i_max"].value();
    int top = lua_gettop(L_);
    PushAnyValue(L_, val, writer_);
    EXPECT_EQ(lua_gettop(L_), top + 1);
    EXPECT_TRUE(lua_isinteger(L_, -1));
    EXPECT_EQ(lua_tointeger(L_, -1), INT64_MAX);
    lua_pop(L_, 1);
  }

  // Test int min string
  {
    auto val = obj["i_min"].value();
    int top = lua_gettop(L_);
    PushAnyValue(L_, val, writer_);
    EXPECT_EQ(lua_gettop(L_), top + 1);
    EXPECT_TRUE(lua_isinteger(L_, -1));
    EXPECT_EQ(lua_tointeger(L_, -1), INT64_MIN);
    lua_pop(L_, 1);
  }

  // Test double
  {
    auto val = obj["d"].value();
    int top = lua_gettop(L_);
    PushAnyValue(L_, val, writer_);
    EXPECT_EQ(lua_gettop(L_), top + 1);
    EXPECT_TRUE(lua_isnumber(L_, -1));
    EXPECT_FALSE(lua_isinteger(L_, -1));
    EXPECT_DOUBLE_EQ(lua_tonumber(L_, -1), 2.71828);
    lua_pop(L_, 1);
  }
}

TEST_F(OtlpCommonTest, PushAnyValueComplexTypes) {
  auto json = R"({
    "arr": {
      "arrayValue": {
        "values": [
          { "stringValue": "item1" },
          { "intValue": "42" }
        ]
      }
    },
    "map": {
      "kvlistValue": {
        "values": [
          { "key": "k1", "value": { "stringValue": "v1" } }
        ]
      }
    },
    "bytes": {
      "bytesValue": "AQIDBA=="
    }
  })"_padded;

  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();

  // Test arrayValue serialized as JSON string
  {
    auto val = obj["arr"].value();
    int top = lua_gettop(L_);
    PushAnyValue(L_, val, writer_);
    EXPECT_EQ(lua_gettop(L_), top + 1);
    EXPECT_TRUE(lua_isstring(L_, -1));
    std::string s = lua_tostring(L_, -1);
    EXPECT_NE(s.find("arrayValue"), std::string::npos);
    EXPECT_NE(s.find("item1"), std::string::npos);
    lua_pop(L_, 1);
  }

  // Test kvlistValue serialized as JSON string
  {
    auto val = obj["map"].value();
    int top = lua_gettop(L_);
    PushAnyValue(L_, val, writer_);
    EXPECT_EQ(lua_gettop(L_), top + 1);
    EXPECT_TRUE(lua_isstring(L_, -1));
    std::string s = lua_tostring(L_, -1);
    EXPECT_NE(s.find("kvlistValue"), std::string::npos);
    EXPECT_NE(s.find("k1"), std::string::npos);
    lua_pop(L_, 1);
  }

  // Test bytesValue serialized as JSON string
  {
    auto val = obj["bytes"].value();
    int top = lua_gettop(L_);
    PushAnyValue(L_, val, writer_);
    EXPECT_EQ(lua_gettop(L_), top + 1);
    EXPECT_TRUE(lua_isstring(L_, -1));
    std::string s = lua_tostring(L_, -1);
    EXPECT_NE(s.find("bytesValue"), std::string::npos);
    EXPECT_NE(s.find("AQIDBA=="), std::string::npos);
    lua_pop(L_, 1);
  }
}

TEST_F(OtlpCommonTest, PushAttributesTable) {
  auto json = R"({
    "attributes": [
      { "key": "str_attr", "value": { "stringValue": "hello" } },
      { "key": "int_attr", "value": { "intValue": "9223372036854775807" } },
      { "key": "dbl_attr", "value": { "doubleValue": 3.14159 } },
      { "key": "bool_attr", "value": { "boolValue": false } },
      { "key": "arr_attr", "value": { "arrayValue": { "values": [{ "stringValue": "elem" }] } } },
      { "key": "map_attr", "value": { "kvlistValue": { "values": [{ "key": "inner", "value": { "intValue": 1 } }] } } }
    ]
  })"_padded;

  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();
  auto attrs = obj["attributes"].value();

  int top_before = lua_gettop(L_);
  PushAttributesTable(L_, attrs, writer_);
  EXPECT_EQ(lua_gettop(L_), top_before + 1);
  EXPECT_TRUE(lua_istable(L_, -1));

  // Check str_attr
  lua_getfield(L_, -1, "str_attr");
  EXPECT_TRUE(lua_isstring(L_, -1));
  EXPECT_STREQ(lua_tostring(L_, -1), "hello");
  lua_pop(L_, 1);

  // Check int_attr
  lua_getfield(L_, -1, "int_attr");
  EXPECT_TRUE(lua_isinteger(L_, -1));
  EXPECT_EQ(lua_tointeger(L_, -1), INT64_MAX);
  lua_pop(L_, 1);

  // Check dbl_attr
  lua_getfield(L_, -1, "dbl_attr");
  EXPECT_TRUE(lua_isnumber(L_, -1));
  EXPECT_DOUBLE_EQ(lua_tonumber(L_, -1), 3.14159);
  lua_pop(L_, 1);

  // Check bool_attr
  lua_getfield(L_, -1, "bool_attr");
  EXPECT_TRUE(lua_isboolean(L_, -1));
  EXPECT_EQ(lua_toboolean(L_, -1), 0);
  lua_pop(L_, 1);

  // Check arr_attr
  lua_getfield(L_, -1, "arr_attr");
  EXPECT_TRUE(lua_isstring(L_, -1));
  std::string arr_json = lua_tostring(L_, -1);
  EXPECT_NE(arr_json.find("arrayValue"), std::string::npos);
  EXPECT_NE(arr_json.find("elem"), std::string::npos);
  lua_pop(L_, 1);

  // Check map_attr
  lua_getfield(L_, -1, "map_attr");
  EXPECT_TRUE(lua_isstring(L_, -1));
  std::string map_json = lua_tostring(L_, -1);
  EXPECT_NE(map_json.find("kvlistValue"), std::string::npos);
  EXPECT_NE(map_json.find("inner"), std::string::npos);
  lua_pop(L_, 1);

  lua_pop(L_, 1);  // Pop table
}

TEST_F(OtlpCommonTest, PushAttributesTableEmptyOrMissing) {
  // Empty array
  {
    auto json = R"({ "attributes": [] })"_padded;
    auto doc = parser_.iterate(json);
    auto obj = doc.get_object().value();
    auto attrs = obj["attributes"].value();

    int top_before = lua_gettop(L_);
    PushAttributesTable(L_, attrs, writer_);
    EXPECT_EQ(lua_gettop(L_), top_before + 1);
    EXPECT_TRUE(lua_istable(L_, -1));
    lua_pop(L_, 1);
  }

  // Non-array value (e.g. null)
  {
    auto json = R"({ "attributes": null })"_padded;
    auto doc = parser_.iterate(json);
    auto obj = doc.get_object().value();
    auto attrs = obj["attributes"].value();

    int top_before = lua_gettop(L_);
    PushAttributesTable(L_, attrs, writer_);
    EXPECT_EQ(lua_gettop(L_), top_before + 1);
    EXPECT_TRUE(lua_istable(L_, -1));
    lua_pop(L_, 1);
  }
}

TEST_F(OtlpCommonTest, PushAttributesTableSpecialCharsAndUnicode) {
  auto json = R"({
    "attributes": [
      {
        "key": "unicode_key",
        "value": { "stringValue": "héllo \"quoted\" \\ \n tab\t ✓" }
      }
    ]
  })"_padded;

  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();
  auto attrs = obj["attributes"].value();

  PushAttributesTable(L_, attrs, writer_);
  ASSERT_TRUE(lua_istable(L_, -1));

  lua_getfield(L_, -1, "unicode_key");
  ASSERT_TRUE(lua_isstring(L_, -1));
  std::string val = lua_tostring(L_, -1);
  EXPECT_NE(val.find("héllo"), std::string::npos);
  EXPECT_NE(val.find("quoted"), std::string::npos);
  EXPECT_NE(val.find("✓"), std::string::npos);
  lua_pop(L_, 2);  // pop string and table
}

TEST_F(OtlpCommonTest, PushAnyValueDirectScalars) {
  auto json = R"({
    "raw_str": "plain string",
    "raw_int": 987654,
    "raw_dbl": 1.414,
    "raw_bool": true,
    "raw_null": null,
    "empty_obj": {}
  })"_padded;

  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();

  {
    auto v = obj["raw_str"].value();
    PushAnyValue(L_, v, writer_);
    EXPECT_TRUE(lua_isstring(L_, -1));
    EXPECT_STREQ(lua_tostring(L_, -1), "plain string");
    lua_pop(L_, 1);
  }
  {
    auto v = obj["raw_int"].value();
    PushAnyValue(L_, v, writer_);
    EXPECT_TRUE(lua_isinteger(L_, -1));
    EXPECT_EQ(lua_tointeger(L_, -1), 987654);
    lua_pop(L_, 1);
  }
  {
    auto v = obj["raw_dbl"].value();
    PushAnyValue(L_, v, writer_);
    EXPECT_TRUE(lua_isnumber(L_, -1));
    EXPECT_FALSE(lua_isinteger(L_, -1));
    EXPECT_DOUBLE_EQ(lua_tonumber(L_, -1), 1.414);
    lua_pop(L_, 1);
  }
  {
    auto v = obj["raw_bool"].value();
    PushAnyValue(L_, v, writer_);
    EXPECT_TRUE(lua_isboolean(L_, -1));
    EXPECT_EQ(lua_toboolean(L_, -1), 1);
    lua_pop(L_, 1);
  }
  {
    auto v = obj["raw_null"].value();
    PushAnyValue(L_, v, writer_);
    EXPECT_TRUE(lua_isnil(L_, -1));
    lua_pop(L_, 1);
  }
  {
    auto v = obj["empty_obj"].value();
    PushAnyValue(L_, v, writer_);
    EXPECT_TRUE(lua_isnil(L_, -1));
    lua_pop(L_, 1);
  }
}

TEST_F(OtlpCommonTest, ParseInt64DoubleAndWhitespace) {
  auto json = R"({
    "dbl_num": 12345.67,
    "ws_neg": "   -42   ",
    "ws_tab": "\t-9999\t",
    "empty_str": ""
  })"_padded;

  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();

  auto v_dbl = obj["dbl_num"].value();
  EXPECT_EQ(ParseInt64(v_dbl), 12345);

  auto v_neg = obj["ws_neg"].value();
  EXPECT_EQ(ParseInt64(v_neg), -42);

  auto v_tab = obj["ws_tab"].value();
  EXPECT_EQ(ParseInt64(v_tab), -9999);

  auto v_empty = obj["empty_str"].value();
  EXPECT_EQ(ParseInt64(v_empty), 0);
}

TEST_F(OtlpCommonTest, RejectsTrailingGarbageInNumericStrings) {
  auto json = R"({
    "bad_int": "123abc",
    "bad_uint": "456xyz",
    "bad_dbl": "12.34bad"
  })"_padded;

  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();

  auto v_int = obj["bad_int"].value();
  EXPECT_EQ(ParseInt64(v_int), 0);

  auto v_uint = obj["bad_uint"].value();
  EXPECT_EQ(ParseUint64(v_uint), 0ULL);

  auto v_dbl = obj["bad_dbl"].value();
  double out = -1.0;
  EXPECT_FALSE(TryParseDouble(v_dbl, out));
  EXPECT_DOUBLE_EQ(out, -1.0);
}

TEST_F(OtlpCommonTest, ParseInt64OutOfRangeDoubleDoesNotInvokeUB) {
  auto json = R"({
    "huge_pos": 1e30,
    "huge_neg": -1e30,
    "two_pow_63": 9223372036854775808.0
  })"_padded;

  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();

  auto v_pos = obj["huge_pos"].value();
  EXPECT_EQ(ParseInt64(v_pos), 0);

  auto v_neg = obj["huge_neg"].value();
  EXPECT_EQ(ParseInt64(v_neg), 0);

  auto v_lim = obj["two_pow_63"].value();
  EXPECT_EQ(ParseInt64(v_lim), 0);
}

TEST_F(OtlpCommonTest, PushAnyValueClampsUint64AboveInt64Max) {
  auto json = R"({
    "raw_u64": 18446744073709551615,
    "obj_int_u64_num": { "intValue": 18446744073709551615 },
    "obj_int_u64_str": { "intValue": "18446744073709551615" }
  })"_padded;

  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();

  {
    auto v = obj["raw_u64"].value();
    PushAnyValue(L_, v, writer_);
    EXPECT_TRUE(lua_isinteger(L_, -1));
    EXPECT_EQ(lua_tointeger(L_, -1), INT64_MAX);
    lua_pop(L_, 1);
  }
  {
    auto v = obj["obj_int_u64_num"].value();
    PushAnyValue(L_, v, writer_);
    EXPECT_TRUE(lua_isinteger(L_, -1));
    EXPECT_EQ(lua_tointeger(L_, -1), INT64_MAX);
    lua_pop(L_, 1);
  }
  {
    auto v = obj["obj_int_u64_str"].value();
    PushAnyValue(L_, v, writer_);
    EXPECT_TRUE(lua_isinteger(L_, -1));
    EXPECT_EQ(lua_tointeger(L_, -1), INT64_MAX);
    lua_pop(L_, 1);
  }
}

TEST_F(OtlpCommonTest, PushAttributesTableExtractsServiceNameSinglePass) {
  // 1. KV-array format with "service.name"
  {
    auto json = R"({
      "attributes": [
        { "key": "service.name", "value": { "stringValue": "kv-array-svc" } },
        { "key": "env", "value": { "stringValue": "staging" } }
      ]
    })"_padded;
    auto doc = parser_.iterate(json);
    auto obj = doc.get_object().value();
    auto attrs = obj["attributes"].value();

    std::string_view svc_name = "unknown_service";
    PushAttributesTable(L_, attrs, writer_, &svc_name);
    EXPECT_EQ(svc_name, "kv-array-svc");
    ASSERT_TRUE(lua_istable(L_, -1));
    lua_getfield(L_, -1, "service.name");
    EXPECT_STREQ(lua_tostring(L_, -1), "kv-array-svc");
    lua_pop(L_, 1);
    lua_getfield(L_, -1, "env");
    EXPECT_STREQ(lua_tostring(L_, -1), "staging");
    lua_pop(L_, 2);
  }

  // 2. Object-map format with "service_name"
  {
    auto json = R"({
      "attributes": {
        "service_name": "map-snake-svc",
        "region": "us-east-1"
      }
    })"_padded;
    auto doc = parser_.iterate(json);
    auto obj = doc.get_object().value();
    auto attrs = obj["attributes"].value();

    std::string_view svc_name = "unknown_service";
    PushAttributesTable(L_, attrs, writer_, &svc_name);
    EXPECT_EQ(svc_name, "map-snake-svc");
    ASSERT_TRUE(lua_istable(L_, -1));
    lua_getfield(L_, -1, "service_name");
    EXPECT_STREQ(lua_tostring(L_, -1), "map-snake-svc");
    lua_pop(L_, 1);
    lua_getfield(L_, -1, "region");
    EXPECT_STREQ(lua_tostring(L_, -1), "us-east-1");
    lua_pop(L_, 2);
  }
}

TEST_F(OtlpCommonTest, TryParseDoubleWhitespaceScientificAndMalformed) {
  auto json = R"({
    "ws_pos": "  123.456  ",
    "ws_tab_newline": "\t-0.5\n",
    "ws_crlf": "\r\n  42.0  \t",
    "sci_pos": "1e6",
    "sci_neg": "-2.5e-3",
    "int_str": "99",
    "num_pos": 42.5,
    "num_int": 100,
    "num_neg": -3.14,
    "bad_alpha": "not_a_number",
    "bad_trail": "12.34bad",
    "bad_ws_only": "   ",
    "bad_empty": "",
    "bad_dot_only": "-.",
    "bad_signs": "+-12",
    "bad_multi_dots": "1.2.3",
    "val_bool": true,
    "val_null": null,
    "val_obj": {},
    "val_arr": [1.0]
  })"_padded;

  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();

  // Valid whitespace padded string doubles
  {
    double out = 0.0;
    auto v = obj["ws_pos"].value();
    EXPECT_TRUE(TryParseDouble(v, out));
    EXPECT_DOUBLE_EQ(out, 123.456);
    EXPECT_DOUBLE_EQ(ParseDouble(v), 123.456);
  }
  {
    double out = 0.0;
    auto v = obj["ws_tab_newline"].value();
    EXPECT_TRUE(TryParseDouble(v, out));
    EXPECT_DOUBLE_EQ(out, -0.5);
    EXPECT_DOUBLE_EQ(ParseDouble(v), -0.5);
  }
  {
    double out = 0.0;
    auto v = obj["ws_crlf"].value();
    EXPECT_TRUE(TryParseDouble(v, out));
    EXPECT_DOUBLE_EQ(out, 42.0);
    EXPECT_DOUBLE_EQ(ParseDouble(v), 42.0);
  }

  // Valid scientific notation strings
  {
    double out = 0.0;
    auto v = obj["sci_pos"].value();
    EXPECT_TRUE(TryParseDouble(v, out));
    EXPECT_DOUBLE_EQ(out, 1000000.0);
  }
  {
    double out = 0.0;
    auto v = obj["sci_neg"].value();
    EXPECT_TRUE(TryParseDouble(v, out));
    EXPECT_DOUBLE_EQ(out, -0.0025);
  }
  {
    double out = 0.0;
    auto v = obj["int_str"].value();
    EXPECT_TRUE(TryParseDouble(v, out));
    EXPECT_DOUBLE_EQ(out, 99.0);
  }

  // Valid numbers
  {
    double out = 0.0;
    auto v = obj["num_pos"].value();
    EXPECT_TRUE(TryParseDouble(v, out));
    EXPECT_DOUBLE_EQ(out, 42.5);
  }
  {
    double out = 0.0;
    auto v = obj["num_int"].value();
    EXPECT_TRUE(TryParseDouble(v, out));
    EXPECT_DOUBLE_EQ(out, 100.0);
  }
  {
    double out = 0.0;
    auto v = obj["num_neg"].value();
    EXPECT_TRUE(TryParseDouble(v, out));
    EXPECT_DOUBLE_EQ(out, -3.14);
  }

  // Malformed and invalid strings
  const char* bad_keys[] = {"bad_alpha",    "bad_trail", "bad_ws_only",    "bad_empty",
                            "bad_dot_only", "bad_signs", "bad_multi_dots", "val_bool",
                            "val_null",     "val_obj",   "val_arr"};
  for (const char* k : bad_keys) {
    double out = -999.0;
    auto v = obj[k].value();
    EXPECT_FALSE(TryParseDouble(v, out)) << "Expected false for key: " << k;
    EXPECT_DOUBLE_EQ(out, -999.0) << "Output modified for key: " << k;
    EXPECT_DOUBLE_EQ(ParseDouble(v), 0.0) << "ParseDouble not zero for key: " << k;
  }
}

TEST_F(OtlpCommonTest, ParseInt64ScientificAndBounds) {
  auto json = R"({
    "sci_str": "1e6",
    "sci_neg_str": "-1e6",
    "flt_str": "123.456",
    "flt_neg_str": "-0.5",
    "u64_max_str": "18446744073709551615",
    "i64_max_plus_one_str": "9223372036854775808",
    "overflow_str": "999999999999999999999999999999999999",
    "underflow_str": "-999999999999999999999999999999999999",
    "sci_num_valid": 1e6,
    "sci_num_neg_valid": -500.0,
    "sci_num_huge": 1e30,
    "sci_num_huge_neg": -1e30,
    "num_two_pow_63": 9223372036854775808.0,
    "num_neg_two_pow_63_exact": -9223372036854775808.0,
    "num_neg_below_min": -9223372036854777856.0
  })"_padded;

  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();

  // Integer from_chars does not accept scientific strings
  auto v_sci = obj["sci_str"].value();
  EXPECT_EQ(ParseInt64(v_sci), 0);

  auto v_sci_neg = obj["sci_neg_str"].value();
  EXPECT_EQ(ParseInt64(v_sci_neg), 0);

  // Floating strings fail full-string integer parsing
  auto v_flt = obj["flt_str"].value();
  EXPECT_EQ(ParseInt64(v_flt), 0);

  auto v_flt_neg = obj["flt_neg_str"].value();
  EXPECT_EQ(ParseInt64(v_flt_neg), 0);

  // String bounds clamping and overflow
  auto v_u64_max = obj["u64_max_str"].value();
  EXPECT_EQ(ParseInt64(v_u64_max), INT64_MAX);

  auto v_i64_plus_one = obj["i64_max_plus_one_str"].value();
  EXPECT_EQ(ParseInt64(v_i64_plus_one), INT64_MAX);

  auto v_overflow = obj["overflow_str"].value();
  EXPECT_EQ(ParseInt64(v_overflow), 0);

  auto v_underflow = obj["underflow_str"].value();
  EXPECT_EQ(ParseInt64(v_underflow), 0);

  // Number doubles in finite int64 range convert safely
  auto v_num_valid = obj["sci_num_valid"].value();
  EXPECT_EQ(ParseInt64(v_num_valid), 1000000);

  auto v_num_neg_valid = obj["sci_num_neg_valid"].value();
  EXPECT_EQ(ParseInt64(v_num_neg_valid), -500);

  // Out of range doubles return 0 safely
  auto v_huge = obj["sci_num_huge"].value();
  EXPECT_EQ(ParseInt64(v_huge), 0);

  auto v_huge_neg = obj["sci_num_huge_neg"].value();
  EXPECT_EQ(ParseInt64(v_huge_neg), 0);

  auto v_two_pow_63 = obj["num_two_pow_63"].value();
  EXPECT_EQ(ParseInt64(v_two_pow_63), 0);

  auto v_neg_exact = obj["num_neg_two_pow_63_exact"].value();
  EXPECT_EQ(ParseInt64(v_neg_exact), INT64_MIN);

  auto v_neg_below = obj["num_neg_below_min"].value();
  EXPECT_EQ(ParseInt64(v_neg_below), 0);
}

TEST_F(OtlpCommonTest, PushAnyValueMixedArraysAndTypes) {
  auto json = R"({
    "mixed_array": [true, "sample", 123, 45.67, null, [1, 2], {"inner_k": "inner_v"}],
    "arr_obj": {
      "arrayValue": {
        "values": [
          {"stringValue": "val1"},
          {"intValue": 42},
          {"doubleValue": 3.14},
          {"boolValue": false},
          {"bytesValue": "aGVsbG8="},
          {"arrayValue": {"values": [{"stringValue": "nested"}]}}
        ]
      }
    },
    "uint_obj": {
      "uintValue": 100
    },
    "uint_snake_obj": {
      "uint_value": "18446744073709551615"
    }
  })"_padded;

  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();

  // Direct mixed array pushed as JSON string
  {
    auto v = obj["mixed_array"].value();
    PushAnyValue(L_, v, writer_);
    ASSERT_TRUE(lua_isstring(L_, -1));
    std::string s = lua_tostring(L_, -1);
    EXPECT_NE(s.find("sample"), std::string::npos);
    EXPECT_NE(s.find("123"), std::string::npos);
    EXPECT_NE(s.find("inner_k"), std::string::npos);
    lua_pop(L_, 1);
  }

  // arrayValue with multiple heterogeneous types serialized as JSON string
  {
    auto v = obj["arr_obj"].value();
    PushAnyValue(L_, v, writer_);
    ASSERT_TRUE(lua_isstring(L_, -1));
    std::string s = lua_tostring(L_, -1);
    EXPECT_NE(s.find("val1"), std::string::npos);
    EXPECT_NE(s.find("42"), std::string::npos);
    EXPECT_NE(s.find("3.14"), std::string::npos);
    EXPECT_NE(s.find("aGVsbG8="), std::string::npos);
    EXPECT_NE(s.find("nested"), std::string::npos);
    lua_pop(L_, 1);
  }

  // uintValue integer
  {
    auto v = obj["uint_obj"].value();
    PushAnyValue(L_, v, writer_);
    ASSERT_TRUE(lua_isinteger(L_, -1));
    EXPECT_EQ(lua_tointeger(L_, -1), 100);
    lua_pop(L_, 1);
  }

  // uint_value string clamped to INT64_MAX
  {
    auto v = obj["uint_snake_obj"].value();
    PushAnyValue(L_, v, writer_);
    ASSERT_TRUE(lua_isinteger(L_, -1));
    EXPECT_EQ(lua_tointeger(L_, -1), INT64_MAX);
    lua_pop(L_, 1);
  }
}

TEST_F(OtlpCommonTest, PushAttributesTableArrayAndMapFormats) {
  // 1. Array format with mixed valid and malformed items
  {
    auto json = R"({
      "attributes": [
        {"key": "str_item", "value": {"stringValue": "text"}},
        {"key": "int_item", "value": {"intValue": 999}},
        {"invalid_no_key": {"stringValue": "skipped"}},
        {"key": 12345},
        {"key": "missing_val"}
      ]
    })"_padded;

    auto doc = parser_.iterate(json);
    auto obj = doc.get_object().value();
    auto attrs = obj["attributes"].value();

    PushAttributesTable(L_, attrs, writer_);
    ASSERT_TRUE(lua_istable(L_, -1));

    lua_getfield(L_, -1, "str_item");
    EXPECT_STREQ(lua_tostring(L_, -1), "text");
    lua_pop(L_, 1);

    lua_getfield(L_, -1, "int_item");
    EXPECT_EQ(lua_tointeger(L_, -1), 999);
    lua_pop(L_, 1);

    lua_pop(L_, 1);  // pop table
  }

  // 2. Map format with direct scalar and object values
  {
    auto json = R"({
      "attributes": {
        "m_obj_str": {"stringValue": "map_val"},
        "m_obj_int": {"intValue": 456},
        "m_direct_str": "direct_hello",
        "m_direct_int": 789,
        "m_direct_dbl": 1.75,
        "m_direct_bool": true,
        "m_direct_arr": ["elem1", "elem2"]
      }
    })"_padded;

    auto doc = parser_.iterate(json);
    auto obj = doc.get_object().value();
    auto attrs = obj["attributes"].value();

    PushAttributesTable(L_, attrs, writer_);
    ASSERT_TRUE(lua_istable(L_, -1));

    lua_getfield(L_, -1, "m_obj_str");
    EXPECT_STREQ(lua_tostring(L_, -1), "map_val");
    lua_pop(L_, 1);

    lua_getfield(L_, -1, "m_obj_int");
    EXPECT_EQ(lua_tointeger(L_, -1), 456);
    lua_pop(L_, 1);

    lua_getfield(L_, -1, "m_direct_str");
    EXPECT_STREQ(lua_tostring(L_, -1), "direct_hello");
    lua_pop(L_, 1);

    lua_getfield(L_, -1, "m_direct_int");
    EXPECT_EQ(lua_tointeger(L_, -1), 789);
    lua_pop(L_, 1);

    lua_getfield(L_, -1, "m_direct_dbl");
    EXPECT_DOUBLE_EQ(lua_tonumber(L_, -1), 1.75);
    lua_pop(L_, 1);

    lua_getfield(L_, -1, "m_direct_bool");
    EXPECT_EQ(lua_toboolean(L_, -1), 1);
    lua_pop(L_, 1);

    lua_getfield(L_, -1, "m_direct_arr");
    ASSERT_TRUE(lua_isstring(L_, -1));
    std::string arr_str = lua_tostring(L_, -1);
    EXPECT_NE(arr_str.find("elem1"), std::string::npos);
    lua_pop(L_, 1);

    lua_pop(L_, 1);  // pop table
  }
}

TEST_F(OtlpCommonTest, ExtractServiceNameEdgeCases) {
  // Empty attributes array
  {
    auto json = R"({ "attributes": [] })"_padded;
    auto doc = parser_.iterate(json);
    auto obj = doc.get_object().value();
    EXPECT_EQ(ExtractServiceName(obj), "unknown_service");
  }

  // Missing service.name in attributes array
  {
    auto json = R"({
      "attributes": [
        {"key": "host.name", "value": {"stringValue": "server-1"}}
      ]
    })"_padded;
    auto doc = parser_.iterate(json);
    auto obj = doc.get_object().value();
    EXPECT_EQ(ExtractServiceName(obj), "unknown_service");
  }

  // Numeric service.name in array with object value -> unknown_service
  {
    auto json = R"({
      "attributes": [
        {"key": "service.name", "value": {"intValue": 404}}
      ]
    })"_padded;
    auto doc = parser_.iterate(json);
    auto obj = doc.get_object().value();
    EXPECT_EQ(ExtractServiceName(obj), "unknown_service");
  }

  // Numeric service.name in array with direct scalar -> unknown_service
  {
    auto json = R"({
      "attributes": [
        {"key": "service.name", "value": 404}
      ]
    })"_padded;
    auto doc = parser_.iterate(json);
    auto obj = doc.get_object().value();
    EXPECT_EQ(ExtractServiceName(obj), "unknown_service");
  }

  // Direct string value in array format -> returns string
  {
    auto json = R"({
      "attributes": [
        {"key": "service.name", "value": "direct-array-service"}
      ]
    })"_padded;
    auto doc = parser_.iterate(json);
    auto obj = doc.get_object().value();
    EXPECT_EQ(ExtractServiceName(obj), "direct-array-service");
  }

  // Missing service.name in map format -> unknown_service
  {
    auto json = R"({
      "attributes": { "host.name": "server-2" }
    })"_padded;
    auto doc = parser_.iterate(json);
    auto obj = doc.get_object().value();
    EXPECT_EQ(ExtractServiceName(obj), "unknown_service");
  }

  // Numeric service.name in map format -> unknown_service
  {
    auto json = R"({
      "attributes": { "service.name": 999 }
    })"_padded;
    auto doc = parser_.iterate(json);
    auto obj = doc.get_object().value();
    EXPECT_EQ(ExtractServiceName(obj), "unknown_service");
  }

  // Numeric service.name in map with object value -> unknown_service
  {
    auto json = R"({
      "attributes": { "service.name": {"intValue": 999} }
    })"_padded;
    auto doc = parser_.iterate(json);
    auto obj = doc.get_object().value();
    EXPECT_EQ(ExtractServiceName(obj), "unknown_service");
  }

  // Empty attributes map -> unknown_service
  {
    auto json = R"({ "attributes": {} })"_padded;
    auto doc = parser_.iterate(json);
    auto obj = doc.get_object().value();
    EXPECT_EQ(ExtractServiceName(obj), "unknown_service");
  }

  // Null attributes -> unknown_service
  {
    auto json = R"({ "attributes": null })"_padded;
    auto doc = parser_.iterate(json);
    auto obj = doc.get_object().value();
    EXPECT_EQ(ExtractServiceName(obj), "unknown_service");
  }

  // Missing attributes field entirely -> unknown_service
  {
    auto json = R"({ "other_field": "val" })"_padded;
    auto doc = parser_.iterate(json);
    auto obj = doc.get_object().value();
    EXPECT_EQ(ExtractServiceName(obj), "unknown_service");
  }
}

TEST_F(OtlpCommonTest, PushAttributesTableDuplicateServiceNameLongThenInt) {
  std::string long_svc(80, 'S');
  std::string raw = R"({"attributes":[{"key":"service.name","value":{"stringValue":")" + long_svc +
                    R"("}},{"key":"service.name","value":{"intValue":"42"}}]})";
  simdjson::padded_string json(raw);
  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();
  auto attrs = obj["attributes"].value();

  std::string_view sn = "unknown_service";
  PushAttributesTable(L_, attrs, writer_, &sn);
  lua_gc(L_, LUA_GCCOLLECT, 0);
  EXPECT_EQ(sn, "unknown_service");
  ASSERT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, -1, "service.name");
  EXPECT_TRUE(lua_isinteger(L_, -1));
  EXPECT_EQ(lua_tointeger(L_, -1), 42);
  lua_pop(L_, 2);
}

TEST_F(OtlpCommonTest, PushAttributesTableDuplicateServiceNameLongThenEmptyString) {
  std::string long_svc(80, 'S');
  std::string raw = R"({"attributes":[{"key":"service.name","value":{"stringValue":")" + long_svc +
                    R"("}},{"key":"service.name","value":{"stringValue":""}}]})";
  simdjson::padded_string json(raw);
  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();
  auto attrs = obj["attributes"].value();

  std::string_view sn = "unknown_service";
  PushAttributesTable(L_, attrs, writer_, &sn);
  lua_gc(L_, LUA_GCCOLLECT, 0);
  EXPECT_EQ(sn, "unknown_service");
  lua_pop(L_, 1);
}

TEST_F(OtlpCommonTest, PushAttributesTableObjectFormDuplicateResets) {
  std::string long_svc(80, 'B');
  std::string raw = R"({"attributes":{"service.name":{"stringValue":")" + long_svc +
                    R"("},"service_name":{"boolValue":true}}})";
  simdjson::padded_string json(raw);
  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();
  auto attrs = obj["attributes"].value();

  std::string_view sn = "unknown_service";
  PushAttributesTable(L_, attrs, writer_, &sn);
  lua_gc(L_, LUA_GCCOLLECT, 0);
  EXPECT_EQ(sn, "unknown_service");
  lua_pop(L_, 1);
}

TEST_F(OtlpCommonTest, PushAttributesTableRejectsArrayValueServiceName) {
  std::string raw =
      R"({"attributes":[{"key":"service.name","value":{"arrayValue":{"values":[{"stringValue":"x"}]}}}]})";
  simdjson::padded_string json(raw);
  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();
  auto attrs = obj["attributes"].value();

  std::string_view sn = "unknown_service";
  PushAttributesTable(L_, attrs, writer_, &sn);
  lua_gc(L_, LUA_GCCOLLECT, 0);
  EXPECT_EQ(sn, "unknown_service");
  ASSERT_TRUE(lua_istable(L_, -1));
  lua_getfield(L_, -1, "service.name");
  ASSERT_TRUE(lua_isstring(L_, -1));
  std::string val = lua_tostring(L_, -1);
  EXPECT_NE(val.find("arrayValue"), std::string::npos);
  EXPECT_NE(val.find("\"x\""), std::string::npos);
  lua_pop(L_, 2);
}

TEST_F(OtlpCommonTest, PushAttributesTableLongServiceNameSurvivesGc) {
  std::string long_svc(80, 'S');
  std::string raw =
      R"({"attributes":[{"key":"service.name","value":{"stringValue":")" + long_svc + R"("}}]})";
  simdjson::padded_string json(raw);
  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();
  auto attrs = obj["attributes"].value();

  std::string_view sn = "unknown_service";
  PushAttributesTable(L_, attrs, writer_, &sn);
  lua_gc(L_, LUA_GCCOLLECT, 0);
  EXPECT_EQ(sn, long_svc);
  lua_pop(L_, 1);
}

TEST_F(OtlpCommonTest, NormalizeOtelIdExactBinary16Bytes) {
  IdHexBuffer buf{};
  std::string raw(16, '\xaa');
  auto res = NormalizeTraceId(raw, buf);
  EXPECT_EQ(res, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  EXPECT_EQ(res.size(), 32);

  std::string raw2(16, '\x12');
  auto res2 = NormalizeTraceId(raw2, buf);
  EXPECT_EQ(res2, "12121212121212121212121212121212");
}

TEST_F(OtlpCommonTest, NormalizeOtelIdExactBinary8Bytes) {
  IdHexBuffer buf{};
  std::string raw(8, '\xbb');
  auto res = NormalizeSpanId(raw, buf);
  EXPECT_EQ(res, "bbbbbbbbbbbbbbbb");
  EXPECT_EQ(res.size(), 16);

  std::string raw2(8, '\x34');
  auto res2 = NormalizeSpanId(raw2, buf);
  EXPECT_EQ(res2, "3434343434343434");
}

TEST_F(OtlpCommonTest, NormalizeOtelIdBinaryWithEmbeddedNul) {
  IdHexBuffer buf{};
  // 8-byte binary with leading, embedded, and trailing \0 bytes
  std::string_view raw_span("\x00\xf0\x67\xaa\x0b\xa9\x02\xb7", 8);
  auto res_span = NormalizeSpanId(raw_span, buf);
  EXPECT_EQ(res_span, "00f067aa0ba902b7");

  // 16-byte binary with leading and trailing \0 bytes
  std::string_view raw_trace("\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x00",
                             16);
  auto res_trace = NormalizeTraceId(raw_trace, buf);
  EXPECT_EQ(res_trace, "000102030405060708090a0b0c0d0e00");
}

TEST_F(OtlpCommonTest, NormalizeOtelIdAllZeroBinary) {
  IdHexBuffer buf{};
  // 16x 0x00 -> 32x '0'
  std::string raw_16(16, '\0');
  auto res_16 = NormalizeTraceId(raw_16, buf);
  EXPECT_EQ(res_16, "00000000000000000000000000000000");

  // 8x 0x00 -> 16x '0'
  std::string raw_8(8, '\0');
  auto res_8 = NormalizeSpanId(raw_8, buf);
  EXPECT_EQ(res_8, "0000000000000000");
}

TEST_F(OtlpCommonTest, NormalizeOtelIdExactLowercaseHexZeroCopy) {
  IdHexBuffer buf{};
  // 32-char lowercase hex
  std::string_view hex32 = "4bf92f3577b34da6a3ce929d0e0e4736";
  auto res32 = NormalizeTraceId(hex32, buf);
  EXPECT_EQ(res32, hex32);
  EXPECT_EQ(res32.data(), hex32.data());  // Zero-copy verification

  // 16-char lowercase hex
  std::string_view hex16 = "00f067aa0ba902b7";
  auto res16 = NormalizeSpanId(hex16, buf);
  EXPECT_EQ(res16, hex16);
  EXPECT_EQ(res16.data(), hex16.data());  // Zero-copy verification
}

TEST_F(OtlpCommonTest, NormalizeOtelIdUppercaseAndMixedCaseHex) {
  IdHexBuffer buf{};
  // 32-char uppercase hex
  std::string_view hex32_upper = "4BF92F3577B34DA6A3CE929D0E0E4736";
  auto res32_upper = NormalizeTraceId(hex32_upper, buf);
  EXPECT_EQ(res32_upper, "4bf92f3577b34da6a3ce929d0e0e4736");

  // 32-char mixed-case hex
  std::string_view hex32_mixed = "4bF92f3577b34Da6A3ce929D0e0E4736";
  auto res32_mixed = NormalizeTraceId(hex32_mixed, buf);
  EXPECT_EQ(res32_mixed, "4bf92f3577b34da6a3ce929d0e0e4736");

  // 16-char uppercase hex
  std::string_view hex16_upper = "00F067AA0BA902B7";
  auto res16_upper = NormalizeSpanId(hex16_upper, buf);
  EXPECT_EQ(res16_upper, "00f067aa0ba902b7");
}

TEST_F(OtlpCommonTest, NormalizeOtelIdEmptyString) {
  IdHexBuffer buf{};
  EXPECT_EQ(NormalizeTraceId("", buf), "");
  EXPECT_EQ(NormalizeSpanId("", buf), "");
  EXPECT_EQ(NormalizeOtelId("", 16, buf), "");
  EXPECT_EQ(NormalizeOtelId("", 8, buf), "");
}

TEST_F(OtlpCommonTest, NormalizeOtelIdMalformedLengthNonPrintable) {
  IdHexBuffer buf{};
  // 4-byte binary with non-printable bytes -> 8-char hex
  std::string_view raw_4("\x00\x01\x02\x03", 4);
  auto res_4 = NormalizeOtelId(raw_4, 16, buf);
  EXPECT_EQ(res_4, "00010203");

  // > 32 bytes binary with non-printable bytes -> ""
  std::string raw_33(33, '\xff');
  auto res_33 = NormalizeOtelId(raw_33, 16, buf);
  EXPECT_EQ(res_33, "");
}

TEST_F(OtlpCommonTest, NormalizeOtelIdMalformedLengthPrintableAscii) {
  IdHexBuffer buf{};
  // Short non-hex printable ASCII strings preserved unchanged
  EXPECT_EQ(NormalizeTraceId("t1", buf), "t1");
  EXPECT_EQ(NormalizeSpanId("t1", buf), "t1");
  EXPECT_EQ(NormalizeTraceId("1111", buf), "1111");
  EXPECT_EQ(NormalizeTraceId("only_trace_id", buf), "only_trace_id");
}

TEST_F(OtlpCommonTest, NormalizeOtelIdExactBinaryAsciiCollision) {
  IdHexBuffer buf{};
  // 16-byte string that happens to be 16 ASCII hex characters
  std::string_view raw_16 = "0123456789abcdef";
  // For TraceId (expected 16 binary bytes), it MUST be hex-encoded to 32 characters
  auto res_trace = NormalizeTraceId(raw_16, buf);
  EXPECT_EQ(res_trace, "30313233343536373839616263646566");

  // For SpanId (expected 8 binary bytes), raw_16 is 16 lowercase hex chars, so zero-copy
  auto res_span = NormalizeSpanId(raw_16, buf);
  EXPECT_EQ(res_span, "0123456789abcdef");
  EXPECT_EQ(res_span.data(), raw_16.data());
}

TEST_F(OtlpCommonTest, ExtractStringFieldDualKey) {
  auto json = R"({
    "traceId": "canonical-trace-id",
    "span_id": "snake-span-id",
    "null_id": null,
    "num_id": 9999
  })"_padded;

  auto doc = parser_.iterate(json);
  auto obj = doc.get_object().value();

  // Primary key found
  EXPECT_EQ(ExtractStringField(obj, "traceId", "trace_id", ""), "canonical-trace-id");

  // Secondary key found
  EXPECT_EQ(ExtractStringField(obj, "spanId", "span_id", ""), "snake-span-id");

  // Neither key found -> default value returned
  EXPECT_EQ(ExtractStringField(obj, "missing1", "missing2", "fallback"), "fallback");
  EXPECT_EQ(ExtractStringField(obj, "missing1", "missing2", ""), "");

  // Primary key is null/number, secondary key missing -> default value returned
  EXPECT_EQ(ExtractStringField(obj, "null_id", "null_id_alt", "def"), "def");
  EXPECT_EQ(ExtractStringField(obj, "num_id", "num_id_alt", "def"), "def");
}

}  // namespace
}  // namespace vector_otel::core
