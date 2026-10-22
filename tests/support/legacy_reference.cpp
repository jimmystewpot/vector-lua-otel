// Test-only reference implementations of the original (Lua-raising, table-building) attribute and
// span helpers. Production code stages rows through StagedRow instead; these remain solely as a
// behavioural oracle for the staged equivalents and are never linked into the modules.
#include "legacy_reference.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include "vector_otel/core/otlp_common.hpp"
#include "vector_otel/core/scratch.hpp"
#include "vector_otel/core/table_reader.hpp"
#include "vector_otel/core/timestamp.hpp"

namespace vector_otel::core {

namespace {
thread_local std::string tls_service_name_scratch;

// The string at idx, or nullptr (length 0) for every other type. Unlike lua_tolstring this never
// converts a number in place, so it can neither allocate nor raise.
const char* LuaStringOrNull(lua_State* L, int idx, size_t* len) noexcept {
  if (lua_type(L, idx) == LUA_TSTRING) {
    return lua_tolstring(L, idx, len);
  }
  if (len != nullptr) {
    *len = 0;
  }
  return nullptr;
}

bool PrepareNumericStringView(lua_State* L, int idx, std::string_view& out) {
  if (lua_type(L, idx) != LUA_TSTRING) {
    return false;
  }
  size_t len = 0;
  const char* s = LuaStringOrNull(L, idx, &len);
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

// Copies the scalar string on top of the stack into scratch; "unknown_service" otherwise.
std::string_view CaptureServiceName(lua_State* L, bool is_scalar_str) noexcept {
  if (!is_scalar_str) {
    return "unknown_service";
  }
  size_t slen = 0;
  const char* s = lua_tolstring(L, -1, &slen);
  if (s == nullptr || slen == 0) {
    return "unknown_service";
  }
  // Copy into thread-local scratch (preserves embedded NULs)
  ReserveScratchString(tls_service_name_scratch, 128);
  tls_service_name_scratch.assign(s, slen);
  return std::string_view(tls_service_name_scratch);
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

void CopyTableIdFieldToTable(lua_State* L, int src_idx, ReadKey name1, ReadKey name2, ReadKey name3,
                             size_t expected_binary_len, int dst_idx, const char* dst_field) {
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

namespace {

bool PushAnyValueScalar(lua_State* L, simdjson::ondemand::value& val, FastJsonWriter& json_writer) {
  if (L == nullptr) {
    return false;
  }
  auto type_res = val.type();
  if (type_res.error() != simdjson::SUCCESS) {
    lua_pushnil(L);
    return false;
  }
  auto type = type_res.value();
  if (type == simdjson::ondemand::json_type::object) {
    auto obj_res = val.get_object();
    if (obj_res.error() != simdjson::SUCCESS) {
      lua_pushnil(L);
      return false;
    }
    auto obj = obj_res.value();
    bool matched = false;
    bool is_scalar_string = false;
    bool has_plain_fields = false;
    for (auto field_res : obj) {
      if (field_res.error() != simdjson::SUCCESS) continue;
      auto k_res = field_res.unescaped_key();
      if (k_res.error() != simdjson::SUCCESS) continue;
      std::string_view k = k_res.value();

      auto val_res = field_res.value();
      if (val_res.error() != simdjson::SUCCESS) continue;
      auto fv = val_res.value();

      if (!has_plain_fields) {
        if (k == "stringValue" || k == "string_value") {
          auto s_res = fv.get_string();
          if (s_res.error() == simdjson::SUCCESS) {
            auto s = s_res.value();
            lua_pushlstring(L, s.data(), s.size());
            matched = true;
            is_scalar_string = true;
            break;
          }
        } else if (k == "intValue" || k == "int_value") {
          int64_t i = ParseInt64(fv);
          lua_pushinteger(L, static_cast<lua_Integer>(i));
          matched = true;
          break;
        } else if (k == "uintValue" || k == "uint_value") {
          uint64_t u = ParseUint64(fv);
          lua_Integer clamped = u > static_cast<uint64_t>(INT64_MAX)
                                    ? std::numeric_limits<lua_Integer>::max()
                                    : static_cast<lua_Integer>(u);
          lua_pushinteger(L, clamped);
          matched = true;
          break;
        } else if (k == "doubleValue" || k == "double_value") {
          lua_pushnumber(L, ParseDouble(fv));
          matched = true;
          break;
        } else if (k == "boolValue" || k == "bool_value") {
          auto b_res = fv.get_bool();
          if (b_res.error() == simdjson::SUCCESS) {
            lua_pushboolean(L, b_res.value() ? 1 : 0);
            matched = true;
            break;
          }
          lua_pushboolean(L, 0);
          matched = true;
          break;
        } else if (k == "arrayValue" || k == "array_value" || k == "kvlistValue" ||
                   k == "kvlist_value" || k == "bytesValue" || k == "bytes_value") {
          std::string_view raw_sv;
          auto raw_res = fv.raw_json();
          if (raw_res.error() == simdjson::SUCCESS) {
            raw_sv = raw_res.value();
          }
          json_writer.Reset();
          json_writer.StartObject();
          json_writer.Key(k);
          if (!raw_sv.empty()) {
            json_writer.RawJson(raw_sv);
          } else {
            json_writer.StartObject();
            json_writer.EndObject();
          }
          json_writer.EndObject();

          std::string_view sv = json_writer.Str();
          lua_pushlstring(L, sv.data(), sv.size());
          matched = true;
          break;
        }
      }

      if (!has_plain_fields) {
        json_writer.Reset();
        json_writer.StartObject();
        has_plain_fields = true;
      }
      json_writer.Key(k);
      auto raw_res = fv.raw_json();
      if (raw_res.error() == simdjson::SUCCESS) {
        std::string_view raw_fv = raw_res.value();
        while (!raw_fv.empty() && (raw_fv.back() == ' ' || raw_fv.back() == '\t' ||
                                   raw_fv.back() == '\r' || raw_fv.back() == '\n')) {
          raw_fv.remove_suffix(1);
        }
        json_writer.RawJson(raw_fv);
      } else {
        json_writer.Null();
      }
    }
    if (!matched) {
      if (has_plain_fields) {
        json_writer.EndObject();
        std::string_view sv = json_writer.Str();
        lua_pushlstring(L, sv.data(), sv.size());
      } else {
        lua_pushnil(L);
      }
    }
    return is_scalar_string;
  } else if (type == simdjson::ondemand::json_type::string) {
    auto s_res = val.get_string();
    if (s_res.error() == simdjson::SUCCESS) {
      auto s = s_res.value();
      lua_pushlstring(L, s.data(), s.size());
      return true;
    } else {
      lua_pushnil(L);
      return false;
    }
  } else if (type == simdjson::ondemand::json_type::number) {
    auto num_res = val.get_number();
    if (num_res.error() == simdjson::SUCCESS) {
      auto num = num_res.value();
      if (num.is_int64()) {
        lua_pushinteger(L, static_cast<lua_Integer>(num.get_int64()));
      } else if (num.is_uint64()) {
        uint64_t u = num.get_uint64();
        lua_Integer clamped = u > static_cast<uint64_t>(INT64_MAX)
                                  ? std::numeric_limits<lua_Integer>::max()
                                  : static_cast<lua_Integer>(u);
        lua_pushinteger(L, clamped);
      } else {
        lua_pushnumber(L, num.get_double());
      }
    } else {
      lua_pushnil(L);
    }
    return false;
  } else if (type == simdjson::ondemand::json_type::boolean) {
    auto b_res = val.get_bool();
    if (b_res.error() == simdjson::SUCCESS) {
      lua_pushboolean(L, b_res.value() ? 1 : 0);
    } else {
      lua_pushnil(L);
    }
    return false;
  } else if (type == simdjson::ondemand::json_type::array) {
    auto raw_res = val.raw_json();
    std::string_view raw_sv;
    if (raw_res.error() == simdjson::SUCCESS) {
      raw_sv = raw_res.value();
    }
    json_writer.Reset();
    if (!raw_sv.empty()) {
      json_writer.RawJson(raw_sv);
    } else {
      json_writer.StartArray();
      json_writer.EndArray();
    }
    std::string_view sv = json_writer.Str();
    lua_pushlstring(L, sv.data(), sv.size());
    return false;
  } else {
    lua_pushnil(L);
    return false;
  }
}

}  // namespace

void PushAnyValue(lua_State* L, simdjson::ondemand::value& val, FastJsonWriter& json_writer) {
  (void)PushAnyValueScalar(L, val, json_writer);
}

void PushAttributesTable(lua_State* L, simdjson::ondemand::value& attrs_val,
                         FastJsonWriter& json_writer, std::string_view* out_service_name) {
  if (L == nullptr) {
    return;
  }
  if (!lua_checkstack(L, 8)) {
    throw std::runtime_error("Lua stack overflow in PushAttributesTable");
  }
  lua_newtable(L);

  auto type_res = attrs_val.type();
  if (type_res.error() != simdjson::SUCCESS) {
    return;
  }
  auto type = type_res.value();

  if (type == simdjson::ondemand::json_type::array) {
    auto arr_res = attrs_val.get_array();
    if (arr_res.error() != simdjson::SUCCESS) {
      return;
    }
    for (auto elem_res : arr_res.value()) {
      if (elem_res.error() != simdjson::SUCCESS) continue;
      auto item_val = elem_res.value();
      auto obj_res = item_val.get_object();
      if (obj_res.error() != simdjson::SUCCESS) continue;
      auto kv_obj = obj_res.value();

      auto key_res = kv_obj.find_field_unordered("key");
      if (key_res.error() != simdjson::SUCCESS) continue;
      auto key_str_res = key_res.value().get_string();
      if (key_str_res.error() != simdjson::SUCCESS) continue;
      std::string_view k = key_str_res.value();

      auto val_res = kv_obj.find_field_unordered("value");
      if (val_res.error() != simdjson::SUCCESS) continue;
      auto v = val_res.value();

      lua_pushlstring(L, k.data(), k.size());
      const bool is_scalar_str = PushAnyValueScalar(L, v, json_writer);
      if (out_service_name != nullptr && (k == "service.name" || k == "service_name")) {
        *out_service_name = CaptureServiceName(L, is_scalar_str);
      }
      lua_rawset(L, -3);
    }
  } else if (type == simdjson::ondemand::json_type::object) {
    auto obj_res = attrs_val.get_object();
    if (obj_res.error() != simdjson::SUCCESS) {
      return;
    }
    for (auto field_res : obj_res.value()) {
      if (field_res.error() != simdjson::SUCCESS) continue;
      auto key_res = field_res.unescaped_key();
      if (key_res.error() != simdjson::SUCCESS) continue;
      std::string_view k = key_res.value();

      auto val_res = field_res.value();
      if (val_res.error() != simdjson::SUCCESS) continue;
      auto v = val_res.value();

      lua_pushlstring(L, k.data(), k.size());
      const bool is_scalar_str = PushAnyValueScalar(L, v, json_writer);
      if (out_service_name != nullptr && (k == "service.name" || k == "service_name")) {
        *out_service_name = CaptureServiceName(L, is_scalar_str);
      }
      lua_rawset(L, -3);
    }
  }
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
            const char* ks = LuaStringOrNull(L, -1, &klen);
            std::string_view key_sv(ks, klen);
            if (GetTableFieldRaw(L, item_idx, "value")) {
              bool is_str_attr = PushSingleAttributeValue(L, -1, writer);
              lua_remove(L,
                         -2);  // remove raw value, leaving [key, converted_val]
              if (out_service_name != nullptr &&
                  (out_service_name->empty() || *out_service_name == "unknown_service") &&
                  is_str_attr && (key_sv == "service.name" || key_sv == "service_name")) {
                size_t vlen = 0;
                const char* vs = LuaStringOrNull(L, -1, &vlen);
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
        const char* ks = LuaStringOrNull(L, -2, &klen);
        std::string_view key_sv(ks, klen);
        lua_pushvalue(L, -2);
        bool is_str_attr = PushSingleAttributeValue(L, -2, writer);
        if (out_service_name != nullptr && is_str_attr) {
          if (key_sv == "service.name" && !has_dot_svc) {
            size_t vlen = 0;
            const char* vs = LuaStringOrNull(L, -1, &vlen);
            if (vs != nullptr && vlen > 0) {
              tls_attr_service_scratch.assign(vs, vlen);
              *out_service_name = std::string_view(tls_attr_service_scratch);
              has_dot_svc = true;
            }
          } else if (key_sv == "service_name" && !has_dot_svc && tls_attr_service_scratch.empty()) {
            size_t vlen = 0;
            const char* vs = LuaStringOrNull(L, -1, &vlen);
            if (vs != nullptr && vlen > 0) {
              tls_attr_service_scratch.assign(vs, vlen);
              *out_service_name = std::string_view(tls_attr_service_scratch);
            }
          }
        }
        lua_rawset(L, out_table_idx);
      } else if (lua_type(L, -2) == LUA_TNUMBER) {
        lua_pushvalue(L, -2);
        lua_tolstring(L, -1, nullptr);  // legacy Push* path, removed once every signal is staged
        PushSingleAttributeValue(L, -2, writer);
        lua_rawset(L, out_table_idx);
      }
      lua_pop(L, 1);  // pop value, keep key for next iteration
    }
  }
}

namespace {
static void CopyTableFieldToTable(lua_State* L, int src_idx, ReadKey name1, ReadKey name2,
                                  int dst_idx, const char* dst_field, ReadKey name3 = nullptr) {
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
}  // namespace

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
