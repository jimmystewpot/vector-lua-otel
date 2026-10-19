#include "vector_otel/core/table_reader.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

#include "vector_otel/core/fast_json_writer.hpp"
#include "vector_otel/core/lua_bridge.hpp"
#include "vector_otel/core/timestamp.hpp"

namespace vector_otel::core {

namespace {

bool PrepareNumericStringView(lua_State* L, int idx, std::string_view& out) {
  if (lua_type(L, idx) != LUA_TSTRING) {
    return false;
  }
  size_t len = 0;
  const char* s = lua_tolstring(L, idx, &len);
  if (s == nullptr) {
    return false;
  }
  std::string_view sv(s, len);
  while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' ||
                         sv.front() == '\r' || sv.front() == '\n')) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' ||
                         sv.back() == '\r' || sv.back() == '\n')) {
    sv.remove_suffix(1);
  }
  if (!sv.empty() && sv.front() == '+') {
    sv.remove_prefix(1);
    if (!sv.empty() && sv.front() == '-') {
      return false;
    }
  }
  if (sv.empty()) {
    return false;
  }
  out = sv;
  return true;
}

bool LuaFloatToInt64(double d, int64_t& out) {
  if (std::isfinite(d) && d >= -9223372036854775808.0 &&
      d < 9223372036854775808.0) {
    out = static_cast<int64_t>(d);
    return true;
  }
  return false;
}

bool LuaFloatToUint64(double d, uint64_t& out) {
  if (std::isfinite(d) && d >= 0.0 && d < 18446744073709551616.0) {
    out = static_cast<uint64_t>(d);
    return true;
  }
  return false;
}

bool ParseLuaStringInt64(lua_State* L, int idx, int64_t& out) {
  std::string_view sv;
  if (!PrepareNumericStringView(L, idx, sv)) {
    return false;
  }
  int64_t val = 0;
  const char* end = sv.data() + sv.size();
  auto [ptr, ec] = std::from_chars(sv.data(), end, val);
  if (ec == std::errc() && ptr == end) {
    out = val;
    return true;
  }
  return false;
}

bool ParseLuaStringUint64(lua_State* L, int idx, uint64_t& out) {
  std::string_view sv;
  if (!PrepareNumericStringView(L, idx, sv)) {
    return false;
  }
  uint64_t val = 0;
  const char* end = sv.data() + sv.size();
  auto [ptr, ec] = std::from_chars(sv.data(), end, val);
  if (ec == std::errc() && ptr == end) {
    out = val;
    return true;
  }
  return false;
}

bool ParseLuaStringDouble(lua_State* L, int idx, double& out) {
  std::string_view sv;
  if (!PrepareNumericStringView(L, idx, sv)) {
    return false;
  }
  double val = 0.0;
  const char* end = sv.data() + sv.size();
  auto [ptr, ec] = std::from_chars(sv.data(), end, val);
  if (ec == std::errc() && ptr == end && std::isfinite(val)) {
    out = val;
    return true;
  }
  return false;
}

}  // namespace

bool GetTableField(lua_State* L, int table_idx, const char* name1,
                   const char* name2) {
  int abs_idx = lua_absindex(L, table_idx);
  lua_getfield(L, abs_idx, name1);
  if (!lua_isnil(L, -1)) {
    return true;
  }
  lua_pop(L, 1);
  if (name2 != nullptr) {
    lua_getfield(L, abs_idx, name2);
    if (!lua_isnil(L, -1)) {
      return true;
    }
    lua_pop(L, 1);
  }
  return false;
}

std::string ReadTableString(lua_State* L, int table_idx, const char* name1,
                            const char* name2, std::string_view default_val) {
  if (GetTableField(L, table_idx, name1, name2)) {
    if (lua_isstring(L, -1)) {
      size_t len = 0;
      const char* s = lua_tolstring(L, -1, &len);
      std::string res(s, len);
      lua_pop(L, 1);
      return res;
    }
    lua_pop(L, 1);
  }
  return std::string(default_val);
}

void CopyStringField(lua_State* L, int table_idx, const char* name1,
                     const char* name2, LuaEmitter& emitter,
                     std::string_view dst_key, std::string_view default_val) {
  if (GetTableField(L, table_idx, name1, name2)) {
    if (lua_isstring(L, -1)) {
      size_t len = 0;
      const char* s = lua_tolstring(L, -1, &len);
      emitter.SetString(dst_key, std::string_view(s, len));
      lua_pop(L, 1);
      return;
    }
    lua_pop(L, 1);
  }
  if (!default_val.empty()) {
    emitter.SetString(dst_key, default_val);
  }
}

