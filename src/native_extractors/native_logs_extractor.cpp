#include "vector_otel/native_extractors/native_logs_extractor.hpp"

#include <array>
#include <cstdint>
#include <exception>
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
#include "vector_otel/core/scratch.hpp"
#include "vector_otel/core/table_reader.hpp"
#include "vector_otel/core/timestamp.hpp"

namespace vector_otel::native_extractors {

namespace {

using core::ScopedLuaRef;
using extractors::ExtractionResult;

std::string_view ExtractBodyView(simdjson::ondemand::value& body_val,
                                 core::FastJsonWriter& writer) {
  auto type_res = body_val.type();
  if (type_res.error() != simdjson::SUCCESS) {
    return "";
  }
  auto type = type_res.value();
  if (type == simdjson::ondemand::json_type::object) {
    auto b_obj_res = body_val.get_object();
    if (b_obj_res.error() != simdjson::SUCCESS) {
      return "";
    }
    auto b_obj = b_obj_res.value();
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
          return s_res.value();
        }
      }
      writer.Key(k);
      auto raw_res = fv.raw_json();
      if (raw_res.error() == simdjson::SUCCESS) {
        writer.RawJson(core::TrimJsonWhitespace(raw_res.value()));
      } else {
        writer.Null();
      }
    }
    writer.EndObject();
    return writer.Str();
  } else if (type == simdjson::ondemand::json_type::string) {
    auto s_res = body_val.get_string();
    if (s_res.error() == simdjson::SUCCESS) {
      return s_res.value();
    }
    return "";
  } else if (type == simdjson::ondemand::json_type::null) {
    return "";
  } else {
    auto raw_res = body_val.raw_json();
    if (raw_res.error() == simdjson::SUCCESS) {
      return core::TrimJsonWhitespace(raw_res.value());
    }
    return "";
  }
}

int64_t ParseJsonSeverityNumber(simdjson::ondemand::value& fv) {
  auto type_res = fv.type();
  if (type_res.error() == simdjson::SUCCESS &&
      type_res.value() == simdjson::ondemand::json_type::string) {
    auto s_res = fv.get_string();
    if (s_res.error() == simdjson::SUCCESS) {
      std::string_view sv = s_res.value();
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
    }
  }
  return core::ParseInt64(fv);
}

