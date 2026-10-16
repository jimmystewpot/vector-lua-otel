#include "vector_otel/extractors/logs_extractor.hpp"

#include <cstdint>
#include <string>
#include <string_view>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "vector_otel/core/fast_json_writer.hpp"
#include "vector_otel/core/lua_bridge.hpp"
#include "vector_otel/core/otlp_common.hpp"
#include "vector_otel/core/parser.hpp"
#include "vector_otel/core/table_reader.hpp"
#include "vector_otel/core/timestamp.hpp"

namespace vector_otel {
namespace extractors {

namespace {

using core::ScopedLuaRef;

void ExtractBodyString(simdjson::ondemand::value& body_val, core::FastJsonWriter& writer,
                       std::string& out_body) {
  auto type_res = body_val.type();
  if (type_res.error() != simdjson::SUCCESS) {
    out_body.clear();
    return;
  }
  auto type = type_res.value();
  if (type == simdjson::ondemand::json_type::object) {
    auto b_obj_res = body_val.get_object();
    if (b_obj_res.error() != simdjson::SUCCESS) {
      out_body.clear();
      return;
    }
    auto b_obj = b_obj_res.value();
    bool is_anyvalue_string = false;
    writer.Reset();
    writer.StartObject();
    for (auto field_res : b_obj) {
      if (field_res.error() != simdjson::SUCCESS) continue;
      auto k_res = field_res.unescaped_key();
      if (k_res.error() != simdjson::SUCCESS) continue;
      std::string_view k = k_res.value();
      auto fv_res = field_res.value();
      if (fv_res.error() != simdjson::SUCCESS) continue;
      auto fv = fv_res.value();

      if (k == "stringValue" || k == "string_value") {
        auto s_res = fv.get_string();
        if (s_res.error() == simdjson::SUCCESS) {
          out_body = std::string(s_res.value());
          is_anyvalue_string = true;
          break;
        }
      }
      writer.Key(k);
      auto raw_res = fv.raw_json();
      if (raw_res.error() == simdjson::SUCCESS) {
        writer.RawJson(raw_res.value());
      } else {
        writer.Null();
      }
    }
    if (!is_anyvalue_string) {
      writer.EndObject();
      out_body = std::string(writer.Str());
    }
  } else if (type == simdjson::ondemand::json_type::string) {
    auto s_res = body_val.get_string();
    if (s_res.error() == simdjson::SUCCESS) {
      out_body = std::string(s_res.value());
    } else {
      out_body.clear();
    }
  } else if (type == simdjson::ondemand::json_type::null) {
    out_body.clear();
  } else {
    auto raw_res = body_val.raw_json();
    if (raw_res.error() == simdjson::SUCCESS) {
      out_body = std::string(raw_res.value());
    } else {
      out_body.clear();
    }
  }
}

}  // namespace

ExtractionResult ProcessLogs(lua_State* L, std::string_view raw_json, int emit_idx) {
  static thread_local core::OtelParser parser;
  static thread_local core::FastJsonWriter writer;

  auto doc_res = parser.Iterate(raw_json);
  if (doc_res.error() != simdjson::SUCCESS) {
    return ExtractionResult{false, std::string(simdjson::error_message(doc_res.error())), 0};
  }

  auto doc = std::move(doc_res.value());
  auto doc_obj_res = doc.get_object();
  if (doc_obj_res.error() != simdjson::SUCCESS) {
    return ExtractionResult{false, std::string(simdjson::error_message(doc_obj_res.error())), 0};
  }
  auto doc_obj = doc_obj_res.value();

  char proc_buf[32];
  core::FormatIso8601Nano(core::CurrentTimeUnixNano(), proc_buf);
  std::string_view processed_at(proc_buf, 30);
  core::LuaEmitter emitter(L, emit_idx);

  auto rl_res = doc_obj.find_field_unordered("resourceLogs");
  if (rl_res.error() != simdjson::SUCCESS) {
    rl_res = doc_obj.find_field_unordered("resource_logs");
  }
  if (rl_res.error() != simdjson::SUCCESS) {
    emitter.Flush();
    return ExtractionResult{true, "", emitter.EmittedCount()};
  }

  auto rl_val = rl_res.value();
  auto rl_arr_res = rl_val.get_array();
  if (rl_arr_res.error() != simdjson::SUCCESS) {
    emitter.Flush();
    return ExtractionResult{true, "", emitter.EmittedCount()};
  }

  for (auto rl_elem_res : rl_arr_res.value()) {
    if (rl_elem_res.error() != simdjson::SUCCESS) {
      return ExtractionResult{false, std::string(simdjson::error_message(rl_elem_res.error())),
                              emitter.EmittedCount()};
    }
    auto rl_val_item = rl_elem_res.value();
    auto rl_obj_res = rl_val_item.get_object();
    if (rl_obj_res.error() != simdjson::SUCCESS) {
      continue;
    }
    auto rl_obj = rl_obj_res.value();

    // 1. Resource schema URL
    std::string resource_schema_url;
    auto res_schema_res = rl_obj.find_field_unordered("schemaUrl");
    if (res_schema_res.error() != simdjson::SUCCESS) {
      res_schema_res = rl_obj.find_field_unordered("schema_url");
    }
    if (res_schema_res.error() == simdjson::SUCCESS) {
      auto s_res = res_schema_res.value().get_string();
      if (s_res.error() == simdjson::SUCCESS) {
        resource_schema_url = std::string(s_res.value());
      }
    }

    // 2. Resource service name and attributes
    std::string_view service_name = "unknown_service";
    ScopedLuaRef res_attrs_scoped(L, LUA_NOREF);

    auto res_res = rl_obj.find_field_unordered("resource");
    if (res_res.error() == simdjson::SUCCESS) {
      auto r_val = res_res.value();
      auto r_obj_res = r_val.get_object();
      if (r_obj_res.error() == simdjson::SUCCESS) {
        auto r_obj = r_obj_res.value();
        service_name = core::ExtractServiceName(r_obj);
        if (L != nullptr) {
          auto attrs_res = r_obj.find_field_unordered("attributes");
          if (attrs_res.error() == simdjson::SUCCESS) {
            auto attrs_val = attrs_res.value();
            core::PushAttributesTable(L, attrs_val, writer);
          } else {
            lua_newtable(L);
          }
          res_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
        }
      }
    }
    if (L != nullptr && res_attrs_scoped.get() == LUA_NOREF) {
      lua_newtable(L);
      res_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
    }

    // 3. Iterate scopeLogs
    auto sl_res = rl_obj.find_field_unordered("scopeLogs");
    if (sl_res.error() != simdjson::SUCCESS) {
      sl_res = rl_obj.find_field_unordered("scope_logs");
    }
    if (sl_res.error() != simdjson::SUCCESS) {
      continue;
    }

    auto sl_val = sl_res.value();
    auto sl_arr_res = sl_val.get_array();
    if (sl_arr_res.error() != simdjson::SUCCESS) {
      continue;
    }

    for (auto sl_elem_res : sl_arr_res.value()) {
      if (sl_elem_res.error() != simdjson::SUCCESS) {
        return ExtractionResult{false, std::string(simdjson::error_message(sl_elem_res.error())),
                                emitter.EmittedCount()};
      }
      auto sl_val_item = sl_elem_res.value();
      auto sl_obj_res = sl_val_item.get_object();
      if (sl_obj_res.error() != simdjson::SUCCESS) {
        continue;
      }
      auto sl_obj = sl_obj_res.value();

      // Scope schema URL
      std::string scope_schema_url;
      auto sl_schema_res = sl_obj.find_field_unordered("schemaUrl");
      if (sl_schema_res.error() != simdjson::SUCCESS) {
        sl_schema_res = sl_obj.find_field_unordered("schema_url");
      }
      if (sl_schema_res.error() == simdjson::SUCCESS) {
        auto s_res = sl_schema_res.value().get_string();
        if (s_res.error() == simdjson::SUCCESS) {
          scope_schema_url = std::string(s_res.value());
        }
      }

      // Scope metadata
      std::string scope_name;
      std::string scope_version;
      ScopedLuaRef scope_attrs_scoped(L, LUA_NOREF);

      auto scope_res = sl_obj.find_field_unordered("scope");
      if (scope_res.error() == simdjson::SUCCESS) {
        auto scope_val = scope_res.value();
        auto s_obj_res = scope_val.get_object();
        if (s_obj_res.error() == simdjson::SUCCESS) {
          auto s_obj = s_obj_res.value();
          scope_name = std::string(core::ExtractStringField(s_obj, "name", ""));
          scope_version = std::string(core::ExtractStringField(s_obj, "version", ""));
          if (L != nullptr) {
            auto s_attrs_res = s_obj.find_field_unordered("attributes");
            if (s_attrs_res.error() == simdjson::SUCCESS) {
              auto s_attrs_val = s_attrs_res.value();
              core::PushAttributesTable(L, s_attrs_val, writer);
            } else {
              lua_newtable(L);
            }
            scope_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
          }
        }
      }
      if (L != nullptr && scope_attrs_scoped.get() == LUA_NOREF) {
        lua_newtable(L);
        scope_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
      }

      // 4. Iterate logRecords
      auto recs_res = sl_obj.find_field_unordered("logRecords");
      if (recs_res.error() != simdjson::SUCCESS) {
        recs_res = sl_obj.find_field_unordered("log_records");
      }
      if (recs_res.error() != simdjson::SUCCESS) {
        continue;
      }

      auto recs_val = recs_res.value();
      auto recs_arr_res = recs_val.get_array();
      if (recs_arr_res.error() != simdjson::SUCCESS) {
        continue;
      }

      for (auto rec_elem_res : recs_arr_res.value()) {
        if (rec_elem_res.error() != simdjson::SUCCESS) {
          return ExtractionResult{false, std::string(simdjson::error_message(rec_elem_res.error())),
                                  emitter.EmittedCount()};
        }
        auto rec_val_item = rec_elem_res.value();
        auto rec_obj_res = rec_val_item.get_object();
        if (rec_obj_res.error() != simdjson::SUCCESS) {
          continue;
        }
        auto rec_obj = rec_obj_res.value();

        emitter.StartRecord();

        // 1. Timestamp
        uint64_t t_ns = 0;
        auto t_res = rec_obj.find_field_unordered("timeUnixNano");
        if (t_res.error() != simdjson::SUCCESS) {
          t_res = rec_obj.find_field_unordered("time_unix_nano");
        }
        if (t_res.error() == simdjson::SUCCESS) {
          auto tv = t_res.value();
          t_ns = core::ParseUint64(tv);
        }
        if (t_ns == 0) {
          auto obs_res = rec_obj.find_field_unordered("observedTimeUnixNano");
          if (obs_res.error() != simdjson::SUCCESS) {
            obs_res = rec_obj.find_field_unordered("observed_time_unix_nano");
          }
          if (obs_res.error() == simdjson::SUCCESS) {
            auto obsv = obs_res.value();
            t_ns = core::ParseUint64(obsv);
          }
        }
        char ts_buf[32];
        core::FormatIso8601Nano(t_ns, ts_buf);
        emitter.SetString("Timestamp", std::string_view(ts_buf, 30));

        // 2. TraceId
        std::string_view trace_id = core::ExtractStringField(rec_obj, "traceId", "");
        if (trace_id.empty()) {
          trace_id = core::ExtractStringField(rec_obj, "trace_id", "");
        }
        emitter.SetString("TraceId", trace_id);

        // 3. SpanId
        std::string_view span_id = core::ExtractStringField(rec_obj, "spanId", "");
        if (span_id.empty()) {
          span_id = core::ExtractStringField(rec_obj, "span_id", "");
        }
        emitter.SetString("SpanId", span_id);

        // 4. TraceFlags
        int64_t flags = 0;
        auto flags_res = rec_obj.find_field_unordered("flags");
        if (flags_res.error() != simdjson::SUCCESS) {
          flags_res = rec_obj.find_field_unordered("traceFlags");
          if (flags_res.error() != simdjson::SUCCESS) {
            flags_res = rec_obj.find_field_unordered("trace_flags");
          }
        }
        if (flags_res.error() == simdjson::SUCCESS) {
          auto fv = flags_res.value();
          flags = core::ParseInt64(fv);
        }
        emitter.SetInt64("TraceFlags", flags);

        // 5. SeverityText
        std::string_view sev_text = core::ExtractStringField(rec_obj, "severityText", "");
        if (sev_text.empty()) {
          sev_text = core::ExtractStringField(rec_obj, "severity_text", "");
        }
        emitter.SetString("SeverityText", sev_text);

        // 6. SeverityNumber
        int64_t sev_num = 0;
        auto sev_num_res = rec_obj.find_field_unordered("severityNumber");
        if (sev_num_res.error() != simdjson::SUCCESS) {
          sev_num_res = rec_obj.find_field_unordered("severity_number");
        }
        if (sev_num_res.error() == simdjson::SUCCESS) {
          auto snv = sev_num_res.value();
          sev_num = core::ParseInt64(snv);
        }
        emitter.SetInt64("SeverityNumber", sev_num);

        // 7. ServiceName
        emitter.SetString("ServiceName", service_name);

        // 8. Body
        std::string body_str;
        auto body_res = rec_obj.find_field_unordered("body");
        if (body_res.error() == simdjson::SUCCESS) {
          auto body_val = body_res.value();
          ExtractBodyString(body_val, writer, body_str);
        }
        emitter.SetString("Body", body_str);

        // 9. ResourceSchemaUrl
        emitter.SetString("ResourceSchemaUrl", resource_schema_url);

        // 10. ResourceAttributes
        if (L != nullptr && res_attrs_scoped.get() != LUA_NOREF) {
          lua_rawgeti(L, LUA_REGISTRYINDEX, res_attrs_scoped.get());
          emitter.SetTableFromStack("ResourceAttributes");
        }

        // 11. ScopeSchemaUrl
        emitter.SetString("ScopeSchemaUrl", scope_schema_url);

        // 12. ScopeName
        emitter.SetString("ScopeName", scope_name);

        // 13. ScopeVersion
        emitter.SetString("ScopeVersion", scope_version);

        // 14. ScopeAttributes
        if (L != nullptr && scope_attrs_scoped.get() != LUA_NOREF) {
          lua_rawgeti(L, LUA_REGISTRYINDEX, scope_attrs_scoped.get());
          emitter.SetTableFromStack("ScopeAttributes");
        }

        // 15. LogAttributes
        if (L != nullptr) {
          auto log_attrs_res = rec_obj.find_field_unordered("attributes");
          if (log_attrs_res.error() == simdjson::SUCCESS) {
            auto log_attrs_val = log_attrs_res.value();
            core::PushAttributesTable(L, log_attrs_val, writer);
          } else {
            lua_newtable(L);
          }
          emitter.SetTableFromStack("LogAttributes");
        }

        // 16. EventName
        std::string_view event_name = core::ExtractStringField(rec_obj, "eventName", "");
        if (event_name.empty()) {
          event_name = core::ExtractStringField(rec_obj, "event_name", "");
        }
        emitter.SetString("EventName", event_name);

        // 17. processed_at
        emitter.SetString("processed_at", processed_at);

        emitter.EndRecord();
        if (emitter.HasError()) {
          return ExtractionResult{false, std::string(emitter.ErrorMessage()),
                                  emitter.EmittedCount()};
        }
      }
    }
  }

  emitter.Flush();
  if (emitter.HasError()) {
    return ExtractionResult{false, std::string(emitter.ErrorMessage()), emitter.EmittedCount()};
  }
  return ExtractionResult{true, "", emitter.EmittedCount()};
}

ExtractionResult ProcessLogsTable(lua_State* L, int table_idx, int emit_idx) {
  int abs_idx = lua_absindex(L, table_idx);
  if (!lua_istable(L, abs_idx)) {
    return ExtractionResult{false, "payload must be a table", 0};
  }

  int rl_arr_idx = 0;
  bool pop_rl = false;

  if (core::GetTableField(L, abs_idx, "resourceLogs", "resource_logs")) {
    rl_arr_idx = lua_absindex(L, -1);
    pop_rl = true;
  } else if (core::GetTableField(L, abs_idx, "log")) {
    int log_idx = lua_absindex(L, -1);
    if (core::GetTableField(L, log_idx, "resourceLogs", "resource_logs")) {
      rl_arr_idx = lua_absindex(L, -1);
      pop_rl = true;
    }
  }

  if (rl_arr_idx == 0) {
    rl_arr_idx = abs_idx;
  }

  if (!lua_istable(L, rl_arr_idx)) {
    if (pop_rl) lua_pop(L, 1);
    return ExtractionResult{true, "", 0};
  }

  static thread_local core::FastJsonWriter writer;
  std::string processed_at = core::FormatIso8601Nano(core::CurrentTimeUnixNano());
  core::LuaEmitter emitter(L, emit_idx);

  lua_Integer rl_len = luaL_len(L, rl_arr_idx);
  for (lua_Integer i = 1; i <= rl_len; ++i) {
    lua_rawgeti(L, rl_arr_idx, i);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      continue;
    }
    int rl_obj_idx = lua_absindex(L, -1);

    // 1. Resource schema URL
    std::string resource_schema_url =
        core::ReadTableString(L, rl_obj_idx, "schemaUrl", "schema_url");

    // 2. Resource service name & attributes
    std::string service_name = "unknown_service";
    ScopedLuaRef res_attrs_scoped(L, LUA_NOREF);

    if (core::GetTableField(L, rl_obj_idx, "resource")) {
      int res_idx = lua_absindex(L, -1);
      service_name = core::ExtractTableServiceName(L, res_idx);
      if (core::GetTableField(L, res_idx, "attributes")) {
        core::PushTableAttributes(L, -1, writer);
        res_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
        lua_pop(L, 1);  // pop attributes
      }
      lua_pop(L, 1);  // pop resource
    }
    if (res_attrs_scoped.get() == LUA_NOREF) {
      lua_newtable(L);
      res_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
    }

    // 3. Iterate scopeLogs
    if (!core::GetTableField(L, rl_obj_idx, "scopeLogs", "scope_logs")) {
      lua_pop(L, 1);  // pop rl_obj
      continue;
    }
    int sl_arr_idx = lua_absindex(L, -1);
    if (!lua_istable(L, sl_arr_idx)) {
      lua_pop(L, 2);  // pop sl_arr and rl_obj
      continue;
    }

    lua_Integer sl_len = luaL_len(L, sl_arr_idx);
    for (lua_Integer j = 1; j <= sl_len; ++j) {
      lua_rawgeti(L, sl_arr_idx, j);
      if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        continue;
      }
      int sl_obj_idx = lua_absindex(L, -1);

      std::string scope_schema_url =
          core::ReadTableString(L, sl_obj_idx, "schemaUrl", "schema_url");
      std::string scope_name;
      std::string scope_version;
      ScopedLuaRef scope_attrs_scoped(L, LUA_NOREF);

      if (core::GetTableField(L, sl_obj_idx, "scope")) {
        int s_idx = lua_absindex(L, -1);
        scope_name = core::ReadTableString(L, s_idx, "name");
        scope_version = core::ReadTableString(L, s_idx, "version");
        if (core::GetTableField(L, s_idx, "attributes")) {
          core::PushTableAttributes(L, -1, writer);
          scope_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
          lua_pop(L, 1);  // pop attributes
        }
        lua_pop(L, 1);  // pop scope
      }
      if (scope_attrs_scoped.get() == LUA_NOREF) {
        lua_newtable(L);
        scope_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
      }

      // 4. Iterate logRecords
      if (core::GetTableField(L, sl_obj_idx, "logRecords", "log_records")) {
        int recs_arr_idx = lua_absindex(L, -1);
        if (lua_istable(L, recs_arr_idx)) {
          lua_Integer recs_len = luaL_len(L, recs_arr_idx);
          for (lua_Integer k = 1; k <= recs_len; ++k) {
            lua_rawgeti(L, recs_arr_idx, k);
            if (!lua_istable(L, -1)) {
              lua_pop(L, 1);
              continue;
            }
            int rec_obj_idx = lua_absindex(L, -1);

            emitter.StartRecord();

            // 1. Timestamp
            uint64_t t_ns = core::ReadTableTimestampNano(L, rec_obj_idx);
            char ts_buf[32];
            core::FormatIso8601Nano(t_ns, ts_buf);
            emitter.SetString("Timestamp", std::string_view(ts_buf, 30));

            // 2. TraceId
            emitter.SetString("TraceId",
                              core::ReadTableString(L, rec_obj_idx, "traceId", "trace_id"));

            // 3. SpanId
            emitter.SetString("SpanId", core::ReadTableString(L, rec_obj_idx, "spanId", "span_id"));

            // 4. TraceFlags
            emitter.SetInt64("TraceFlags",
                             core::ReadTableInt64(L, rec_obj_idx, "flags", "traceFlags"));

            // 5. SeverityText
            emitter.SetString("SeverityText", core::ReadTableString(L, rec_obj_idx, "severityText",
                                                                    "severity_text"));

            // 6. SeverityNumber
            int64_t sev_num = 0;
            if (core::GetTableField(L, rec_obj_idx, "severityNumber", "severity_number")) {
              sev_num = core::ParseTableSeverityNumber(L, -1);
              lua_pop(L, 1);
            }
            emitter.SetInt64("SeverityNumber", sev_num);

            // 7. ServiceName
            emitter.SetString("ServiceName", service_name);

            // 8. Body
            emitter.SetString("Body", core::ReadTableBodyString(L, rec_obj_idx, writer));

            // 9. ResourceSchemaUrl
            emitter.SetString("ResourceSchemaUrl", resource_schema_url);

            // 10. ResourceAttributes
            if (res_attrs_scoped.get() != LUA_NOREF) {
              lua_rawgeti(L, LUA_REGISTRYINDEX, res_attrs_scoped.get());
              emitter.SetTableFromStack("ResourceAttributes");
            }

            // 11. ScopeSchemaUrl
            emitter.SetString("ScopeSchemaUrl", scope_schema_url);

            // 12. ScopeName
            emitter.SetString("ScopeName", scope_name);

            // 13. ScopeVersion
            emitter.SetString("ScopeVersion", scope_version);

            // 14. ScopeAttributes
            if (scope_attrs_scoped.get() != LUA_NOREF) {
              lua_rawgeti(L, LUA_REGISTRYINDEX, scope_attrs_scoped.get());
              emitter.SetTableFromStack("ScopeAttributes");
            }

            // 15. LogAttributes
            if (core::GetTableField(L, rec_obj_idx, "attributes")) {
              core::PushTableAttributes(L, -1, writer);
              emitter.SetTableFromStack("LogAttributes");
              lua_pop(L, 1);  // pop attributes
            } else {
              lua_newtable(L);
              emitter.SetTableFromStack("LogAttributes");
            }

            // 16. EventName
            emitter.SetString("EventName",
                              core::ReadTableString(L, rec_obj_idx, "eventName", "event_name"));

            // 17. processed_at
            emitter.SetString("processed_at", processed_at);

            emitter.EndRecord();
            lua_pop(L, 1);  // pop rec_obj
            if (emitter.HasError()) {
              if (pop_rl) lua_pop(L, 1);
              return ExtractionResult{false, std::string(emitter.ErrorMessage()),
                                      emitter.EmittedCount()};
            }
          }
        }
        lua_pop(L, 1);  // pop recs_arr
      }
      lua_pop(L, 1);  // pop sl_obj
    }
    lua_pop(L, 1);  // pop sl_arr
    lua_pop(L, 1);  // pop rl_obj
  }

  if (pop_rl) {
    lua_pop(L, 1);
  }

  emitter.Flush();
  if (emitter.HasError()) {
    return ExtractionResult{false, std::string(emitter.ErrorMessage()), emitter.EmittedCount()};
  }
  return ExtractionResult{true, "", emitter.EmittedCount()};
}

}  // namespace extractors
}  // namespace vector_otel