int64_t ReadTableInt64(lua_State* L, int table_idx, const char* name1,
                       const char* name2, int64_t default_val) {
  if (GetTableField(L, table_idx, name1, name2)) {
    int t = lua_type(L, -1);
    int64_t v = default_val;
    if (t == LUA_TNUMBER) {
      if (lua_isinteger(L, -1)) {
        v = lua_tointeger(L, -1);
      } else {
        LuaFloatToInt64(lua_tonumber(L, -1), v);
      }
    } else if (t == LUA_TSTRING) {
      ParseLuaStringInt64(L, -1, v);
    }
    lua_pop(L, 1);
    return v;
  }
  return default_val;
}

uint64_t ReadTableUint64(lua_State* L, int table_idx, const char* name1,
                         const char* name2, uint64_t default_val) {
  if (GetTableField(L, table_idx, name1, name2)) {
    int t = lua_type(L, -1);
    uint64_t v = default_val;
    if (t == LUA_TNUMBER) {
      if (lua_isinteger(L, -1)) {
        lua_Integer iv = lua_tointeger(L, -1);
        if (iv >= 0) {
          v = static_cast<uint64_t>(iv);
        }
      } else {
        LuaFloatToUint64(lua_tonumber(L, -1), v);
      }
    } else if (t == LUA_TSTRING) {
      ParseLuaStringUint64(L, -1, v);
    }
    lua_pop(L, 1);
    return v;
  }
  return default_val;
}

double ReadTableDouble(lua_State* L, int table_idx, const char* name1,
                       const char* name2, double default_val) {
  if (GetTableField(L, table_idx, name1, name2)) {
    int t = lua_type(L, -1);
    double v = default_val;
    if (t == LUA_TNUMBER) {
      double d = lua_tonumber(L, -1);
      if (std::isfinite(d)) {
        v = d;
      }
    } else if (t == LUA_TSTRING) {
      ParseLuaStringDouble(L, -1, v);
    }
    lua_pop(L, 1);
    return v;
  }
  return default_val;
}

int64_t ParseTableSeverityNumber(lua_State* L, int val_idx) {
  int abs_idx = lua_absindex(L, val_idx);
  int t = lua_type(L, abs_idx);
  if (t == LUA_TNUMBER) {
    if (lua_isinteger(L, abs_idx)) {
      return lua_tointeger(L, abs_idx);
    }
    int64_t v = 0;
    LuaFloatToInt64(lua_tonumber(L, abs_idx), v);
    return v;
  }
  if (t == LUA_TSTRING) {
    size_t len = 0;
    const char* s = lua_tolstring(L, abs_idx, &len);
    std::string_view sv(s, len);
    if (sv == "SEVERITY_NUMBER_TRACE") return 1;
    if (sv == "SEVERITY_NUMBER_TRACE2") return 2;
    if (sv == "SEVERITY_NUMBER_TRACE3") return 3;
    if (sv == "SEVERITY_NUMBER_TRACE4") return 4;
    if (sv == "SEVERITY_NUMBER_DEBUG") return 5;
    if (sv == "SEVERITY_NUMBER_DEBUG2") return 6;
    if (sv == "SEVERITY_NUMBER_DEBUG3") return 7;
    if (sv == "SEVERITY_NUMBER_DEBUG4") return 8;
    if (sv == "SEVERITY_NUMBER_INFO") return 9;
    if (sv == "SEVERITY_NUMBER_INFO2") return 10;
    if (sv == "SEVERITY_NUMBER_INFO3") return 11;
    if (sv == "SEVERITY_NUMBER_INFO4") return 12;
    if (sv == "SEVERITY_NUMBER_WARN") return 13;
    if (sv == "SEVERITY_NUMBER_WARN2") return 14;
    if (sv == "SEVERITY_NUMBER_WARN3") return 15;
    if (sv == "SEVERITY_NUMBER_WARN4") return 16;
    if (sv == "SEVERITY_NUMBER_ERROR") return 17;
    if (sv == "SEVERITY_NUMBER_ERROR2") return 18;
    if (sv == "SEVERITY_NUMBER_ERROR3") return 19;
    if (sv == "SEVERITY_NUMBER_ERROR4") return 20;
    if (sv == "SEVERITY_NUMBER_FATAL") return 21;
    if (sv == "SEVERITY_NUMBER_FATAL2") return 22;
    if (sv == "SEVERITY_NUMBER_FATAL3") return 23;
    if (sv == "SEVERITY_NUMBER_FATAL4") return 24;
    int64_t v = 0;
    if (ParseLuaStringInt64(L, abs_idx, v)) return v;
  }
  return 0;
}

