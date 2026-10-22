#include "vector_otel/core/table_reader.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include "vector_otel/core/fast_json_writer.hpp"
#include "vector_otel/core/lua_bridge.hpp"
#include "vector_otel/core/otlp_common.hpp"
#include "vector_otel/core/scratch.hpp"
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
  while (!sv.empty() &&
         (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() &&
         (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
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
  if (std::isfinite(d) && d >= -9223372036854775808.0 && d < 9223372036854775808.0) {
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

bool GetTableFieldRaw(lua_State* L, int table_idx, const char* name1, const char* name2,
                      const char* name3) {
  int abs_idx = lua_absindex(L, table_idx);
  if (!lua_istable(L, abs_idx)) {
    return false;
  }
  if (name1 != nullptr) {
    lua_pushstring(L, name1);
    lua_rawget(L, abs_idx);
    if (!lua_isnil(L, -1)) {
      return true;
    }
    lua_pop(L, 1);
  }
  if (name2 != nullptr) {
    lua_pushstring(L, name2);
    lua_rawget(L, abs_idx);
    if (!lua_isnil(L, -1)) {
      return true;
    }
    lua_pop(L, 1);
  }
  if (name3 != nullptr) {
    lua_pushstring(L, name3);
    lua_rawget(L, abs_idx);
    if (!lua_isnil(L, -1)) {
      return true;
    }
    lua_pop(L, 1);
  }
  return false;
}

bool GetTableField(lua_State* L, int table_idx, const char* name1, const char* name2,
                   const char* name3) {
  return GetTableFieldRaw(L, table_idx, name1, name2, name3);
}

bool IsLuaTableEmpty(lua_State* L, int table_idx) noexcept {
  if (L == nullptr) {
    return true;
  }
  int abs_idx = lua_absindex(L, table_idx);
  if (!lua_istable(L, abs_idx)) {
    return true;
  }
  lua_pushnil(L);
  if (lua_next(L, abs_idx) == 0) {
    return true;
  }
  lua_pop(L, 2);
  return false;
}

std::string ReadTableString(lua_State* L, int table_idx, const char* name1, const char* name2,
                            std::string_view default_val) {
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

std::string_view ReadTableStringView(lua_State* L, int table_idx, const char* name1,
                                     const char* name2, std::string& scratch,
                                     std::string_view default_val) {
  if (GetTableFieldRaw(L, table_idx, name1, name2)) {
    int t = lua_type(L, -1);
    if (t == LUA_TSTRING) {
      size_t len = 0;
      const char* s = lua_tolstring(L, -1, &len);
      scratch.assign(s, len);
      lua_pop(L, 1);
      return std::string_view(scratch);
    }
    if (t == LUA_TNUMBER) {
      size_t len = 0;
      const char* s = lua_tolstring(L, -1, &len);
      scratch.assign(s, len);
      lua_pop(L, 1);
      return std::string_view(scratch);
    }
    lua_pop(L, 1);
  }
  return default_val;
}

void CopyStringField(lua_State* L, int table_idx, const char* name1, const char* name2,
                     LuaEmitter& emitter, std::string_view dst_key, std::string_view default_val) {
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

namespace {

std::string_view NormalizeIdOnStack(lua_State* L, int idx, size_t expected_binary_len,
                                    IdHexBuffer& id_buf) noexcept {
  if (lua_type(L, idx) != LUA_TSTRING) {
    return "";
  }
  size_t len = 0;
  const char* s = lua_tolstring(L, idx, &len);
  if (s == nullptr || len == 0) {
    return "";
  }
  return NormalizeOtelId(std::string_view(s, len), expected_binary_len, id_buf);
}

void CopyTableIdFieldToTable(lua_State* L, int src_idx, const char* name1, const char* name2,
                             const char* name3, size_t expected_binary_len, int dst_idx,
                             const char* dst_field) {
  IdHexBuffer id_buf{};
  std::string_view norm;
  if (GetTableField(L, src_idx, name1, name2, name3)) {
    norm = NormalizeIdOnStack(L, -1, expected_binary_len, id_buf);
    lua_pushlstring(L, norm.data(), norm.size());
    lua_setfield(L, dst_idx, dst_field);
    lua_pop(L, 1);
    return;
  }
  lua_pushliteral(L, "");
  lua_setfield(L, dst_idx, dst_field);
}

}  // namespace

void CopyIdField(lua_State* L, int table_idx, const char* name1, const char* name2,
                 size_t expected_binary_len, LuaEmitter& emitter, std::string_view dst_key,
                 const char* name3) {
  IdHexBuffer id_buf{};
  if (GetTableField(L, table_idx, name1, name2, name3)) {
    std::string_view norm = NormalizeIdOnStack(L, -1, expected_binary_len, id_buf);
    emitter.SetString(dst_key, norm);
    lua_pop(L, 1);
    return;
  }
  emitter.SetString(dst_key, "");
}

void CopyStringFieldAlways(lua_State* L, int table_idx, const char* name1, const char* name2,
                           LuaEmitter& emitter, std::string_view dst_key,
                           std::string_view default_val) {
  if (GetTableFieldRaw(L, table_idx, name1, name2)) {
    if (lua_isstring(L, -1)) {
      size_t len = 0;
      const char* s = lua_tolstring(L, -1, &len);
      emitter.SetString(dst_key, std::string_view(s, len));
      lua_pop(L, 1);
      return;
    }
    lua_pop(L, 1);
  }
  emitter.SetString(dst_key, default_val);
}

bool TryParseTableInt64At(lua_State* L, int val_idx, int64_t& out) {
  int abs_idx = lua_absindex(L, val_idx);
  int t = lua_type(L, abs_idx);
  if (t == LUA_TNUMBER) {
    if (lua_isinteger(L, abs_idx)) {
      out = lua_tointeger(L, abs_idx);
      return true;
    }
    return LuaFloatToInt64(lua_tonumber(L, abs_idx), out);
  }
  if (t == LUA_TSTRING) {
    return ParseLuaStringInt64(L, abs_idx, out);
  }
  return false;
}

bool TryParseTableUint64At(lua_State* L, int val_idx, uint64_t& out) {
  int abs_idx = lua_absindex(L, val_idx);
  int t = lua_type(L, abs_idx);
  if (t == LUA_TNUMBER) {
    if (lua_isinteger(L, abs_idx)) {
      lua_Integer iv = lua_tointeger(L, abs_idx);
      if (iv >= 0) {
        out = static_cast<uint64_t>(iv);
        return true;
      }
      return false;
    }
    return LuaFloatToUint64(lua_tonumber(L, abs_idx), out);
  }
  if (t == LUA_TSTRING) {
    return ParseLuaStringUint64(L, abs_idx, out);
  }
  return false;
}

bool TryParseTableDoubleAt(lua_State* L, int val_idx, double& out) {
  int abs_idx = lua_absindex(L, val_idx);
  int t = lua_type(L, abs_idx);
  if (t == LUA_TNUMBER) {
    double d = lua_tonumber(L, abs_idx);
    if (std::isfinite(d)) {
      out = d;
      return true;
    }
    return false;
  }
  if (t == LUA_TSTRING) {
    return ParseLuaStringDouble(L, abs_idx, out);
  }
  return false;
}

bool TryReadTableInt64(lua_State* L, int table_idx, const char* name1, const char* name2,
                       int64_t& out, const char* name3) {
  if (GetTableField(L, table_idx, name1, name2, name3)) {
    bool ok = TryParseTableInt64At(L, -1, out);
    lua_pop(L, 1);
    return ok;
  }
  return false;
}

bool TryReadTableUint64(lua_State* L, int table_idx, const char* name1, const char* name2,
                        uint64_t& out, const char* name3) {
  if (GetTableField(L, table_idx, name1, name2, name3)) {
    bool ok = TryParseTableUint64At(L, -1, out);
    lua_pop(L, 1);
    return ok;
  }
  return false;
}

bool TryReadTableDouble(lua_State* L, int table_idx, const char* name1, const char* name2,
                        double& out, const char* name3) {
  if (GetTableField(L, table_idx, name1, name2, name3)) {
    bool ok = TryParseTableDoubleAt(L, -1, out);
    lua_pop(L, 1);
    return ok;
  }
  return false;
}

int64_t ReadTableInt64(lua_State* L, int table_idx, const char* name1, const char* name2,
                       int64_t default_val, const char* name3) {
  int64_t v = default_val;
  if (TryReadTableInt64(L, table_idx, name1, name2, v, name3)) {
    return v;
  }
  return default_val;
}

uint64_t ReadTableUint64(lua_State* L, int table_idx, const char* name1, const char* name2,
                         uint64_t default_val, const char* name3) {
  uint64_t v = default_val;
  if (TryReadTableUint64(L, table_idx, name1, name2, v, name3)) {
    return v;
  }
  return default_val;
}

double ReadTableDouble(lua_State* L, int table_idx, const char* name1, const char* name2,
                       double default_val, const char* name3) {
  double v = default_val;
  if (TryReadTableDouble(L, table_idx, name1, name2, v, name3)) {
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
    return ParseSeverityNumberString(std::string_view(s, len));
  }
  return 0;
}

uint64_t ReadTableTimestampNano(lua_State* L, int rec_idx) {
  uint64_t t_ns = ReadTableUint64(L, rec_idx, "timeUnixNano", "time_unix_nano", 0);
  if (t_ns == 0) {
    t_ns = ReadTableUint64(L, rec_idx, "observedTimeUnixNano", "observed_time_unix_nano", 0);
  }
  return t_ns;
}

bool ReadLuaTimestampValue(lua_State* L, int val_idx, uint64_t& out_ns) noexcept {
  if (L == nullptr) {
    return false;
  }
  int abs_idx = lua_absindex(L, val_idx);
  int t = lua_type(L, abs_idx);
  if (t == LUA_TTABLE) {
    auto read_int_field = [&](const char* key, int64_t& out_v, bool required) noexcept -> bool {
      if (!GetTableFieldRaw(L, abs_idx, key)) {
        return !required;
      }
      bool ok = false;
      if (lua_isinteger(L, -1)) {
        out_v = static_cast<int64_t>(lua_tointeger(L, -1));
        ok = true;
      } else if (lua_type(L, -1) == LUA_TNUMBER) {
        ok = LuaFloatToInt64(lua_tonumber(L, -1), out_v);
      } else if (lua_type(L, -1) == LUA_TSTRING) {
        ok = ParseLuaStringInt64(L, -1, out_v);
      }
      lua_pop(L, 1);
      return ok;
    };
    int64_t year = 0;
    int64_t month = 0;
    int64_t day = 0;
    int64_t hour = 0;
    int64_t min = 0;
    int64_t sec = 0;
    int64_t nanosec = 0;
    if (!read_int_field("year", year, true) || !read_int_field("month", month, true) ||
        !read_int_field("day", day, true)) {
      return false;
    }
    if (!read_int_field("hour", hour, false) || !read_int_field("min", min, false) ||
        !read_int_field("sec", sec, false) || !read_int_field("nanosec", nanosec, false)) {
      return false;
    }
    return CivilToUnixNano(year, month, day, hour, min, sec, nanosec, out_ns);
  }
  if (t == LUA_TSTRING) {
    size_t len = 0;
    const char* s = lua_tolstring(L, abs_idx, &len);
    if (s == nullptr) {
      return false;
    }
    return ParseIso8601ToUnixNano(std::string_view(s, len), out_ns);
  }
  if (t == LUA_TNUMBER) {
    if (lua_isinteger(L, abs_idx)) {
      lua_Integer iv = lua_tointeger(L, abs_idx);
      if (iv < 0) {
        return false;
      }
      out_ns = static_cast<uint64_t>(iv);
      return true;
    }
    double d = lua_tonumber(L, abs_idx);
    return LuaFloatToUint64(d, out_ns);
  }
  return false;
}

bool ReadTableTimestampField(lua_State* L, int table_idx, const char* name1, const char* name2,
                             uint64_t& out_ns) noexcept {
  if (L == nullptr) {
    return false;
  }
  if (!GetTableFieldRaw(L, table_idx, name1, name2)) {
    return false;
  }
  const bool ok = ReadLuaTimestampValue(L, -1, out_ns);
  lua_pop(L, 1);
  return ok;
}

constexpr int kMaxLuaJsonDepth = 32;

static bool HasSingleLuaTableKey(lua_State* L, int abs_idx) noexcept {
  lua_pushnil(L);
  if (lua_next(L, abs_idx) == 0) {
    return false;
  }
  lua_pop(L, 1);  // pop first value, keep first key
  if (lua_next(L, abs_idx) != 0) {
    lua_pop(L, 2);  // pop second key and value
    return false;
  }
  return true;
}

void SerializeLuaValueToJson(lua_State* L, int val_idx, FastJsonWriter& writer, int depth) {
  if (depth >= kMaxLuaJsonDepth) {
    writer.String("[max_depth_exceeded]");
    return;
  }
  if (!lua_checkstack(L, 8)) {
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
    const lua_Integer raw_len = static_cast<lua_Integer>(lua_rawlen(L, abs_idx));
    if (raw_len == 0 && HasSingleLuaTableKey(L, abs_idx)) {
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
      if (GetTableField(L, abs_idx, "uintValue", "uint_value")) {
        uint64_t u = 0;
        if (TryParseTableUint64At(L, -1, u)) {
          writer.Uint64(u);
        } else {
          writer.Uint64(0);
        }
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
            lua_Integer len = static_cast<lua_Integer>(lua_rawlen(L, vals_idx));
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
            lua_Integer len = static_cast<lua_Integer>(lua_rawlen(L, vals_idx));
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
    }

    // Generic table: check if sequential array or map
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

std::string ReadTableBodyString(lua_State* L, int rec_idx, FastJsonWriter& writer) {
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

void CopyTableBody(lua_State* L, int rec_idx, FastJsonWriter& writer, LuaEmitter& emitter) {
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
  std::string scratch;
  return std::string(ExtractTableServiceNameView(L, res_idx, scratch));
}

std::string_view ExtractTableServiceNameView(lua_State* L, int res_idx, std::string& scratch) {
  constexpr std::string_view kDefaultService = "unknown_service";
  if (!GetTableFieldRaw(L, res_idx, "attributes")) {
    return kDefaultService;
  }
  int attrs_idx = lua_absindex(L, -1);
  if (!lua_istable(L, attrs_idx)) {
    lua_pop(L, 1);
    return kDefaultService;
  }

  std::string_view service_name = kDefaultService;
  size_t len = lua_rawlen(L, attrs_idx);

  if (len > 0) {
    // Array of { key = "...", value = ... }
    for (size_t i = 1; i <= len; ++i) {
      lua_rawgeti(L, attrs_idx, static_cast<lua_Integer>(i));
      if (lua_istable(L, -1)) {
        int item_idx = lua_absindex(L, -1);
        std::string_view k = ReadTableStringView(L, item_idx, "key", nullptr, scratch);
        if (k == "service.name" || k == "service_name") {
          if (GetTableFieldRaw(L, item_idx, "value")) {
            int val_idx = lua_absindex(L, -1);
            if (lua_type(L, val_idx) == LUA_TSTRING) {
              size_t vlen = 0;
              const char* s = lua_tolstring(L, val_idx, &vlen);
              scratch.assign(s, vlen);
              service_name = std::string_view(scratch);
            } else if (lua_istable(L, val_idx)) {
              std::string_view s =
                  ReadTableStringView(L, val_idx, "stringValue", "string_value", scratch);
              if (!s.empty()) service_name = s;
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
    if (GetTableFieldRaw(L, attrs_idx, "service.name", "service_name")) {
      if (lua_type(L, -1) == LUA_TSTRING) {
        size_t vlen = 0;
        const char* s = lua_tolstring(L, -1, &vlen);
        scratch.assign(s, vlen);
        service_name = std::string_view(scratch);
      } else if (lua_istable(L, -1)) {
        std::string_view s = ReadTableStringView(L, -1, "stringValue", "string_value", scratch);
        if (!s.empty()) service_name = s;
      }
      lua_pop(L, 1);
    }
  }

  lua_pop(L, 1);  // pop attributes
  return service_name;
}

// Returns true if the attribute was a plain string or OTLP stringValue.
static bool PushSingleAttributeValue(lua_State* L, int val_idx, FastJsonWriter& writer) {
  int abs_idx = lua_absindex(L, val_idx);
  int type = lua_type(L, abs_idx);

  if (type == LUA_TSTRING) {
    lua_pushvalue(L, abs_idx);
    return true;
  } else if (type == LUA_TNUMBER) {
    lua_pushvalue(L, abs_idx);
    return false;
  } else if (type == LUA_TBOOLEAN) {
    lua_pushvalue(L, abs_idx);
    return false;
  } else if (type == LUA_TTABLE) {
    if (GetTableFieldRaw(L, abs_idx, "stringValue", "string_value")) {
      if (lua_type(L, -1) == LUA_TSTRING) {
        return true;  // value is already on stack
      }
      lua_pop(L, 1);
      lua_pushliteral(L, "");
      return true;
    }
    if (GetTableFieldRaw(L, abs_idx, "intValue", "int_value")) {
      if (lua_isinteger(L, -1)) {
        return false;
      }
      int64_t v = 0;
      if (lua_type(L, -1) == LUA_TNUMBER) {
        LuaFloatToInt64(lua_tonumber(L, -1), v);
      } else if (lua_type(L, -1) == LUA_TSTRING) {
        ParseLuaStringInt64(L, -1, v);
      }
      lua_pop(L, 1);
      lua_pushinteger(L, v);
      return false;
    }
    if (GetTableFieldRaw(L, abs_idx, "uintValue", "uint_value")) {
      uint64_t u = 0;
      TryParseTableUint64At(L, -1, u);
      lua_pop(L, 1);
      constexpr uint64_t kMaxLuaInt =
          static_cast<uint64_t>(std::numeric_limits<lua_Integer>::max());
      lua_pushinteger(L, static_cast<lua_Integer>(std::min(u, kMaxLuaInt)));
      return false;
    }
    if (GetTableFieldRaw(L, abs_idx, "boolValue", "bool_value")) {
      if (lua_type(L, -1) == LUA_TBOOLEAN) {
        return false;
      }
      bool b = lua_toboolean(L, -1) != 0;
      lua_pop(L, 1);
      lua_pushboolean(L, b ? 1 : 0);
      return false;
    }
    if (GetTableFieldRaw(L, abs_idx, "doubleValue", "double_value")) {
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
      return false;
    }
    // Complex value -> serialise to JSON string
    writer.Reset();
    SerializeLuaValueToJson(L, abs_idx, writer);
    lua_pushlstring(L, writer.Str().data(), writer.Str().size());
    return false;
  } else {
    lua_pushnil(L);
    return false;
  }
}

void PushTableAttributes(lua_State* L, int attrs_idx, FastJsonWriter& writer,
                         std::string_view* out_service_name) {
  static thread_local std::string tls_attr_service_scratch;
  if (out_service_name != nullptr) {
    ResetScratchString(tls_attr_service_scratch);
    ReserveScratchString(tls_attr_service_scratch, 128);
  }
  if (!lua_checkstack(L, 10)) {
    throw std::runtime_error("Lua stack overflow in PushTableAttributes");
  }
  int abs_idx = lua_absindex(L, attrs_idx);
  lua_newtable(L);
  int out_table_idx = lua_absindex(L, -1);

  if (!lua_istable(L, abs_idx)) {
    return;
  }

  bool is_kv_array = false;
  const size_t len = lua_rawlen(L, abs_idx);
  if (len > 0) {
    lua_rawgeti(L, abs_idx, 1);
    if (lua_istable(L, -1)) {
      if (GetTableFieldRaw(L, -1, "key")) {
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
    for (size_t i = 1; i <= len; ++i) {
      lua_rawgeti(L, abs_idx, static_cast<lua_Integer>(i));
      if (lua_istable(L, -1)) {
        int item_idx = lua_absindex(L, -1);
        if (GetTableFieldRaw(L, item_idx, "key")) {
          if (lua_type(L, -1) == LUA_TSTRING && lua_rawlen(L, -1) > 0) {
            size_t klen = 0;
            const char* ks = lua_tolstring(L, -1, &klen);
            std::string_view key_sv(ks, klen);
            if (GetTableFieldRaw(L, item_idx, "value")) {
              bool is_str_attr = PushSingleAttributeValue(L, -1, writer);
              lua_remove(L,
                         -2);  // remove raw value, leaving [key, converted_val]
              if (out_service_name != nullptr &&
                  (out_service_name->empty() || *out_service_name == "unknown_service") &&
                  is_str_attr && (key_sv == "service.name" || key_sv == "service_name")) {
                size_t vlen = 0;
                const char* vs = lua_tolstring(L, -1, &vlen);
                if (vs != nullptr && vlen > 0) {
                  tls_attr_service_scratch.assign(vs, vlen);
                  *out_service_name = std::string_view(tls_attr_service_scratch);
                }
              }
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
    bool has_dot_svc = false;
    lua_pushnil(L);
    while (lua_next(L, abs_idx) != 0) {
      if (lua_type(L, -2) == LUA_TSTRING) {
        size_t klen = 0;
        const char* ks = lua_tolstring(L, -2, &klen);
        std::string_view key_sv(ks, klen);
        lua_pushvalue(L, -2);
        bool is_str_attr = PushSingleAttributeValue(L, -2, writer);
        if (out_service_name != nullptr && is_str_attr) {
          if (key_sv == "service.name" && !has_dot_svc) {
            size_t vlen = 0;
            const char* vs = lua_tolstring(L, -1, &vlen);
            if (vs != nullptr && vlen > 0) {
              tls_attr_service_scratch.assign(vs, vlen);
              *out_service_name = std::string_view(tls_attr_service_scratch);
              has_dot_svc = true;
            }
          } else if (key_sv == "service_name" && !has_dot_svc && tls_attr_service_scratch.empty()) {
            size_t vlen = 0;
            const char* vs = lua_tolstring(L, -1, &vlen);
            if (vs != nullptr && vlen > 0) {
              tls_attr_service_scratch.assign(vs, vlen);
              *out_service_name = std::string_view(tls_attr_service_scratch);
            }
          }
        }
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

static void CopyTableFieldToTable(lua_State* L, int src_idx, const char* name1, const char* name2,
                                  int dst_idx, const char* dst_field, const char* name3 = nullptr) {
  if (GetTableField(L, src_idx, name1, name2, name3)) {
    if (lua_type(L, -1) == LUA_TSTRING) {
      lua_setfield(L, dst_idx, dst_field);
      return;
    }
    lua_pop(L, 1);
  }
  lua_pushliteral(L, "");
  lua_setfield(L, dst_idx, dst_field);
}

static bool TryExtractScalarServiceName(lua_State* L, int val_idx, std::string& scratch) {
  int abs_idx = lua_absindex(L, val_idx);
  int t = lua_type(L, abs_idx);
  if (t == LUA_TSTRING) {
    size_t vlen = 0;
    const char* vs = lua_tolstring(L, abs_idx, &vlen);
    if (vs != nullptr && vlen > 0) {
      scratch.assign(vs, vlen);
      return true;
    }
    return false;
  }
  if (t == LUA_TTABLE && lua_rawlen(L, abs_idx) == 0 && HasSingleLuaTableKey(L, abs_idx)) {
    if (GetTableFieldRaw(L, abs_idx, "stringValue", "string_value")) {
      bool ok = false;
      if (lua_type(L, -1) == LUA_TSTRING) {
        size_t vlen = 0;
        const char* vs = lua_tolstring(L, -1, &vlen);
        if (vs != nullptr && vlen > 0) {
          scratch.assign(vs, vlen);
          ok = true;
        }
      }
      lua_pop(L, 1);
      return ok;
    }
  }
  return false;
}

static void UpdateTableServiceNameCandidate(lua_State* L, int val_idx, std::string_view key_sv,
                                            std::string_view* out_service_name,
                                            std::string& scratch, bool& has_dot_svc,
                                            bool& has_snake_svc) {
  if (out_service_name == nullptr) {
    return;
  }
  if (key_sv == "service.name") {
    if (TryExtractScalarServiceName(L, val_idx, scratch)) {
      *out_service_name = std::string_view(scratch);
      has_dot_svc = true;
    } else {
      has_dot_svc = false;
      if (!has_snake_svc) {
        *out_service_name = "unknown_service";
      }
    }
  } else if (key_sv == "service_name") {
    if (!has_dot_svc) {
      if (TryExtractScalarServiceName(L, val_idx, scratch)) {
        *out_service_name = std::string_view(scratch);
        has_snake_svc = true;
      } else {
        has_snake_svc = false;
        *out_service_name = "unknown_service";
      }
    } else {
      std::string dummy;
      has_snake_svc = TryExtractScalarServiceName(L, val_idx, dummy);
    }
  }
}

void WriteTableAttributesEntriesJson(lua_State* L, int attrs_idx, FastJsonWriter& writer,
                                     std::string_view* out_service_name) {
  static thread_local std::string tls_attr_json_service_scratch;
  if (out_service_name != nullptr) {
    ResetScratchString(tls_attr_json_service_scratch);
    ReserveScratchString(tls_attr_json_service_scratch, 128);
  }
  if (!lua_checkstack(L, 10)) {
    throw std::runtime_error("Lua stack overflow in WriteTableAttributesEntriesJson");
  }
  int abs_idx = lua_absindex(L, attrs_idx);
  if (!lua_istable(L, abs_idx)) {
    return;
  }

  bool is_kv_array = false;
  lua_Integer len = static_cast<lua_Integer>(lua_rawlen(L, abs_idx));
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

  bool has_dot_svc = false;
  bool has_snake_svc = false;

  if (is_kv_array) {
    for (lua_Integer i = 1; i <= len; ++i) {
      lua_rawgeti(L, abs_idx, i);
      if (lua_istable(L, -1)) {
        int item_idx = lua_absindex(L, -1);
        if (GetTableField(L, item_idx, "key")) {
          if (lua_type(L, -1) == LUA_TSTRING && lua_rawlen(L, -1) > 0) {
            size_t klen = 0;
            const char* ks = lua_tolstring(L, -1, &klen);
            std::string_view key_sv(ks, klen);
            if (GetTableField(L, item_idx, "value")) {
              if (out_service_name != nullptr &&
                  (key_sv == "service.name" || key_sv == "service_name")) {
                UpdateTableServiceNameCandidate(L, -1, key_sv, out_service_name,
                                                tls_attr_json_service_scratch, has_dot_svc,
                                                has_snake_svc);
              }
              writer.Key(key_sv);
              SerializeLuaValueToJson(L, -1, writer, 0);
              lua_pop(L, 1);  // pop value
            }
          }
          lua_pop(L, 1);  // pop key
        }
      }
      lua_pop(L, 1);  // pop item
    }
  } else {
    // Direct key-value map
    lua_pushnil(L);
    while (lua_next(L, abs_idx) != 0) {
      if (lua_type(L, -2) == LUA_TSTRING) {
        size_t klen = 0;
        const char* ks = lua_tolstring(L, -2, &klen);
        std::string_view key_sv(ks, klen);
        if (out_service_name != nullptr && (key_sv == "service.name" || key_sv == "service_name")) {
          UpdateTableServiceNameCandidate(L, -1, key_sv, out_service_name,
                                          tls_attr_json_service_scratch, has_dot_svc,
                                          has_snake_svc);
        }
        writer.Key(key_sv);
        SerializeLuaValueToJson(L, -1, writer, 0);
      } else if (lua_type(L, -2) == LUA_TNUMBER) {
        lua_pushvalue(L, -2);
        size_t klen = 0;
        const char* ks = lua_tolstring(L, -1, &klen);
        writer.Key(std::string_view(ks, klen));
        lua_pop(L, 1);  // pop converted string
        SerializeLuaValueToJson(L, -1, writer, 0);
      }
      lua_pop(L, 1);  // pop value, keep key for next iteration
    }
  }
}

void WriteTableAttributesJson(lua_State* L, int attrs_idx, FastJsonWriter& writer,
                              std::string_view* out_service_name) {
  writer.StartObject();
  WriteTableAttributesEntriesJson(L, attrs_idx, writer, out_service_name);
  writer.EndObject();
}

void SerializeTableSpanEventsToJson(lua_State* L, int events_idx, FastJsonWriter& writer) {
  if (!lua_checkstack(L, 10)) {
    throw std::runtime_error("Lua stack overflow in SerializeTableSpanEventsToJson");
  }
  int abs_idx = lua_absindex(L, events_idx);
  writer.StartArray();

  if (!lua_istable(L, abs_idx)) {
    writer.EndArray();
    return;
  }

  lua_Integer len = static_cast<lua_Integer>(lua_rawlen(L, abs_idx));
  for (lua_Integer i = 1; i <= len; ++i) {
    lua_rawgeti(L, abs_idx, i);
    if (lua_istable(L, -1)) {
      int item_idx = lua_absindex(L, -1);
      writer.StartObject();

      // Timestamp
      uint64_t t_ns = ReadTableUint64(L, item_idx, "timeUnixNano", "time_unix_nano", 0);
      std::array<char, 32> ts_buf;
      FormatIso8601Nano(t_ns, ts_buf.data());
      constexpr size_t kIsoLen = 30;
      writer.Key("Timestamp");
      writer.String(std::string_view(ts_buf.data(), kIsoLen));

      // Name
      writer.Key("Name");
      if (GetTableField(L, item_idx, "name", "Name")) {
        if (lua_type(L, -1) == LUA_TSTRING) {
          size_t nlen = 0;
          const char* ns = lua_tolstring(L, -1, &nlen);
          writer.String(std::string_view(ns, nlen));
        } else {
          writer.String("");
        }
        lua_pop(L, 1);
      } else {
        writer.String("");
      }

      // Attributes
      writer.Key("Attributes");
      if (GetTableField(L, item_idx, "attributes", "Attributes")) {
        WriteTableAttributesJson(L, -1, writer);
        lua_pop(L, 1);
      } else {
        writer.StartObject();
        writer.EndObject();
      }

      writer.EndObject();
    }
    lua_pop(L, 1);
  }

  writer.EndArray();
}

void SerializeTableSpanLinksToJson(lua_State* L, int links_idx, FastJsonWriter& writer) {
  if (!lua_checkstack(L, 10)) {
    throw std::runtime_error("Lua stack overflow in SerializeTableSpanLinksToJson");
  }
  int abs_idx = lua_absindex(L, links_idx);
  writer.StartArray();

  if (!lua_istable(L, abs_idx)) {
    writer.EndArray();
    return;
  }

  lua_Integer len = static_cast<lua_Integer>(lua_rawlen(L, abs_idx));
  for (lua_Integer i = 1; i <= len; ++i) {
    lua_rawgeti(L, abs_idx, i);
    if (lua_istable(L, -1)) {
      int item_idx = lua_absindex(L, -1);
      writer.StartObject();

      // TraceId
      IdHexBuffer id_buf{};
      writer.Key("TraceId");
      if (GetTableField(L, item_idx, "traceId", "trace_id", "TraceId")) {
        writer.String(NormalizeIdOnStack(L, -1, kTraceIdBinaryLen, id_buf));
        lua_pop(L, 1);
      } else {
        writer.String("");
      }

      // SpanId
      writer.Key("SpanId");
      if (GetTableField(L, item_idx, "spanId", "span_id", "SpanId")) {
        writer.String(NormalizeIdOnStack(L, -1, kSpanIdBinaryLen, id_buf));
        lua_pop(L, 1);
      } else {
        writer.String("");
      }

      // TraceState
      writer.Key("TraceState");
      if (GetTableField(L, item_idx, "traceState", "trace_state", "TraceState")) {
        if (lua_type(L, -1) == LUA_TSTRING) {
          size_t tslen = 0;
          const char* tss = lua_tolstring(L, -1, &tslen);
          writer.String(std::string_view(tss, tslen));
        } else {
          writer.String("");
        }
        lua_pop(L, 1);
      } else {
        writer.String("");
      }

      // Attributes
      writer.Key("Attributes");
      if (GetTableField(L, item_idx, "attributes", "Attributes")) {
        WriteTableAttributesJson(L, -1, writer);
        lua_pop(L, 1);
      } else {
        writer.StartObject();
        writer.EndObject();
      }

      writer.EndObject();
    }
    lua_pop(L, 1);
  }

  writer.EndArray();
}

void SerializeTableExemplarsToJson(lua_State* L, int exemplars_idx, FastJsonWriter& writer) {
  if (!lua_checkstack(L, 10)) {
    throw std::runtime_error("Lua stack overflow in SerializeTableExemplarsToJson");
  }
  int abs_idx = lua_absindex(L, exemplars_idx);
  writer.StartArray();

  if (!lua_istable(L, abs_idx)) {
    writer.EndArray();
    return;
  }

  lua_Integer len = static_cast<lua_Integer>(lua_rawlen(L, abs_idx));
  for (lua_Integer i = 1; i <= len; ++i) {
    lua_rawgeti(L, abs_idx, i);
    if (lua_istable(L, -1)) {
      int item_idx = lua_absindex(L, -1);
      writer.StartObject();

      // FilteredAttributes
      writer.Key("FilteredAttributes");
      if (GetTableField(L, item_idx, "filteredAttributes", "filtered_attributes",
                        "FilteredAttributes")) {
        WriteTableAttributesJson(L, -1, writer);
        lua_pop(L, 1);
      } else {
        writer.StartObject();
        writer.EndObject();
      }

      // TimeUnix
      writer.Key("TimeUnix");
      uint64_t t_ns = ReadTableUint64(L, item_idx, "timeUnixNano", "time_unix_nano", 0);
      if (t_ns > 0) {
        std::array<char, 32> ts_buf;
        FormatIso8601Nano(t_ns, ts_buf.data());
        constexpr size_t kIsoLen = 30;
        writer.String(std::string_view(ts_buf.data(), kIsoLen));
      } else if (GetTableField(L, item_idx, "TimeUnix", "timeUnix", "time_unix")) {
        if (lua_type(L, -1) == LUA_TSTRING) {
          size_t tslen = 0;
          const char* tss = lua_tolstring(L, -1, &tslen);
          writer.String(std::string_view(tss, tslen));
        } else {
          writer.String("");
        }
        lua_pop(L, 1);
      } else {
        writer.String("");
      }

      // Value
      bool value_set = false;
      if (GetTableField(L, item_idx, "asDouble", "as_double", "as_double_value")) {
        double d = 0.0;
        if (TryParseTableDoubleAt(L, -1, d)) {
          value_set = true;
        }
        lua_pop(L, 1);
        if (value_set) {
          writer.Key("Value");
          writer.Double(d);
        }
      }
      if (!value_set && GetTableField(L, item_idx, "asInt", "as_int", "as_int_value")) {
        int64_t v = 0;
        if (TryParseTableInt64At(L, -1, v)) {
          value_set = true;
        }
        lua_pop(L, 1);
        if (value_set) {
          writer.Key("Value");
          writer.Int64(v);
        }
      }
      if (!value_set && GetTableField(L, item_idx, "value", "Value")) {
        if (lua_type(L, -1) == LUA_TNUMBER) {
          if (lua_isinteger(L, -1)) {
            writer.Key("Value");
            writer.Int64(lua_tointeger(L, -1));
            value_set = true;
          } else {
            double num = lua_tonumber(L, -1);
            if (std::isfinite(num)) {
              writer.Key("Value");
              writer.Double(num);
              value_set = true;
            }
          }
        } else if (lua_type(L, -1) == LUA_TSTRING) {
          int64_t iv = 0;
          double d = 0.0;
          if (ParseLuaStringInt64(L, -1, iv)) {
            writer.Key("Value");
            writer.Int64(iv);
            value_set = true;
          } else if (ParseLuaStringDouble(L, -1, d)) {
            writer.Key("Value");
            writer.Double(d);
            value_set = true;
          }
        }
        lua_pop(L, 1);
      }

      // SpanId
      IdHexBuffer id_buf{};
      writer.Key("SpanId");
      if (GetTableField(L, item_idx, "spanId", "span_id", "SpanId")) {
        writer.String(NormalizeIdOnStack(L, -1, kSpanIdBinaryLen, id_buf));
        lua_pop(L, 1);
      } else {
        writer.String("");
      }

      // TraceId
      writer.Key("TraceId");
      if (GetTableField(L, item_idx, "traceId", "trace_id", "TraceId")) {
        writer.String(NormalizeIdOnStack(L, -1, kTraceIdBinaryLen, id_buf));
        lua_pop(L, 1);
      } else {
        writer.String("");
      }

      writer.EndObject();
    }
    lua_pop(L, 1);
  }

  writer.EndArray();
}

void PushTableExemplars(lua_State* L, int exemplars_idx, FastJsonWriter& writer) {
  if (!lua_checkstack(L, 10)) {
    throw std::runtime_error("Lua stack overflow in PushTableExemplars");
  }
  int abs_idx = lua_absindex(L, exemplars_idx);
  lua_newtable(L);
  int out_arr_idx = lua_absindex(L, -1);

  if (!lua_istable(L, abs_idx)) {
    return;
  }

  lua_Integer len = static_cast<lua_Integer>(lua_rawlen(L, abs_idx));
  lua_Integer out_idx = 1;
  for (lua_Integer i = 1; i <= len; ++i) {
    lua_rawgeti(L, abs_idx, i);
    if (lua_istable(L, -1)) {
      int item_idx = lua_absindex(L, -1);
      lua_newtable(L);
      int ex_obj_idx = lua_absindex(L, -1);

      // FilteredAttributes
      if (GetTableField(L, item_idx, "filteredAttributes", "filtered_attributes",
                        "FilteredAttributes")) {
        PushTableAttributes(L, -1, writer);
        lua_setfield(L, ex_obj_idx, "FilteredAttributes");
        lua_pop(L, 1);
      } else {
        lua_newtable(L);
        lua_setfield(L, ex_obj_idx, "FilteredAttributes");
      }

      // TimeUnix
      uint64_t t_ns = ReadTableUint64(L, item_idx, "timeUnixNano", "time_unix_nano", 0);
      if (t_ns > 0) {
        std::array<char, 32> ts_buf;
        FormatIso8601Nano(t_ns, ts_buf.data());
        constexpr size_t kIsoLen = 30;
        lua_pushlstring(L, ts_buf.data(), kIsoLen);
        lua_setfield(L, ex_obj_idx, "TimeUnix");
      } else if (GetTableField(L, item_idx, "TimeUnix", "timeUnix", "time_unix")) {
        if (lua_type(L, -1) == LUA_TSTRING) {
          lua_setfield(L, ex_obj_idx, "TimeUnix");
        } else {
          lua_pop(L, 1);
          lua_pushliteral(L, "");
          lua_setfield(L, ex_obj_idx, "TimeUnix");
        }
      } else {
        lua_pushliteral(L, "");
        lua_setfield(L, ex_obj_idx, "TimeUnix");
      }

      // Value
      bool value_set = false;
      if (GetTableField(L, item_idx, "asDouble", "as_double", "as_double_value")) {
        double d = 0.0;
        if (TryParseTableDoubleAt(L, -1, d)) {
          value_set = true;
        }
        lua_pop(L, 1);
        if (value_set) {
          lua_pushnumber(L, d);
          lua_setfield(L, ex_obj_idx, "Value");
        }
      }
      if (!value_set && GetTableField(L, item_idx, "asInt", "as_int", "as_int_value")) {
        int64_t v = 0;
        if (TryParseTableInt64At(L, -1, v)) {
          value_set = true;
        }
        lua_pop(L, 1);
        if (value_set) {
          lua_pushinteger(L, v);
          lua_setfield(L, ex_obj_idx, "Value");
        }
      }
      if (!value_set && GetTableField(L, item_idx, "value", "Value")) {
        if (lua_type(L, -1) == LUA_TNUMBER) {
          if (lua_isinteger(L, -1)) {
            lua_pushinteger(L, lua_tointeger(L, -1));
            lua_setfield(L, ex_obj_idx, "Value");
            value_set = true;
          } else {
            double num = lua_tonumber(L, -1);
            if (std::isfinite(num)) {
              lua_pushnumber(L, num);
              lua_setfield(L, ex_obj_idx, "Value");
              value_set = true;
            }
          }
        } else if (lua_type(L, -1) == LUA_TSTRING) {
          int64_t iv = 0;
          double d = 0.0;
          if (ParseLuaStringInt64(L, -1, iv)) {
            lua_pushinteger(L, iv);
            lua_setfield(L, ex_obj_idx, "Value");
            value_set = true;
          } else if (ParseLuaStringDouble(L, -1, d)) {
            lua_pushnumber(L, d);
            lua_setfield(L, ex_obj_idx, "Value");
            value_set = true;
          }
        }
        lua_pop(L, 1);
      }

      // SpanId
      CopyTableIdFieldToTable(L, item_idx, "spanId", "span_id", "SpanId", kSpanIdBinaryLen,
                              ex_obj_idx, "SpanId");

      // TraceId
      CopyTableIdFieldToTable(L, item_idx, "traceId", "trace_id", "TraceId", kTraceIdBinaryLen,
                              ex_obj_idx, "TraceId");

      lua_rawseti(L, out_arr_idx, out_idx++);
    }
    lua_pop(L, 1);
  }
}

void PushTableSpanEvents(lua_State* L, int events_idx, FastJsonWriter& writer) {
  if (!lua_checkstack(L, 10)) {
    throw std::runtime_error("Lua stack overflow in PushTableSpanEvents");
  }
  int abs_idx = lua_absindex(L, events_idx);
  lua_newtable(L);
  int out_arr_idx = lua_absindex(L, -1);

  if (!lua_istable(L, abs_idx)) return;

  lua_Integer len = static_cast<lua_Integer>(lua_rawlen(L, abs_idx));
  lua_Integer out_idx = 1;
  for (lua_Integer i = 1; i <= len; ++i) {
    lua_rawgeti(L, abs_idx, i);
    if (lua_istable(L, -1)) {
      int item_idx = lua_absindex(L, -1);
      lua_newtable(L);
      int ev_obj_idx = lua_absindex(L, -1);

      uint64_t t_ns = ReadTableUint64(L, item_idx, "timeUnixNano", "time_unix_nano", 0);
      std::array<char, 32> ts_buf;
      FormatIso8601Nano(t_ns, ts_buf.data());
      constexpr size_t kIsoLen = 30;
      std::string_view ts_sv(ts_buf.data(), kIsoLen);
      lua_pushlstring(L, ts_sv.data(), ts_sv.size());
      lua_setfield(L, ev_obj_idx, "Timestamp");

      CopyTableFieldToTable(L, item_idx, "name", "Name", ev_obj_idx, "Name");

      if (GetTableField(L, item_idx, "attributes", "Attributes")) {
        PushTableAttributes(L, -1, writer);
        lua_setfield(L, ev_obj_idx, "Attributes");
        lua_pop(L, 1);
      } else {
        lua_newtable(L);
        lua_setfield(L, ev_obj_idx, "Attributes");
      }

      lua_rawseti(L, out_arr_idx, out_idx++);
    }
    lua_pop(L, 1);
  }
}

void PushTableSpanLinks(lua_State* L, int links_idx, FastJsonWriter& writer) {
  if (!lua_checkstack(L, 10)) {
    throw std::runtime_error("Lua stack overflow in PushTableSpanLinks");
  }
  int abs_idx = lua_absindex(L, links_idx);
  lua_newtable(L);
  int out_arr_idx = lua_absindex(L, -1);

  if (!lua_istable(L, abs_idx)) return;

  lua_Integer len = static_cast<lua_Integer>(lua_rawlen(L, abs_idx));
  lua_Integer out_idx = 1;
  for (lua_Integer i = 1; i <= len; ++i) {
    lua_rawgeti(L, abs_idx, i);
    if (lua_istable(L, -1)) {
      int item_idx = lua_absindex(L, -1);
      lua_newtable(L);
      int link_obj_idx = lua_absindex(L, -1);

      CopyTableIdFieldToTable(L, item_idx, "traceId", "trace_id", "TraceId", kTraceIdBinaryLen,
                              link_obj_idx, "TraceId");
      CopyTableIdFieldToTable(L, item_idx, "spanId", "span_id", "SpanId", kSpanIdBinaryLen,
                              link_obj_idx, "SpanId");
      CopyTableFieldToTable(L, item_idx, "traceState", "trace_state", link_obj_idx, "TraceState",
                            "TraceState");

      if (GetTableField(L, item_idx, "attributes", "Attributes")) {
        PushTableAttributes(L, -1, writer);
        lua_setfield(L, link_obj_idx, "Attributes");
        lua_pop(L, 1);
      } else {
        lua_newtable(L);
        lua_setfield(L, link_obj_idx, "Attributes");
      }

      lua_rawseti(L, out_arr_idx, out_idx++);
    }
    lua_pop(L, 1);
  }
}

}  // namespace vector_otel::core
