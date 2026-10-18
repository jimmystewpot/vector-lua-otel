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

}  // namespace
}  // namespace vector_otel::core