uint64_t ReadTableTimestampNano(lua_State* L, int rec_idx) {
  uint64_t t_ns =
      ReadTableUint64(L, rec_idx, "timeUnixNano", "time_unix_nano", 0);
  if (t_ns == 0) {
    t_ns = ReadTableUint64(L, rec_idx, "observedTimeUnixNano",
                           "observed_time_unix_nano", 0);
  }
  return t_ns;
}

constexpr int kMaxLuaJsonDepth = 16;

void SerializeLuaValueToJson(lua_State* L, int val_idx, FastJsonWriter& writer,
                             int depth) {
  if (depth >= kMaxLuaJsonDepth) {
    writer.String("[max_depth_exceeded]");
    return;
  }
  if (!lua_checkstack(L, 6)) {
    writer.String("[out_of_stack]");
    return;
  }
  int abs_idx = lua_absindex(L, val_idx);
  int type = lua_type(L, abs_idx);

  if (type == LUA_TNIL) {
    writer.Null();
  } else if (type == LUA_TBOOLEAN) {
    writer.Bool(lua_toboolean(L, abs_idx) != 0);
  } else if (type == LUA_TNUMBER) {
    if (lua_isinteger(L, abs_idx)) {
      writer.Int64(lua_tointeger(L, abs_idx));
    } else {
      writer.Double(lua_tonumber(L, abs_idx));
    }
  } else if (type == LUA_TSTRING) {
    size_t len = 0;
    const char* s = lua_tolstring(L, abs_idx, &len);
    writer.String(std::string_view(s, len));
  } else if (type == LUA_TTABLE) {
    // Check if it's an OTLP AnyValue wrapper:
    if (GetTableField(L, abs_idx, "stringValue", "string_value")) {
      if (lua_type(L, -1) == LUA_TSTRING) {
        size_t len = 0;
        const char* s = lua_tolstring(L, -1, &len);
        writer.String(std::string_view(s, len));
      } else {
        writer.String("");
      }
      lua_pop(L, 1);
      return;
    }
    if (GetTableField(L, abs_idx, "intValue", "int_value")) {
      int64_t v = 0;
      if (lua_isinteger(L, -1)) {
        v = lua_tointeger(L, -1);
      } else if (lua_type(L, -1) == LUA_TNUMBER) {
        LuaFloatToInt64(lua_tonumber(L, -1), v);
      } else if (lua_type(L, -1) == LUA_TSTRING) {
        ParseLuaStringInt64(L, -1, v);
      }
      writer.Int64(v);
      lua_pop(L, 1);
      return;
    }
    if (GetTableField(L, abs_idx, "boolValue", "bool_value")) {
      writer.Bool(lua_toboolean(L, -1) != 0);
      lua_pop(L, 1);
      return;
    }
    if (GetTableField(L, abs_idx, "doubleValue", "double_value")) {
      double v = 0.0;
      if (lua_type(L, -1) == LUA_TNUMBER) {
        double d = lua_tonumber(L, -1);
        if (std::isfinite(d)) {
          v = d;
        }
      } else if (lua_type(L, -1) == LUA_TSTRING) {
        ParseLuaStringDouble(L, -1, v);
      }
      writer.Double(v);
      lua_pop(L, 1);
      return;
    }
    if (GetTableField(L, abs_idx, "bytesValue", "bytes_value")) {
      if (lua_type(L, -1) == LUA_TSTRING) {
        size_t len = 0;
        const char* s = lua_tolstring(L, -1, &len);
        writer.String(std::string_view(s, len));
      } else {
        writer.String("");
      }
      lua_pop(L, 1);
      return;
    }
    if (GetTableField(L, abs_idx, "arrayValue", "array_value")) {
      int arr_idx = lua_absindex(L, -1);
      if (lua_istable(L, arr_idx) && GetTableField(L, arr_idx, "values")) {
        int vals_idx = lua_absindex(L, -1);
        writer.StartArray();
        if (lua_istable(L, vals_idx)) {
          lua_Integer len = luaL_len(L, vals_idx);
          for (lua_Integer i = 1; i <= len; ++i) {
            lua_rawgeti(L, vals_idx, i);
            SerializeLuaValueToJson(L, -1, writer, depth + 1);
            lua_pop(L, 1);
          }
        }
        writer.EndArray();
        lua_pop(L, 1);  // pop values
      } else {
        writer.StartArray();
        writer.EndArray();
      }
      lua_pop(L, 1);  // pop arrayValue
      return;
    }
    if (GetTableField(L, abs_idx, "kvlistValue", "kvlist_value")) {
      int kv_idx = lua_absindex(L, -1);
      if (lua_istable(L, kv_idx) && GetTableField(L, kv_idx, "values")) {
        int vals_idx = lua_absindex(L, -1);
        writer.StartObject();
        if (lua_istable(L, vals_idx)) {
          lua_Integer len = luaL_len(L, vals_idx);
          for (lua_Integer i = 1; i <= len; ++i) {
            lua_rawgeti(L, vals_idx, i);
            int item_idx = lua_absindex(L, -1);
            if (lua_istable(L, item_idx)) {
              if (GetTableField(L, item_idx, "key")) {
                if (lua_type(L, -1) == LUA_TSTRING) {
                  size_t klen = 0;
                  const char* ks = lua_tolstring(L, -1, &klen);
                  writer.Key(std::string_view(ks, klen));
                } else {
                  writer.Key("");
                }
                lua_pop(L, 1);
              } else {
                writer.Key("");
              }
              if (GetTableField(L, item_idx, "value")) {
                SerializeLuaValueToJson(L, -1, writer, depth + 1);
                lua_pop(L, 1);
              } else {
                writer.Null();
              }
            }
            lua_pop(L, 1);  // pop item
          }
        }
        writer.EndObject();
        lua_pop(L, 1);  // pop values
      } else {
        writer.StartObject();
        writer.EndObject();
      }
      lua_pop(L, 1);  // pop kvlistValue
      return;
    }

    // Generic table: check if sequential array or map
    lua_Integer raw_len = luaL_len(L, abs_idx);
    bool is_seq_array = (raw_len > 0);
    if (is_seq_array) {
      lua_Integer count = 0;
      lua_pushnil(L);
      while (lua_next(L, abs_idx) != 0) {
        count++;
        if (lua_type(L, -2) != LUA_TNUMBER || !lua_isinteger(L, -2)) {
          is_seq_array = false;
          lua_pop(L, 2);
          break;
        }
        lua_Integer k = lua_tointeger(L, -2);
        if (k < 1 || k > raw_len) {
          is_seq_array = false;
          lua_pop(L, 2);
          break;
        }
        lua_pop(L, 1);
      }
      if (is_seq_array && count != raw_len) {
        is_seq_array = false;
      }
    }

    if (is_seq_array) {
      writer.StartArray();
      for (lua_Integer i = 1; i <= raw_len; ++i) {
        lua_rawgeti(L, abs_idx, i);
        SerializeLuaValueToJson(L, -1, writer, depth + 1);
        lua_pop(L, 1);
      }
      writer.EndArray();
    } else {
      writer.StartObject();
      lua_pushnil(L);
      while (lua_next(L, abs_idx) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
          size_t klen = 0;
          const char* ks = lua_tolstring(L, -2, &klen);
          writer.Key(std::string_view(ks, klen));
          SerializeLuaValueToJson(L, -1, writer, depth + 1);
        } else if (lua_type(L, -2) == LUA_TNUMBER) {
          lua_pushvalue(L, -2);
          size_t klen = 0;
          const char* ks = lua_tolstring(L, -1, &klen);
          writer.Key(std::string_view(ks, klen));
          lua_pop(L, 1);  // Pop duplicate string key
          SerializeLuaValueToJson(L, -1, writer, depth + 1);
        }
        lua_pop(L, 1);  // Pop value, preserve original key for next iteration
      }
      writer.EndObject();
    }
  } else {
    writer.Null();
  }
}