bool ExtractNativeLogObjectJson(lua_State* L, simdjson::ondemand::object& rec_obj,
                                std::string_view processed_at, core::FastJsonWriter& writer,
                                core::FastJsonWriter& body_writer, core::LuaEmitter& emitter,
                                std::string& err_out, int depth = 0, bool* out_matched = nullptr) {
  uint64_t t_ns = 0;
  uint64_t obs_t_ns = 0;
  bool has_ts = false;
  bool has_obs_ts = false;
  std::string_view trace_id;
  std::string_view span_id;
  int64_t flags = 0;
  std::string_view sev_text;
  int64_t sev_num = 0;
  std::string_view res_service_name = "unknown_service";
  std::string_view top_service_name;
  std::string_view body_sv;
  bool has_message = false;
  std::string_view resource_schema_url;
  std::string_view scope_schema_url;
  std::string_view scope_name;
  std::string_view scope_version;
  std::string_view event_name;
  ScopedLuaRef res_attrs_scoped(L, LUA_NOREF);
  ScopedLuaRef scope_attrs_scoped(L, LUA_NOREF);
  ScopedLuaRef log_attrs_scoped(L, LUA_NOREF);
  bool has_recognized_field = false;

  for (auto field_res : rec_obj) {
    if (field_res.error() != simdjson::SUCCESS) {
      err_out = std::string(simdjson::error_message(field_res.error()));
      return false;
    }
    auto k_res = field_res.unescaped_key();
    if (k_res.error() != simdjson::SUCCESS) {
      err_out = std::string(simdjson::error_message(k_res.error()));
      return false;
    }
    std::string_view k = k_res.value();
    auto fv_res = field_res.value();
    if (fv_res.error() != simdjson::SUCCESS) {
      err_out = std::string(simdjson::error_message(fv_res.error()));
      return false;
    }
    auto fv = fv_res.value();

    if (k == "log" && depth == 0) {
      auto t_res = fv.type();
      if (t_res.error() != simdjson::SUCCESS) {
        err_out = std::string(simdjson::error_message(t_res.error()));
        return false;
      }
      if (t_res.value() == simdjson::ondemand::json_type::object) {
        auto inner_obj_res = fv.get_object();
        if (inner_obj_res.error() != simdjson::SUCCESS) {
          err_out = std::string(simdjson::error_message(inner_obj_res.error()));
          return false;
        }
        auto inner_obj = inner_obj_res.value();
        bool inner_matched = false;
        if (!ExtractNativeLogObjectJson(L, inner_obj, processed_at, writer, body_writer, emitter,
                                        err_out, depth + 1, &inner_matched)) {
          return false;
        }
        if (inner_matched) {
          if (out_matched != nullptr) *out_matched = true;
          return true;
        }
        continue;
      } else if (t_res.value() == simdjson::ondemand::json_type::array) {
        auto inner_arr_res = fv.get_array();
        if (inner_arr_res.error() != simdjson::SUCCESS) {
          err_out = std::string(simdjson::error_message(inner_arr_res.error()));
          return false;
        }
        bool any_matched = false;
        for (auto elem_res : inner_arr_res.value()) {
          if (elem_res.error() != simdjson::SUCCESS) {
            err_out = std::string(simdjson::error_message(elem_res.error()));
            return false;
          }
          auto elem_obj_res = elem_res.value().get_object();
          if (elem_obj_res.error() != simdjson::SUCCESS) {
            err_out = std::string(simdjson::error_message(elem_obj_res.error()));
            return false;
          }
          auto elem_obj = elem_obj_res.value();
          bool elem_matched = false;
          if (!ExtractNativeLogObjectJson(L, elem_obj, processed_at, writer, body_writer, emitter,
                                          err_out, depth + 1, &elem_matched)) {
            return false;
          }
          any_matched = any_matched || elem_matched;
        }
        if (any_matched) {
          if (out_matched != nullptr) *out_matched = true;
          return true;
        }
        continue;
      }
      continue;
    }

    if (k == "timestamp" || k == "timeUnixNano" || k == "time_unix_nano") {
      has_recognized_field = true;
      uint64_t parsed = 0;
      if (core::ParseJsonTimestamp(fv, parsed)) {
        t_ns = parsed;
        has_ts = true;
      }
    } else if (k == "observed_timestamp" || k == "observedTimeUnixNano" ||
               k == "observed_time_unix_nano") {
      has_recognized_field = true;
      uint64_t parsed = 0;
      if (core::ParseJsonTimestamp(fv, parsed)) {
        obs_t_ns = parsed;
        has_obs_ts = true;
      }
    } else if (k == "trace_id" || k == "traceId") {
      has_recognized_field = true;
      auto s_res = fv.get_string();
      if (s_res.error() == simdjson::SUCCESS) {
        trace_id = s_res.value();
      }
    } else if (k == "span_id" || k == "spanId") {
      has_recognized_field = true;
      auto s_res = fv.get_string();
      if (s_res.error() == simdjson::SUCCESS) {
        span_id = s_res.value();
      }
    } else if (k == "flags" || k == "trace_flags" || k == "traceFlags") {
      has_recognized_field = true;
      flags = core::ParseInt64(fv);
    } else if (k == "severity_text" || k == "severityText") {
      has_recognized_field = true;
      auto s_res = fv.get_string();
      if (s_res.error() == simdjson::SUCCESS) {
        sev_text = s_res.value();
      }
    } else if (k == "severity_number" || k == "severityNumber") {
      has_recognized_field = true;
      sev_num = ParseJsonSeverityNumber(fv);
    } else if (k == "message") {
      has_recognized_field = true;
      body_sv = ExtractBodyView(fv, body_writer);
      has_message = true;
    } else if (k == "body") {
      has_recognized_field = true;
      if (!has_message) {
        body_sv = ExtractBodyView(fv, body_writer);
      }
    } else if (k == "resources" || k == "resource_attributes" || k == "resourceAttributes" ||
               k == "resource") {
      has_recognized_field = true;
      if (L != nullptr) {
        res_service_name = "unknown_service";
        core::PushAttributesTable(L, fv, writer, &res_service_name);
        res_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
      }
    } else if (k == "resource_schema_url" || k == "resourceSchemaUrl" || k == "schema_url" ||
               k == "schemaUrl") {
      has_recognized_field = true;
      auto s_res = fv.get_string();
      if (s_res.error() == simdjson::SUCCESS) {
        resource_schema_url = s_res.value();
      }
    } else if (k == "scope") {
      has_recognized_field = true;
      auto s_obj_res = fv.get_object();
      if (s_obj_res.error() == simdjson::SUCCESS) {
        for (auto sf_res : s_obj_res.value()) {
          if (sf_res.error() != simdjson::SUCCESS) continue;
          auto sk_res = sf_res.unescaped_key();
          if (sk_res.error() != simdjson::SUCCESS) continue;
          std::string_view sk = sk_res.value();
          auto sfv_res = sf_res.value();
          if (sfv_res.error() != simdjson::SUCCESS) continue;
          auto sfv = sfv_res.value();

          if (sk == "name") {
            auto s_res = sfv.get_string();
            if (s_res.error() == simdjson::SUCCESS) {
              scope_name = s_res.value();
            }
          } else if (sk == "version") {
            auto s_res = sfv.get_string();
            if (s_res.error() == simdjson::SUCCESS) {
              scope_version = s_res.value();
            }
          } else if (sk == "schema_url" || sk == "schemaUrl") {
            auto s_res = sfv.get_string();
            if (s_res.error() == simdjson::SUCCESS) {
              scope_schema_url = s_res.value();
            }
          } else if (sk == "attributes" && L != nullptr) {
            core::PushAttributesTable(L, sfv, writer);
            scope_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
          }
        }
      }
    } else if (k == "scope_schema_url" || k == "scopeSchemaUrl") {
      has_recognized_field = true;
      auto s_res = fv.get_string();
      if (s_res.error() == simdjson::SUCCESS && scope_schema_url.empty()) {
        scope_schema_url = s_res.value();
      }
    } else if (k == "scope_name" || k == "scopeName") {
      has_recognized_field = true;
      auto s_res = fv.get_string();
      if (s_res.error() == simdjson::SUCCESS && scope_name.empty()) {
        scope_name = s_res.value();
      }
    } else if (k == "scope_version" || k == "scopeVersion") {
      has_recognized_field = true;
      auto s_res = fv.get_string();
      if (s_res.error() == simdjson::SUCCESS && scope_version.empty()) {
        scope_version = s_res.value();
      }
    } else if (k == "scope_attributes" || k == "scopeAttributes") {
      has_recognized_field = true;
      if (L != nullptr && scope_attrs_scoped.get() == LUA_NOREF) {
        core::PushAttributesTable(L, fv, writer);
        scope_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
      }
    } else if (k == "attributes" || k == "log_attributes" || k == "logAttributes") {
      has_recognized_field = true;
      if (L != nullptr) {
        core::PushAttributesTable(L, fv, writer);
        log_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
      }
    } else if (k == "event_name" || k == "eventName") {
      has_recognized_field = true;
      auto s_res = fv.get_string();
      if (s_res.error() == simdjson::SUCCESS) {
        event_name = s_res.value();
      }
    } else if (k == "service_name" || k == "service") {
      has_recognized_field = true;
      auto s_res = fv.get_string();
      if (s_res.error() == simdjson::SUCCESS) {
        top_service_name = s_res.value();
      }
    }
  }

  if (!has_recognized_field) {
    return true;
  }
  if (out_matched != nullptr) {
    *out_matched = true;
  }

  std::string_view service_name = res_service_name;
  if (service_name.empty() || service_name == "unknown_service") {
    if (!top_service_name.empty()) {
      service_name = top_service_name;
    } else {
      service_name = "unknown_service";
    }
  }

  std::array<char, 32> ts_buf;
  std::string_view timestamp_sv = processed_at;
  if (has_ts && t_ns > 0) {
    core::FormatIso8601Nano(t_ns, ts_buf.data());
    timestamp_sv = std::string_view(ts_buf.data(), 30);
  } else if (has_obs_ts && obs_t_ns > 0) {
    core::FormatIso8601Nano(obs_t_ns, ts_buf.data());
    timestamp_sv = std::string_view(ts_buf.data(), 30);
  } else if (has_ts || has_obs_ts) {
    core::FormatIso8601Nano(0, ts_buf.data());
    timestamp_sv = std::string_view(ts_buf.data(), 30);
  }

  emitter.StartRecord();
  emitter.SetString("Timestamp", timestamp_sv);
  emitter.SetString("TraceId", trace_id);
  emitter.SetString("SpanId", span_id);
  emitter.SetInt64("TraceFlags", flags);
  emitter.SetString("SeverityText", sev_text);
  emitter.SetInt64("SeverityNumber", sev_num);
  emitter.SetString("ServiceName", service_name);
  emitter.SetString("Body", body_sv);
  emitter.SetString("ResourceSchemaUrl", resource_schema_url);

  if (L != nullptr) {
    if (res_attrs_scoped.get() != LUA_NOREF) {
      lua_rawgeti(L, LUA_REGISTRYINDEX, res_attrs_scoped.get());
      res_attrs_scoped.reset();
    } else {
      lua_newtable(L);
    }
    emitter.SetTableFromStack("ResourceAttributes");
  }

  emitter.SetString("ScopeSchemaUrl", scope_schema_url);
  emitter.SetString("ScopeName", scope_name);
  emitter.SetString("ScopeVersion", scope_version);

  if (L != nullptr) {
    if (scope_attrs_scoped.get() != LUA_NOREF) {
      lua_rawgeti(L, LUA_REGISTRYINDEX, scope_attrs_scoped.get());
      scope_attrs_scoped.reset();
    } else {
      lua_newtable(L);
    }
    emitter.SetTableFromStack("ScopeAttributes");
  }

  if (L != nullptr) {
    if (log_attrs_scoped.get() != LUA_NOREF) {
      lua_rawgeti(L, LUA_REGISTRYINDEX, log_attrs_scoped.get());
      log_attrs_scoped.reset();
    } else {
      lua_newtable(L);
    }
    emitter.SetTableFromStack("LogAttributes");
  }

  emitter.SetString("EventName", event_name);
  emitter.SetString("processed_at", processed_at);

  emitter.EndRecord();
  if (emitter.HasError()) {
    err_out = std::string(emitter.ErrorMessage());
    return false;
  }
  return true;
}

