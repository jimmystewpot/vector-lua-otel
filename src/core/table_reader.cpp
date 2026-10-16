#include "vector_otel/core/table_reader.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "vector_otel/core/fast_json_writer.hpp"
#include "vector_otel/core/timestamp.hpp"

namespace vector_otel {
namespace core {

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

int64_t ReadTableInt64(lua_State* L, int table_idx, const char* name1,
                       const char* name2, int64_t default_val) {
  if (GetTableField(L, table_idx, name1, name2)) {
    int t = lua_type(L, -1);
    if (t == LUA_TNUMBER) {
      if (lua_isinteger(L, -1)) {
        int64_t v = lua_tointeger(L, -1);
        lua_pop(L, 1);
        return v;
      }
      int64_t v = static_cast<int64_t>(lua_tonumber(L, -1));
      lua_pop(L, 1);
      return v;
    } else if (t == LUA_TSTRING) {
      const char* s = lua_tostring(L, -1);
      char* end = nullptr;
      int64_t v = std::strtoll(s, &end, 10);
      lua_pop(L, 1);
      return v;
    }
    lua_pop(L, 1);
  }
  return default_val;
}

uint64_t ReadTableUint64(lua_State* L, int table_idx, const char* name1,
                         const char* name2, uint64_t default_val) {
  if (GetTableField(L, table_idx, name1, name2)) {
    int t = lua_type(L, -1);
    if (t == LUA_TNUMBER) {
      if (lua_isinteger(L, -1)) {
        uint64_t v = static_cast<uint64_t>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        return v;
      }
      uint64_t v = static_cast<uint64_t>(lua_tonumber(L, -1));
      lua_pop(L, 1);
      return v;
    } else if (t == LUA_TSTRING) {
      const char* s = lua_tostring(L, -1);
      char* end = nullptr;
      uint64_t v = std::strtoull(s, &end, 10);
      lua_pop(L, 1);
      return v;
    }
    lua_pop(L, 1);
  }
  return default_val;
}

double ReadTableDouble(lua_State* L, int table_idx, const char* name1,
                       const char* name2, double default_val) {
  if (GetTableField(L, table_idx, name1, name2)) {
    int t = lua_type(L, -1);
    if (t == LUA_TNUMBER) {
      double v = lua_tonumber(L, -1);
      lua_pop(L, 1);
      return v;
    } else if (t == LUA_TSTRING) {
      const char* s = lua_tostring(L, -1);
      char* end = nullptr;
      double v = std::strtod(s, &end);
      lua_pop(L, 1);
      return v;
    }
    lua_pop(L, 1);
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
    return static_cast<int64_t>(lua_tonumber(L, abs_idx));
  }
  if (t == LUA_TSTRING) {
    const char* s = lua_tostring(L, abs_idx);
    if (std::strcmp(s, "SEVERITY_NUMBER_TRACE") == 0) return 1;
    if (std::strcmp(s, "SEVERITY_NUMBER_TRACE2") == 0) return 2;
    if (std::strcmp(s, "SEVERITY_NUMBER_TRACE3") == 0) return 3;
    if (std::strcmp(s, "SEVERITY_NUMBER_TRACE4") == 0) return 4;
    if (std::strcmp(s, "SEVERITY_NUMBER_DEBUG") == 0) return 5;
    if (std::strcmp(s, "SEVERITY_NUMBER_DEBUG2") == 0) return 6;
    if (std::strcmp(s, "SEVERITY_NUMBER_DEBUG3") == 0) return 7;
    if (std::strcmp(s, "SEVERITY_NUMBER_DEBUG4") == 0) return 8;
    if (std::strcmp(s, "SEVERITY_NUMBER_INFO") == 0) return 9;
    if (std::strcmp(s, "SEVERITY_NUMBER_INFO2") == 0) return 10;
    if (std::strcmp(s, "SEVERITY_NUMBER_INFO3") == 0) return 11;
    if (std::strcmp(s, "SEVERITY_NUMBER_INFO4") == 0) return 12;
    if (std::strcmp(s, "SEVERITY_NUMBER_WARN") == 0) return 13;
    if (std::strcmp(s, "SEVERITY_NUMBER_WARN2") == 0) return 14;
    if (std::strcmp(s, "SEVERITY_NUMBER_WARN3") == 0) return 15;
    if (std::strcmp(s, "SEVERITY_NUMBER_WARN4") == 0) return 16;
    if (std::strcmp(s, "SEVERITY_NUMBER_ERROR") == 0) return 17;
    if (std::strcmp(s, "SEVERITY_NUMBER_ERROR2") == 0) return 18;
    if (std::strcmp(s, "SEVERITY_NUMBER_ERROR3") == 0) return 19;
    if (std::strcmp(s, "SEVERITY_NUMBER_ERROR4") == 0) return 20;
    if (std::strcmp(s, "SEVERITY_NUMBER_FATAL") == 0) return 21;
    if (std::strcmp(s, "SEVERITY_NUMBER_FATAL2") == 0) return 22;
    if (std::strcmp(s, "SEVERITY_NUMBER_FATAL3") == 0) return 23;
    if (std::strcmp(s, "SEVERITY_NUMBER_FATAL4") == 0) return 24;
    char* end = nullptr;
    int64_t v = std::strtoll(s, &end, 10);
    if (end != s) return v;
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

void SerializeLuaValueToJson(lua_State* L, int val_idx,
                             FastJsonWriter& writer) {
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
      size_t len = 0;
      const char* s = lua_tolstring(L, -1, &len);
      writer.String(std::string_view(s, len));
      lua_pop(L, 1);
      return;
    }
    if (GetTableField(L, abs_idx, "intValue", "int_value")) {
      int64_t v = lua_isinteger(L, -1)
                      ? lua_tointeger(L, -1)
                      : std::strtoll(lua_tostring(L, -1), nullptr, 10);
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
      writer.Double(lua_tonumber(L, -1));
      lua_pop(L, 1);
      return;
    }
    if (GetTableField(L, abs_idx, "bytesValue", "bytes_value")) {
      size_t len = 0;
      const char* s = lua_tolstring(L, -1, &len);
      writer.String(std::string_view(s, len));
      lua_pop(L, 1);
      return;
    }
    if (GetTableField(L, abs_idx, "arrayValue", "array_value")) {
      int arr_idx = lua_absindex(L, -1);
      if (GetTableField(L, arr_idx, "values")) {
        int vals_idx = lua_absindex(L, -1);
        writer.StartArray();
        lua_Integer len = luaL_len(L, vals_idx);
        for (lua_Integer i = 1; i <= len; ++i) {
          lua_rawgeti(L, vals_idx, i);
          SerializeLuaValueToJson(L, -1, writer);
          lua_pop(L, 1);
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
      if (GetTableField(L, kv_idx, "values")) {
        int vals_idx = lua_absindex(L, -1);
        writer.StartObject();
        lua_Integer len = luaL_len(L, vals_idx);
        for (lua_Integer i = 1; i <= len; ++i) {
          lua_rawgeti(L, vals_idx, i);
          int item_idx = lua_absindex(L, -1);
          std::string k = ReadTableString(L, item_idx, "key", nullptr, "");
          writer.Key(k);
          if (GetTableField(L, item_idx, "value")) {
            SerializeLuaValueToJson(L, -1, writer);
            lua_pop(L, 1);
          } else {
            writer.Null();
          }
          lua_pop(L, 1);  // pop item
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

    // Generic table: check if array or map
    lua_Integer raw_len = luaL_len(L, abs_idx);
    if (raw_len > 0) {
      writer.StartArray();
      for (lua_Integer i = 1; i <= raw_len; ++i) {
        lua_rawgeti(L, abs_idx, i);
        SerializeLuaValueToJson(L, -1, writer);
        lua_pop(L, 1);
      }
      writer.EndArray();
    } else {
      writer.StartObject();
      lua_pushnil(L);
      while (lua_next(L, abs_idx) != 0) {
        if (lua_isstring(L, -2)) {
          size_t klen = 0;
          const char* ks = lua_tolstring(L, -2, &klen);
          writer.Key(std::string_view(ks, klen));
          SerializeLuaValueToJson(L, -1, writer);
        }
        lua_pop(L, 1);  // pop value, keep key for next
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
        size_t len = 0;
        const char* s = lua_tolstring(L, -1, &len);
        std::string res(s, len);
        lua_pop(L, 2);  // pop stringValue and body
        return res;
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
            if (lua_isstring(L, val_idx)) {
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
      if (lua_isstring(L, -1)) {
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
      return;  // value is already on stack
    }
    if (GetTableField(L, abs_idx, "intValue", "int_value")) {
      if (lua_isstring(L, -1)) {
        int64_t v = std::strtoll(lua_tostring(L, -1), nullptr, 10);
        lua_pop(L, 1);
        lua_pushinteger(L, v);
      }
      return;
    }
    if (GetTableField(L, abs_idx, "boolValue", "bool_value")) {
      return;
    }
    if (GetTableField(L, abs_idx, "doubleValue", "double_value")) {
      return;
    }
    // Complex value -> serialize to JSON string
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

  lua_Integer len = luaL_len(L, abs_idx);
  if (len > 0) {
    // Array of { key = "...", value = ... }
    for (lua_Integer i = 1; i <= len; ++i) {
      lua_rawgeti(L, abs_idx, i);
      if (lua_istable(L, -1)) {
        int item_idx = lua_absindex(L, -1);
        std::string key = ReadTableString(L, item_idx, "key");
        if (!key.empty()) {
          if (GetTableField(L, item_idx, "value")) {
            PushSingleAttributeValue(L, -1, writer);
            lua_setfield(L, out_table_idx, key.c_str());
            lua_pop(L, 1);  // pop value
          }
        }
      }
      lua_pop(L, 1);  // pop item
    }
  } else {
    // Direct key-value map
    lua_pushnil(L);
    while (lua_next(L, abs_idx) != 0) {
      if (lua_isstring(L, -2)) {
        size_t klen = 0;
        const char* ks = lua_tolstring(L, -2, &klen);
        std::string key(ks, klen);
        PushSingleAttributeValue(L, -1, writer);
        lua_setfield(L, out_table_idx, key.c_str());
      }
      lua_pop(L, 1);  // pop value, keep key
    }
  }
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
      char ts_buf[32];
      FormatIso8601Nano(t_ns, ts_buf);
      lua_pushlstring(L, ts_buf, 30);
      lua_setfield(L, ev_obj_idx, "Timestamp");

      std::string name = ReadTableString(L, item_idx, "name");
      lua_pushlstring(L, name.data(), name.size());
      lua_setfield(L, ev_obj_idx, "Name");

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

      std::string trace_id =
          ReadTableString(L, item_idx, "traceId", "trace_id");
      lua_pushlstring(L, trace_id.data(), trace_id.size());
      lua_setfield(L, link_obj_idx, "TraceId");

      std::string span_id = ReadTableString(L, item_idx, "spanId", "span_id");
      lua_pushlstring(L, span_id.data(), span_id.size());
      lua_setfield(L, link_obj_idx, "SpanId");

      std::string trace_state =
          ReadTableString(L, item_idx, "traceState", "trace_state");
      lua_pushlstring(L, trace_state.data(), trace_state.size());
      lua_setfield(L, link_obj_idx, "TraceState");

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

}  // namespace core
}  // namespace vector_otel