std::string ReadTableBodyString(lua_State* L, int rec_idx,
                                FastJsonWriter& writer) {
  if (GetTableField(L, rec_idx, "body")) {
    int body_idx = lua_absindex(L, -1);
    if (lua_isstring(L, body_idx)) {
      size_t len = 0;
      const char* s = lua_tolstring(L, body_idx, &len);
      std::string res(s, len);
      lua_pop(L, 1);
      return res;
    } else if (lua_istable(L, body_idx)) {
      if (GetTableField(L, body_idx, "stringValue", "string_value")) {
        if (lua_type(L, -1) == LUA_TSTRING) {
          size_t len = 0;
          const char* s = lua_tolstring(L, -1, &len);
          std::string res(s, len);
          lua_pop(L, 2);  // pop stringValue and body
          return res;
        }
        lua_pop(L, 1);  // pop non-string stringValue
      }
      writer.Reset();
      SerializeLuaValueToJson(L, body_idx, writer);
      lua_pop(L, 1);  // pop body
      return std::string(writer.Str());
    } else if (lua_isnil(L, body_idx)) {
      lua_pop(L, 1);
      return "";
    } else {
      writer.Reset();
      SerializeLuaValueToJson(L, body_idx, writer);
      lua_pop(L, 1);
      return std::string(writer.Str());
    }
  }
  return "";
}