bool HasNativeLogFields(lua_State* L, int table_idx) {
  int abs_idx = lua_absindex(L, table_idx);
  if (core::IsLuaTableEmpty(L, abs_idx)) {
    return false;
  }
  static constexpr core::ReadKey kKeys[] = {
      "message",
      "timestamp",
      "body",
      "observed_timestamp",
      "timeUnixNano",
      "time_unix_nano",
      "observedTimeUnixNano",
      "observed_time_unix_nano",
      "resources",
      "resource_attributes",
      "resourceAttributes",
      "resource",
      "attributes",
      "log_attributes",
      "logAttributes",
      "scope",
      "scope_name",
      "scopeName",
      "scope_version",
      "scopeVersion",
      "scope_schema_url",
      "scopeSchemaUrl",
      "scope_attributes",
      "scopeAttributes",
      "trace_id",
      "traceId",
      "span_id",
      "spanId",
      "severity_text",
      "severityText",
      "severity_number",
      "severityNumber",
      "flags",
      "trace_flags",
      "traceFlags",
      "event_name",
      "eventName",
      "resource_schema_url",
      "resourceSchemaUrl",
      "schema_url",
      "schemaUrl",
      "service_name",
      "service",
  };
  for (const core::ReadKey& k : kKeys) {
    if (core::GetTableFieldRaw(L, abs_idx, k)) {
      lua_pop(L, 1);
      return true;
    }
  }
  return false;
}