void CopyTableBody(lua_State* L, int rec_idx, FastJsonWriter& writer,
                   LuaEmitter& emitter) {
  if (GetTableField(L, rec_idx, "body")) {
    int body_idx = lua_absindex(L, -1);
    if (lua_isstring(L, body_idx)) {
      size_t len = 0;
      const char* s = lua_tolstring(L, body_idx, &len);
      emitter.SetString("Body", std::string_view(s, len));
      lua_pop(L, 1);
      return;
    } else if (lua_istable(L, body_idx)) {
      if (GetTableField(L, body_idx, "stringValue", "string_value")) {
        if (lua_type(L, -1) == LUA_TSTRING) {
          size_t len = 0;
          const char* s = lua_tolstring(L, -1, &len);
          emitter.SetString("Body", std::string_view(s, len));
          lua_pop(L, 2);  // pop stringValue and body
          return;
        }
        lua_pop(L, 1);  // pop non-string stringValue
      }
      writer.Reset();
      SerializeLuaValueToJson(L, body_idx, writer);
      emitter.SetString("Body", writer.Str());
      lua_pop(L, 1);  // pop body
      return;
    } else if (lua_isnil(L, body_idx)) {
      lua_pop(L, 1);
      emitter.SetString("Body", "");
      return;
    } else {
      writer.Reset();
      SerializeLuaValueToJson(L, body_idx, writer);
      emitter.SetString("Body", writer.Str());
      lua_pop(L, 1);
      return;
    }
  }
  emitter.SetString("Body", "");
}

std::string ExtractTableServiceName(lua_State* L, int res_idx) {
  if (!GetTableField(L, res_idx, "attributes")) {
    return "unknown_service";
  }
  int attrs_idx = lua_absindex(L, -1);
  if (!lua_istable(L, attrs_idx)) {
    lua_pop(L, 1);
    return "unknown_service";
  }

  std::string service_name = "unknown_service";
  lua_Integer len = luaL_len(L, attrs_idx);

  if (len > 0) {
    // Array of { key = "...", value = ... }
    for (lua_Integer i = 1; i <= len; ++i) {
      lua_rawgeti(L, attrs_idx, i);
      if (lua_istable(L, -1)) {
        int item_idx = lua_absindex(L, -1);
        std::string k = ReadTableString(L, item_idx, "key");
        if (k == "service.name" || k == "service_name") {
          if (GetTableField(L, item_idx, "value")) {
            int val_idx = lua_absindex(L, -1);
            if (lua_type(L, val_idx) == LUA_TSTRING) {
              service_name = lua_tostring(L, val_idx);
            } else if (lua_istable(L, val_idx)) {
              std::string s =
                  ReadTableString(L, val_idx, "stringValue", "string_value");
              if (!s.empty()) service_name = std::move(s);
            }
            lua_pop(L, 1);
          }
          lua_pop(L, 1);
          break;
        }
      }
      lua_pop(L, 1);
    }
  } else {
    // Direct map: attrs["service.name"]
    if (GetTableField(L, attrs_idx, "service.name", "service_name")) {
      if (lua_type(L, -1) == LUA_TSTRING) {
        service_name = lua_tostring(L, -1);
      } else if (lua_istable(L, -1)) {
        std::string s = ReadTableString(L, -1, "stringValue", "string_value");
        if (!s.empty()) service_name = std::move(s);
      }
      lua_pop(L, 1);
    }
  }

  lua_pop(L, 1);  // pop attributes
  return service_name;
}