void CopyNativeTableBody(lua_State* L, int rec_idx, core::FastJsonWriter& writer,
                         core::LuaEmitter& emitter) {
  if (core::GetTableFieldRaw(L, rec_idx, "message", "body")) {
    int body_idx = lua_absindex(L, -1);
    int t = lua_type(L, body_idx);
    if (t == LUA_TSTRING) {
      size_t len = 0;
      const char* s = lua_tolstring(L, body_idx, &len);
      emitter.SetString("Body", std::string_view(s, len));
      lua_pop(L, 1);
      return;
    }
    if (t == LUA_TTABLE) {
      if (core::GetTableFieldRaw(L, body_idx, "stringValue", "string_value")) {
        if (lua_type(L, -1) == LUA_TSTRING) {
          size_t len = 0;
          const char* s = lua_tolstring(L, -1, &len);
          emitter.SetString("Body", std::string_view(s, len));
          lua_pop(L, 2);
          return;
        }
        lua_pop(L, 1);
      }
      writer.Reset();
      core::SerializeLuaValueToJson(L, body_idx, writer);
      emitter.SetString("Body", writer.Str());
      lua_pop(L, 1);
      return;
    }
    if (t == LUA_TNIL) {
      lua_pop(L, 1);
      emitter.SetString("Body", "");
      return;
    }
    writer.Reset();
    core::SerializeLuaValueToJson(L, body_idx, writer);
    emitter.SetString("Body", writer.Str());
    lua_pop(L, 1);
    return;
  }
  emitter.SetString("Body", "");
}