static void PushSingleAttributeValue(lua_State* L, int val_idx,
                                     FastJsonWriter& writer) {
  int abs_idx = lua_absindex(L, val_idx);
  int type = lua_type(L, abs_idx);

  if (type == LUA_TSTRING) {
    lua_pushvalue(L, abs_idx);
  } else if (type == LUA_TNUMBER) {
    lua_pushvalue(L, abs_idx);
  } else if (type == LUA_TBOOLEAN) {
    lua_pushvalue(L, abs_idx);
  } else if (type == LUA_TTABLE) {
    if (GetTableField(L, abs_idx, "stringValue", "string_value")) {
      if (lua_type(L, -1) == LUA_TSTRING) {
        return;  // value is already on stack
      }
      lua_pop(L, 1);
      lua_pushliteral(L, "");
      return;
    }
    if (GetTableField(L, abs_idx, "intValue", "int_value")) {
      if (lua_isinteger(L, -1)) {
        return;
      }
      int64_t v = 0;
      if (lua_type(L, -1) == LUA_TNUMBER) {
        LuaFloatToInt64(lua_tonumber(L, -1), v);
      } else if (lua_type(L, -1) == LUA_TSTRING) {
        ParseLuaStringInt64(L, -1, v);
      }
      lua_pop(L, 1);
      lua_pushinteger(L, v);
      return;
    }
    if (GetTableField(L, abs_idx, "boolValue", "bool_value")) {
      if (lua_type(L, -1) == LUA_TBOOLEAN) {
        return;
      }
      bool b = lua_toboolean(L, -1) != 0;
      lua_pop(L, 1);
      lua_pushboolean(L, b ? 1 : 0);
      return;
    }
    if (GetTableField(L, abs_idx, "doubleValue", "double_value")) {
      double d = 0.0;
      if (lua_type(L, -1) == LUA_TNUMBER) {
        double num = lua_tonumber(L, -1);
        if (std::isfinite(num)) {
          d = num;
        }
      } else if (lua_type(L, -1) == LUA_TSTRING) {
        ParseLuaStringDouble(L, -1, d);
      }
      lua_pop(L, 1);
      lua_pushnumber(L, d);
      return;
    }
    // Complex value -> serialise to JSON string
    writer.Reset();
    SerializeLuaValueToJson(L, abs_idx, writer);
    lua_pushlstring(L, writer.Str().data(), writer.Str().size());
  } else {
    lua_pushnil(L);
  }
}

void PushTableAttributes(lua_State* L, int attrs_idx, FastJsonWriter& writer) {
  int abs_idx = lua_absindex(L, attrs_idx);
  lua_newtable(L);
  int out_table_idx = lua_absindex(L, -1);

  if (!lua_istable(L, abs_idx)) {
    return;
  }

  bool is_kv_array = false;
  lua_Integer len = luaL_len(L, abs_idx);
  if (len > 0) {
    lua_rawgeti(L, abs_idx, 1);
    if (lua_istable(L, -1)) {
      if (GetTableField(L, -1, "key")) {
        if (lua_type(L, -1) == LUA_TSTRING) {
          is_kv_array = true;
        }
        lua_pop(L, 1);  // pop key field
      }
    }
    lua_pop(L, 1);  // pop item 1
  }

  if (is_kv_array) {
    // Array of { key = "...", value = ... }
    for (lua_Integer i = 1; i <= len; ++i) {
      lua_rawgeti(L, abs_idx, i);
      if (lua_istable(L, -1)) {
        int item_idx = lua_absindex(L, -1);
        if (GetTableField(L, item_idx, "key")) {
          if (lua_type(L, -1) == LUA_TSTRING && lua_rawlen(L, -1) > 0) {
            if (GetTableField(L, item_idx, "value")) {
              PushSingleAttributeValue(L, -1, writer);
              lua_remove(L,
                         -2);  // remove raw value, leaving [key, converted_val]
              lua_rawset(L, out_table_idx);
            } else {
              lua_pop(L, 1);  // pop key
            }
          } else {
            lua_pop(L, 1);  // pop key
          }
        }
      }
      lua_pop(L, 1);  // pop item
    }
  } else {
    // Direct key-value map
    lua_pushnil(L);
    while (lua_next(L, abs_idx) != 0) {
      if (lua_type(L, -2) == LUA_TSTRING) {
        lua_pushvalue(L, -2);
        PushSingleAttributeValue(L, -2, writer);
        lua_rawset(L, out_table_idx);
      } else if (lua_type(L, -2) == LUA_TNUMBER) {
        lua_pushvalue(L, -2);
        lua_tolstring(L, -1, nullptr);
        PushSingleAttributeValue(L, -2, writer);
        lua_rawset(L, out_table_idx);
      }
      lua_pop(L, 1);  // pop value, keep key for next iteration
    }
  }
}

static void CopyTableFieldToTable(lua_State* L, int src_idx, const char* name1,
                                  const char* name2, int dst_idx,
                                  const char* dst_field) {
  if (GetTableField(L, src_idx, name1, name2)) {
    if (lua_type(L, -1) == LUA_TSTRING) {
      lua_setfield(L, dst_idx, dst_field);
      return;
    }
    lua_pop(L, 1);
  }
  lua_pushliteral(L, "");
  lua_setfield(L, dst_idx, dst_field);
}

void PushTableSpanEvents(lua_State* L, int events_idx, FastJsonWriter& writer) {
  int abs_idx = lua_absindex(L, events_idx);
  lua_newtable(L);
  int out_arr_idx = lua_absindex(L, -1);

  if (!lua_istable(L, abs_idx)) return;

  lua_Integer len = luaL_len(L, abs_idx);
  for (lua_Integer i = 1; i <= len; ++i) {
    lua_rawgeti(L, abs_idx, i);
    if (lua_istable(L, -1)) {
      int item_idx = lua_absindex(L, -1);
      lua_newtable(L);
      int ev_obj_idx = lua_absindex(L, -1);

      uint64_t t_ns =
          ReadTableUint64(L, item_idx, "timeUnixNano", "time_unix_nano", 0);
      std::array<char, 32> ts_buf;
      FormatIso8601Nano(t_ns, ts_buf.data());
      constexpr size_t len = 30;
      std::string_view ts_sv(ts_buf.data(), len);
      lua_pushlstring(L, ts_sv.data(), ts_sv.size());
      lua_setfield(L, ev_obj_idx, "Timestamp");

      CopyTableFieldToTable(L, item_idx, "name", nullptr, ev_obj_idx, "Name");

      if (GetTableField(L, item_idx, "attributes")) {
        PushTableAttributes(L, -1, writer);
        lua_setfield(L, ev_obj_idx, "Attributes");
        lua_pop(L, 1);
      } else {
        lua_newtable(L);
        lua_setfield(L, ev_obj_idx, "Attributes");
      }

      lua_rawseti(L, out_arr_idx, i);
    }
    lua_pop(L, 1);
  }
}

void PushTableSpanLinks(lua_State* L, int links_idx, FastJsonWriter& writer) {
  int abs_idx = lua_absindex(L, links_idx);
  lua_newtable(L);
  int out_arr_idx = lua_absindex(L, -1);

  if (!lua_istable(L, abs_idx)) return;

  lua_Integer len = luaL_len(L, abs_idx);
  for (lua_Integer i = 1; i <= len; ++i) {
    lua_rawgeti(L, abs_idx, i);
    if (lua_istable(L, -1)) {
      int item_idx = lua_absindex(L, -1);
      lua_newtable(L);
      int link_obj_idx = lua_absindex(L, -1);

      CopyTableFieldToTable(L, item_idx, "traceId", "trace_id", link_obj_idx,
                            "TraceId");
      CopyTableFieldToTable(L, item_idx, "spanId", "span_id", link_obj_idx,
                            "SpanId");
      CopyTableFieldToTable(L, item_idx, "traceState", "trace_state",
                            link_obj_idx, "TraceState");

      if (GetTableField(L, item_idx, "attributes")) {
        PushTableAttributes(L, -1, writer);
        lua_setfield(L, link_obj_idx, "Attributes");
        lua_pop(L, 1);
      } else {
        lua_newtable(L);
        lua_setfield(L, link_obj_idx, "Attributes");
      }

      lua_rawseti(L, out_arr_idx, i);
    }
    lua_pop(L, 1);
  }
}

}  // namespace vector_otel::core