bool ExtractSingleNativeLogTable(lua_State* L, int rec_idx, std::string_view processed_at,
                                 core::FastJsonWriter& writer, std::string& tls_res_schema_scratch,
                                 std::string& tls_service_scratch,
                                 std::string& tls_scope_schema_scratch,
                                 std::string& tls_scope_name_scratch,
                                 std::string& tls_scope_ver_scratch, core::LuaEmitter& emitter,
                                 std::string& err_out) {
  if (!lua_checkstack(L, 16)) {
    err_out = "Lua stack overflow during native log extraction";
    return false;
  }
  int abs_rec_idx = lua_absindex(L, rec_idx);

  // 1. Timestamp: timestamp -> observed_timestamp -> processed_at (now())
  uint64_t t_ns = 0;
  bool has_ts = core::ReadTableTimestampField(L, abs_rec_idx, "timestamp", "timeUnixNano", t_ns);
  if (!has_ts) {
    has_ts = core::ReadTableTimestampField(L, abs_rec_idx, "time_unix_nano", nullptr, t_ns);
  }
  uint64_t obs_t_ns = 0;
  bool has_obs_ts = core::ReadTableTimestampField(L, abs_rec_idx, "observed_timestamp",
                                                  "observedTimeUnixNano", obs_t_ns);
  if (!has_obs_ts) {
    has_obs_ts =
        core::ReadTableTimestampField(L, abs_rec_idx, "observed_time_unix_nano", nullptr, obs_t_ns);
  }

  std::array<char, 32> ts_buf;
  std::string_view timestamp_sv = processed_at;
  if (has_ts && t_ns > 0) {
    core::FormatIso8601Nano(t_ns, ts_buf.data());
    timestamp_sv = std::string_view(ts_buf.data(), 30);
  } else if (has_obs_ts && obs_t_ns > 0) {
    core::FormatIso8601Nano(obs_t_ns, ts_buf.data());
    timestamp_sv = std::string_view(ts_buf.data(), 30);
  } else if (has_ts || has_obs_ts) {
    core::FormatIso8601Nano(0, ts_buf.data());
    timestamp_sv = std::string_view(ts_buf.data(), 30);
  }

  // 2. ResourceSchemaUrl, ResourceAttributes, ServiceName
  std::string_view resource_schema_url = core::ReadTableStringView(
      L, abs_rec_idx, "resource_schema_url", "resourceSchemaUrl", tls_res_schema_scratch);
  if (resource_schema_url.empty()) {
    resource_schema_url = core::ReadTableStringView(L, abs_rec_idx, "schema_url", "schemaUrl",
                                                    tls_res_schema_scratch);
  }

  std::string_view service_name;
  ScopedLuaRef res_attrs_scoped(L, LUA_NOREF);

  bool has_res = core::GetTableFieldRaw(L, abs_rec_idx, "resources", "resource_attributes");
  if (!has_res) {
    has_res = core::GetTableFieldRaw(L, abs_rec_idx, "resourceAttributes", "resource");
  }
  if (has_res) {
    int res_idx = lua_absindex(L, -1);
    if (lua_istable(L, res_idx)) {
      if (resource_schema_url.empty()) {
        resource_schema_url = core::ReadTableStringView(L, res_idx, "schema_url", "schemaUrl",
                                                        tls_res_schema_scratch);
      }
      int attrs_src_idx = res_idx;
      bool has_nested_attrs = false;
      if (core::GetTableFieldRaw(L, res_idx, "attributes")) {
        if (lua_istable(L, -1)) {
          attrs_src_idx = lua_absindex(L, -1);
          has_nested_attrs = true;
        } else {
          lua_pop(L, 1);
        }
      }
      core::PushTableAttributes(L, attrs_src_idx, writer, &service_name);
      res_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
      if (has_nested_attrs) {
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }
  if (service_name.empty() || service_name == "unknown_service") {
    std::string_view top_svc =
        core::ReadTableStringView(L, abs_rec_idx, "service_name", "service", tls_service_scratch);
    if (!top_svc.empty()) {
      service_name = top_svc;
    } else {
      service_name = "unknown_service";
    }
  }

  // 3. Scope metadata & attributes
  std::string_view scope_schema_url = core::ReadTableStringView(
      L, abs_rec_idx, "scope_schema_url", "scopeSchemaUrl", tls_scope_schema_scratch);
  std::string_view scope_name =
      core::ReadTableStringView(L, abs_rec_idx, "scope_name", "scopeName", tls_scope_name_scratch);
  std::string_view scope_version = core::ReadTableStringView(L, abs_rec_idx, "scope_version",
                                                             "scopeVersion", tls_scope_ver_scratch);
  ScopedLuaRef scope_attrs_scoped(L, LUA_NOREF);

  if (core::GetTableFieldRaw(L, abs_rec_idx, "scope")) {
    int s_idx = lua_absindex(L, -1);
    if (lua_istable(L, s_idx)) {
      if (scope_name.empty()) {
        scope_name = core::ReadTableStringView(L, s_idx, "name", nullptr, tls_scope_name_scratch);
      }
      if (scope_version.empty()) {
        scope_version =
            core::ReadTableStringView(L, s_idx, "version", nullptr, tls_scope_ver_scratch);
      }
      if (scope_schema_url.empty()) {
        scope_schema_url = core::ReadTableStringView(L, s_idx, "schema_url", "schemaUrl",
                                                     tls_scope_schema_scratch);
      }
      if (core::GetTableFieldRaw(L, s_idx, "attributes")) {
        core::PushTableAttributes(L, -1, writer);
        scope_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }
  if (scope_attrs_scoped.get() == LUA_NOREF &&
      core::GetTableFieldRaw(L, abs_rec_idx, "scope_attributes", "scopeAttributes")) {
    core::PushTableAttributes(L, -1, writer);
    scope_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
    lua_pop(L, 1);
  }

  // 4. Emit all 17 columns of logs.schema
  emitter.StartRecord();
  emitter.SetString("Timestamp", timestamp_sv);
  core::CopyStringFieldAlways(L, abs_rec_idx, "trace_id", "traceId", emitter, "TraceId");
  core::CopyStringFieldAlways(L, abs_rec_idx, "span_id", "spanId", emitter, "SpanId");

  int64_t flags = core::ReadTableInt64(L, abs_rec_idx, "flags", "trace_flags", 0);
  if (flags == 0) {
    flags = core::ReadTableInt64(L, abs_rec_idx, "traceFlags", nullptr, 0);
  }
  emitter.SetInt64("TraceFlags", flags);

  core::CopyStringFieldAlways(L, abs_rec_idx, "severity_text", "severityText", emitter,
                              "SeverityText");

  int64_t sev_num = 0;
  if (core::GetTableFieldRaw(L, abs_rec_idx, "severity_number", "severityNumber")) {
    sev_num = core::ParseTableSeverityNumber(L, -1);
    lua_pop(L, 1);
  }
  emitter.SetInt64("SeverityNumber", sev_num);

  emitter.SetString("ServiceName", service_name);
  CopyNativeTableBody(L, abs_rec_idx, writer, emitter);
  emitter.SetString("ResourceSchemaUrl", resource_schema_url);

  if (res_attrs_scoped.get() != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, res_attrs_scoped.get());
    res_attrs_scoped.reset();
  } else {
    lua_newtable(L);
  }
  emitter.SetTableFromStack("ResourceAttributes");

  emitter.SetString("ScopeSchemaUrl", scope_schema_url);
  emitter.SetString("ScopeName", scope_name);
  emitter.SetString("ScopeVersion", scope_version);

  if (scope_attrs_scoped.get() != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, scope_attrs_scoped.get());
    scope_attrs_scoped.reset();
  } else {
    lua_newtable(L);
  }
  emitter.SetTableFromStack("ScopeAttributes");

  bool has_log_attrs = core::GetTableFieldRaw(L, abs_rec_idx, "attributes", "log_attributes");
  if (!has_log_attrs) {
    has_log_attrs = core::GetTableFieldRaw(L, abs_rec_idx, "logAttributes");
  }
  if (has_log_attrs) {
    core::PushTableAttributes(L, -1, writer);
    emitter.SetTableFromStack("LogAttributes");
    lua_pop(L, 1);
  } else {
    lua_newtable(L);
    emitter.SetTableFromStack("LogAttributes");
  }

  core::CopyStringFieldAlways(L, abs_rec_idx, "event_name", "eventName", emitter, "EventName");
  emitter.SetString("processed_at", processed_at);

  emitter.EndRecord();
  if (emitter.HasError()) {
    err_out = std::string(emitter.ErrorMessage());
    return false;
  }
  return true;
}

}  // namespace

extractors::ExtractionResult ProcessNativeLogs(lua_State* L, std::string_view raw_json,
                                               int emit_idx) {
  if (raw_json.empty()) {
    return ExtractionResult{false, "Empty JSON input", 0};
  }
  core::LuaEmitter emitter(L, emit_idx);
  try {
    auto& parser = core::OtelParser::Instance();
    static thread_local core::FastJsonWriter writer;
    static thread_local core::FastJsonWriter body_writer;

    writer.Reset();
    body_writer.Reset();
    struct WriterTrimGuard {
      explicit WriterTrimGuard(core::FastJsonWriter& writer1, core::FastJsonWriter& writer2)
          : w1(writer1), w2(writer2) {}
      WriterTrimGuard(const WriterTrimGuard&) = delete;
      WriterTrimGuard& operator=(const WriterTrimGuard&) = delete;
      ~WriterTrimGuard() noexcept {
        w1.Trim();
        w2.Trim();
        core::ResetServiceNameScratch();
      }
      core::FastJsonWriter& w1;
      core::FastJsonWriter& w2;
    } trim_guard{writer, body_writer};

    auto doc_res = parser.Iterate(raw_json);
    if (doc_res.error() != simdjson::SUCCESS) {
      return ExtractionResult{false, std::string(simdjson::error_message(doc_res.error())), 0};
    }

    auto doc = std::move(doc_res.value());
    auto type_res = doc.type();
    if (type_res.error() != simdjson::SUCCESS) {
      return ExtractionResult{false, std::string(simdjson::error_message(type_res.error())), 0};
    }

    std::array<char, 32> proc_buf;
    core::FormatIso8601Nano(core::CurrentTimeUnixNano(), proc_buf.data());
    std::string_view processed_at(proc_buf.data(), 30);
    std::string err_out;

    const auto root_type = type_res.value();
    if (root_type == simdjson::ondemand::json_type::object) {
      auto doc_obj_res = doc.get_object();
      if (doc_obj_res.error() != simdjson::SUCCESS) {
        return ExtractionResult{false, std::string(simdjson::error_message(doc_obj_res.error())),
                                0};
      }
      auto doc_obj = doc_obj_res.value();
      if (!ExtractNativeLogObjectJson(L, doc_obj, processed_at, writer, body_writer, emitter,
                                      err_out)) {
        return ExtractionResult{false, err_out, emitter.EmittedCount()};
      }
    } else if (root_type == simdjson::ondemand::json_type::array) {
      auto doc_arr_res = doc.get_array();
      if (doc_arr_res.error() != simdjson::SUCCESS) {
        return ExtractionResult{false, std::string(simdjson::error_message(doc_arr_res.error())),
                                0};
      }
      for (auto elem_res : doc_arr_res.value()) {
        if (elem_res.error() != simdjson::SUCCESS) {
          return ExtractionResult{false, std::string(simdjson::error_message(elem_res.error())),
                                  emitter.EmittedCount()};
        }
        auto elem_obj_res = elem_res.value().get_object();
        if (elem_obj_res.error() != simdjson::SUCCESS) {
          return ExtractionResult{false, std::string(simdjson::error_message(elem_obj_res.error())),
                                  emitter.EmittedCount()};
        }
        auto elem_obj = elem_obj_res.value();
        if (!ExtractNativeLogObjectJson(L, elem_obj, processed_at, writer, body_writer, emitter,
                                        err_out)) {
          return ExtractionResult{false, err_out, emitter.EmittedCount()};
        }
      }
    } else {
      return ExtractionResult{false, "root JSON value must be an object or array", 0};
    }

    emitter.Flush();
    if (emitter.HasError()) {
      return ExtractionResult{false, std::string(emitter.ErrorMessage()), emitter.EmittedCount()};
    }
    return ExtractionResult{true, "", emitter.EmittedCount()};
  } catch (const std::exception& ex) {
    return ExtractionResult{false, ex.what(), emitter.EmittedCount()};
  } catch (...) {
    return ExtractionResult{false, "Unknown C++ exception during native logs processing",
                            emitter.EmittedCount()};
  }
}

extractors::ExtractionResult ProcessNativeLogsTable(lua_State* L, int table_idx, int emit_idx) {
  if (L == nullptr) {
    return ExtractionResult{false, "lua_State must not be null", 0};
  }
  const int entry_top = lua_gettop(L);
  int abs_idx = lua_absindex(L, table_idx);
  int abs_emit_idx = lua_absindex(L, emit_idx);
  if (!lua_istable(L, abs_idx)) {
    return ExtractionResult{false, "payload must be a table", 0};
  }

  static thread_local core::FastJsonWriter writer;
  static thread_local std::string tls_res_schema_scratch;
  static thread_local std::string tls_service_scratch;
  static thread_local std::string tls_scope_schema_scratch;
  static thread_local std::string tls_scope_name_scratch;
  static thread_local std::string tls_scope_ver_scratch;
  writer.Reset();
  core::ReserveScratchString(tls_res_schema_scratch);
  core::ReserveScratchString(tls_service_scratch);
  core::ReserveScratchString(tls_scope_schema_scratch);
  core::ReserveScratchString(tls_scope_name_scratch);
  core::ReserveScratchString(tls_scope_ver_scratch);
  struct TableScopeGuard {
    TableScopeGuard(lua_State* l, int t, core::FastJsonWriter& w) : L(l), top(t), writer(w) {}
    TableScopeGuard(const TableScopeGuard&) = delete;
    TableScopeGuard& operator=(const TableScopeGuard&) = delete;
    ~TableScopeGuard() noexcept {
      if (L != nullptr) {
        lua_settop(L, top);
      }
      writer.Trim();
      core::TrimScratchString(tls_res_schema_scratch);
      core::TrimScratchString(tls_service_scratch);
      core::TrimScratchString(tls_scope_schema_scratch);
      core::TrimScratchString(tls_scope_name_scratch);
      core::TrimScratchString(tls_scope_ver_scratch);
    }
    lua_State* L;
    int top;
    core::FastJsonWriter& writer;
  } scope_guard{L, entry_top, writer};

  core::LuaEmitter emitter(L, abs_emit_idx);
  try {
    std::array<char, 32> proc_buf;
    core::FormatIso8601Nano(core::CurrentTimeUnixNano(), proc_buf.data());
    std::string_view processed_at(proc_buf.data(), 30);
    std::string err_out;

    auto extract_array = [&](int arr_idx, size_t len) -> bool {
      for (size_t i = 1; i <= len; ++i) {
        lua_rawgeti(L, arr_idx, static_cast<lua_Integer>(i));
        if (lua_istable(L, -1)) {
          int item_idx = lua_absindex(L, -1);
          if (HasNativeLogFields(L, item_idx)) {
            if (!ExtractSingleNativeLogTable(L, item_idx, processed_at, writer,
                                             tls_res_schema_scratch, tls_service_scratch,
                                             tls_scope_schema_scratch, tls_scope_name_scratch,
                                             tls_scope_ver_scratch, emitter, err_out)) {
              lua_pop(L, 1);
              return false;
            }
          }
        }
        lua_pop(L, 1);
      }
      return true;
    };

    const size_t raw_len = lua_rawlen(L, abs_idx);
    if (raw_len > 0) {
      if (!extract_array(abs_idx, raw_len)) {
        return ExtractionResult{false, err_out, emitter.EmittedCount()};
      }
    } else {
      bool handled_log_wrapper = false;
      if (core::GetTableFieldRaw(L, abs_idx, "log")) {
        int log_idx = lua_absindex(L, -1);
        if (lua_istable(L, log_idx)) {
          const size_t inner_len = lua_rawlen(L, log_idx);
          if (inner_len > 0) {
            handled_log_wrapper = true;
            if (!extract_array(log_idx, inner_len)) {
              lua_pop(L, 1);
              return ExtractionResult{false, err_out, emitter.EmittedCount()};
            }
          } else if (HasNativeLogFields(L, log_idx)) {
            handled_log_wrapper = true;
            if (!ExtractSingleNativeLogTable(L, log_idx, processed_at, writer,
                                             tls_res_schema_scratch, tls_service_scratch,
                                             tls_scope_schema_scratch, tls_scope_name_scratch,
                                             tls_scope_ver_scratch, emitter, err_out)) {
              lua_pop(L, 1);
              return ExtractionResult{false, err_out, emitter.EmittedCount()};
            }
          }
        }
        lua_pop(L, 1);
      }
      if (!handled_log_wrapper && HasNativeLogFields(L, abs_idx)) {
        if (!ExtractSingleNativeLogTable(L, abs_idx, processed_at, writer, tls_res_schema_scratch,
                                         tls_service_scratch, tls_scope_schema_scratch,
                                         tls_scope_name_scratch, tls_scope_ver_scratch, emitter,
                                         err_out)) {
          return ExtractionResult{false, err_out, emitter.EmittedCount()};
        }
      }
    }

    emitter.Flush();
    if (emitter.HasError()) {
      return ExtractionResult{false, std::string(emitter.ErrorMessage()), emitter.EmittedCount()};
    }
    return ExtractionResult{true, "", emitter.EmittedCount()};
  } catch (const std::exception& ex) {
    return ExtractionResult{false, ex.what(), emitter.EmittedCount()};
  } catch (...) {
    return ExtractionResult{false, "Unknown C++ exception during native logs table processing",
                            emitter.EmittedCount()};
  }
}

}  // namespace vector_otel::native_extractors
