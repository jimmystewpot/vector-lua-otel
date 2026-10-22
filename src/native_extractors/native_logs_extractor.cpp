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
#include "vector_otel/core/otlp_common.hpp"
#include "vector_otel/core/parser.hpp"
#include "vector_otel/core/process_runner.hpp"
#include "vector_otel/core/row_emitter.hpp"
#include "vector_otel/core/scratch.hpp"
#include "vector_otel/core/staged_row.hpp"
#include "vector_otel/core/table_reader.hpp"
#include "vector_otel/core/timestamp.hpp"

namespace vector_otel::native_extractors {

namespace {

using core::ErrorCode;
using core::OperationContext;
using extractors::ExtractionResult;

// Reusable per-thread staging buffers (capacity retained between invocations). A `log` wrapper
// nests at most one level, so two rows cover an enclosing object and its wrapped record.
struct NativeLogsScratch {
  core::FastJsonWriter res_attrs_writer;    // ResourceAttributes JSON
  core::FastJsonWriter scope_attrs_writer;  // ScopeAttributes JSON
  core::FastJsonWriter log_attrs_writer;    // LogAttributes JSON
  core::FastJsonWriter body_writer;         // structured body serialisation
  core::StagedRow rows[2];
};

void ResetScratch(NativeLogsScratch& s) {
  s.res_attrs_writer.Reset();
  s.scope_attrs_writer.Reset();
  s.log_attrs_writer.Reset();
  s.body_writer.Reset();
}

void TrimScratch(NativeLogsScratch& s) noexcept {
  s.res_attrs_writer.Trim();
  s.scope_attrs_writer.Trim();
  s.log_attrs_writer.Trim();
  s.body_writer.Trim();
}

NativeLogsScratch& Scratch() {
  static thread_local NativeLogsScratch scratch;
  return scratch;
}

struct TableScratch {
  std::string res_schema;
  std::string service;
  std::string scope_schema;
  std::string scope_name;
  std::string scope_version;
};

TableScratch& TScratch() {
  static thread_local TableScratch scratch;
  return scratch;
}

bool FailJson(OperationContext& ctx, simdjson::error_code code) {
  ctx.error.Set(ErrorCode::Json, simdjson::error_message(code));
  return false;
}

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

bool ExtractNativeLogObjectJson(OperationContext& ctx, NativeLogsScratch& s,
                                simdjson::ondemand::object& rec_obj, std::string_view processed_at,
                                int depth = 0, bool* out_matched = nullptr) {
  // A nested wrapper object stages into its own row, so an unrecognised wrapper never disturbs
  // what the enclosing object has staged so far (the wrapper nests at most one level). The
  // attribute writers are shared: an unrecognised wrapper writes none of them, and a recognised
  // one ends the enclosing object's extraction.
  core::StagedRow& row = s.rows[depth];
  core::FastJsonWriter& res_attrs_writer = s.res_attrs_writer;
  core::FastJsonWriter& scope_attrs_writer = s.scope_attrs_writer;
  core::FastJsonWriter& log_attrs_writer = s.log_attrs_writer;
  core::RowEmitter emitter(ctx, row);
  emitter.StartRecord();
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
  bool has_res_attrs = false;
  bool has_scope_attrs = false;
  bool has_log_attrs = false;
  bool has_recognized_field = false;

  for (auto field_res : rec_obj) {
    if (field_res.error() != simdjson::SUCCESS) {
      return FailJson(ctx, field_res.error());
    }
    auto k_res = field_res.unescaped_key();
    if (k_res.error() != simdjson::SUCCESS) {
      return FailJson(ctx, k_res.error());
    }
    std::string_view k = k_res.value();
    auto fv_res = field_res.value();
    if (fv_res.error() != simdjson::SUCCESS) {
      return FailJson(ctx, fv_res.error());
    }
    auto fv = fv_res.value();

    if (k == "log" && depth == 0) {
      auto t_res = fv.type();
      if (t_res.error() != simdjson::SUCCESS) {
        return FailJson(ctx, t_res.error());
      }
      if (t_res.value() == simdjson::ondemand::json_type::object) {
        auto inner_obj_res = fv.get_object();
        if (inner_obj_res.error() != simdjson::SUCCESS) {
          return FailJson(ctx, inner_obj_res.error());
        }
        auto inner_obj = inner_obj_res.value();
        bool inner_matched = false;
        if (!ExtractNativeLogObjectJson(ctx, s, inner_obj, processed_at, depth + 1,
                                        &inner_matched)) {
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
          return FailJson(ctx, inner_arr_res.error());
        }
        bool any_matched = false;
        for (auto elem_res : inner_arr_res.value()) {
          if (elem_res.error() != simdjson::SUCCESS) {
            return FailJson(ctx, elem_res.error());
          }
          auto elem_obj_res = elem_res.value().get_object();
          if (elem_obj_res.error() != simdjson::SUCCESS) {
            return FailJson(ctx, elem_obj_res.error());
          }
          auto elem_obj = elem_obj_res.value();
          bool elem_matched = false;
          if (!ExtractNativeLogObjectJson(ctx, s, elem_obj, processed_at, depth + 1,
                                          &elem_matched)) {
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
      body_sv = ExtractBodyView(fv, s.body_writer);
      has_message = true;
    } else if (k == "body") {
      has_recognized_field = true;
      if (!has_message) {
        body_sv = ExtractBodyView(fv, s.body_writer);
      }
    } else if (k == "resources" || k == "resource_attributes" || k == "resourceAttributes" ||
               k == "resource") {
      has_recognized_field = true;
      res_service_name = "unknown_service";
      res_attrs_writer.Reset();
      core::WriteAttributesJson(res_attrs_writer, fv, &res_service_name);
      has_res_attrs = true;
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
          } else if (sk == "attributes") {
            scope_attrs_writer.Reset();
            core::WriteAttributesJson(scope_attrs_writer, sfv);
            has_scope_attrs = true;
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
      if (!has_scope_attrs) {
        scope_attrs_writer.Reset();
        core::WriteAttributesJson(scope_attrs_writer, fv);
        has_scope_attrs = true;
      }
    } else if (k == "attributes" || k == "log_attributes" || k == "logAttributes") {
      has_recognized_field = true;
      log_attrs_writer.Reset();
      core::WriteAttributesJson(log_attrs_writer, fv);
      has_log_attrs = true;
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

  emitter.SetString("Timestamp", timestamp_sv);
  emitter.SetString("TraceId", trace_id);
  emitter.SetString("SpanId", span_id);
  emitter.SetInt64("TraceFlags", flags);
  emitter.SetString("SeverityText", sev_text);
  emitter.SetInt64("SeverityNumber", sev_num);
  emitter.SetString("ServiceName", service_name);
  emitter.SetString("Body", body_sv);
  emitter.SetString("ResourceSchemaUrl", resource_schema_url);
  emitter.SetString("ResourceAttributes", has_res_attrs ? res_attrs_writer.Str() : "{}");
  emitter.SetString("ScopeSchemaUrl", scope_schema_url);
  emitter.SetString("ScopeName", scope_name);
  emitter.SetString("ScopeVersion", scope_version);
  emitter.SetString("ScopeAttributes", has_scope_attrs ? scope_attrs_writer.Str() : "{}");
  emitter.SetString("LogAttributes", has_log_attrs ? log_attrs_writer.Str() : "{}");
  emitter.SetString("EventName", event_name);
  emitter.SetString("processed_at", processed_at);

  emitter.EndRecord();
  return !emitter.HasError();
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
                         core::RowEmitter& emitter) {
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

// Table-mode extraction of one native log record. Every Lua access is a raw, allocation-free read
// (see read_keys.hpp): this runs in the dispatcher's outer frame and must never see a Lua error.
bool ExtractSingleNativeLogTable(OperationContext& ctx, NativeLogsScratch& s, TableScratch& t,
                                 int rec_idx, std::string_view processed_at) {
  lua_State* L = ctx.L;
  core::FastJsonWriter& res_attrs_writer = s.res_attrs_writer;
  core::FastJsonWriter& scope_attrs_writer = s.scope_attrs_writer;
  core::FastJsonWriter& log_attrs_writer = s.log_attrs_writer;
  core::StagedRow& row = s.rows[0];
  core::RowEmitter emitter(ctx, row);
  const int abs_rec_idx = lua_absindex(L, rec_idx);

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

  // The row is built in the order the table is read; attribute maps are staged directly into it.
  emitter.StartRecord();

  // 2. ResourceSchemaUrl, ResourceAttributes, ServiceName
  std::string_view resource_schema_url = core::ReadTableStringView(
      L, abs_rec_idx, "resource_schema_url", "resourceSchemaUrl", t.res_schema);
  if (resource_schema_url.empty()) {
    resource_schema_url =
        core::ReadTableStringView(L, abs_rec_idx, "schema_url", "schemaUrl", t.res_schema);
  }

  std::string_view service_name;
  res_attrs_writer.Reset();
  bool has_res_attrs = false;

  bool has_res = core::GetTableFieldRaw(L, abs_rec_idx, "resources", "resource_attributes");
  if (!has_res) {
    has_res = core::GetTableFieldRaw(L, abs_rec_idx, "resourceAttributes", "resource");
  }
  if (has_res) {
    int res_idx = lua_absindex(L, -1);
    if (lua_istable(L, res_idx)) {
      if (resource_schema_url.empty()) {
        resource_schema_url =
            core::ReadTableStringView(L, res_idx, "schema_url", "schemaUrl", t.res_schema);
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
      core::WriteTableAttributesJson(L, attrs_src_idx, res_attrs_writer, &service_name);
      has_res_attrs = true;
      if (has_nested_attrs) {
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }
  if (service_name.empty() || service_name == "unknown_service") {
    std::string_view top_svc =
        core::ReadTableStringView(L, abs_rec_idx, "service_name", "service", t.service);
    if (!top_svc.empty()) {
      service_name = top_svc;
    } else {
      service_name = "unknown_service";
    }
  }

  // 3. Scope metadata & attributes
  std::string_view scope_schema_url = core::ReadTableStringView(L, abs_rec_idx, "scope_schema_url",
                                                                "scopeSchemaUrl", t.scope_schema);
  std::string_view scope_name =
      core::ReadTableStringView(L, abs_rec_idx, "scope_name", "scopeName", t.scope_name);
  std::string_view scope_version =
      core::ReadTableStringView(L, abs_rec_idx, "scope_version", "scopeVersion", t.scope_version);
  scope_attrs_writer.Reset();
  bool has_scope_attrs = false;

  if (core::GetTableFieldRaw(L, abs_rec_idx, "scope")) {
    int s_idx = lua_absindex(L, -1);
    if (lua_istable(L, s_idx)) {
      if (scope_name.empty()) {
        scope_name = core::ReadTableStringView(L, s_idx, "name", nullptr, t.scope_name);
      }
      if (scope_version.empty()) {
        scope_version = core::ReadTableStringView(L, s_idx, "version", nullptr, t.scope_version);
      }
      if (scope_schema_url.empty()) {
        scope_schema_url =
            core::ReadTableStringView(L, s_idx, "schema_url", "schemaUrl", t.scope_schema);
      }
      if (core::GetTableFieldRaw(L, s_idx, "attributes")) {
        core::WriteTableAttributesJson(L, -1, scope_attrs_writer);
        has_scope_attrs = true;
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }
  if (!has_scope_attrs &&
      core::GetTableFieldRaw(L, abs_rec_idx, "scope_attributes", "scopeAttributes")) {
    core::WriteTableAttributesJson(L, -1, scope_attrs_writer);
    has_scope_attrs = true;
    lua_pop(L, 1);
  }

  // 4. The remaining columns of logs.schema
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
  CopyNativeTableBody(L, abs_rec_idx, s.body_writer, emitter);
  emitter.SetString("ResourceSchemaUrl", resource_schema_url);
  emitter.SetString("ResourceAttributes", has_res_attrs ? res_attrs_writer.Str() : "{}");
  emitter.SetString("ScopeSchemaUrl", scope_schema_url);
  emitter.SetString("ScopeName", scope_name);
  emitter.SetString("ScopeVersion", scope_version);
  emitter.SetString("ScopeAttributes", has_scope_attrs ? scope_attrs_writer.Str() : "{}");

  log_attrs_writer.Reset();
  bool has_log_attrs = core::GetTableFieldRaw(L, abs_rec_idx, "attributes", "log_attributes");
  if (!has_log_attrs) {
    has_log_attrs = core::GetTableFieldRaw(L, abs_rec_idx, "logAttributes");
  }
  if (has_log_attrs) {
    core::WriteTableAttributesJson(L, -1, log_attrs_writer);
    lua_pop(L, 1);
  }
  emitter.SetString("LogAttributes", has_log_attrs ? log_attrs_writer.Str() : "{}");

  core::CopyStringFieldAlways(L, abs_rec_idx, "event_name", "eventName", emitter, "EventName");
  emitter.SetString("processed_at", processed_at);

  emitter.EndRecord();
  return !emitter.HasError();
}

}  // namespace

bool ExtractNativeLogsJson(OperationContext& ctx, core::OtelParser& parser,
                           simdjson::padded_string_view admitted) {
  NativeLogsScratch& s = Scratch();
  ResetScratch(s);
  struct TrimGuard {
    explicit TrimGuard(NativeLogsScratch& scratch) : s(scratch) {}
    TrimGuard(const TrimGuard&) = delete;
    TrimGuard& operator=(const TrimGuard&) = delete;
    ~TrimGuard() noexcept {
      TrimScratch(s);
      core::ResetServiceNameScratch();
    }
    NativeLogsScratch& s;
  } trim_guard{s};

  core::RowEmitter emitter(ctx, s.rows[0]);

  auto doc_res = parser.IteratePrepared(admitted);
  if (doc_res.error() != simdjson::SUCCESS) {
    return FailJson(ctx, doc_res.error());
  }
  auto doc = std::move(doc_res.value());
  auto type_res = doc.type();
  if (type_res.error() != simdjson::SUCCESS) {
    return FailJson(ctx, type_res.error());
  }

  std::array<char, 32> proc_buf;
  core::FormatIso8601Nano(core::CurrentTimeUnixNano(), proc_buf.data());
  std::string_view processed_at(proc_buf.data(), 30);

  const auto root_type = type_res.value();
  if (root_type == simdjson::ondemand::json_type::object) {
    auto doc_obj_res = doc.get_object();
    if (doc_obj_res.error() != simdjson::SUCCESS) {
      return FailJson(ctx, doc_obj_res.error());
    }
    auto doc_obj = doc_obj_res.value();
    if (!ExtractNativeLogObjectJson(ctx, s, doc_obj, processed_at)) {
      return false;
    }
  } else if (root_type == simdjson::ondemand::json_type::array) {
    auto doc_arr_res = doc.get_array();
    if (doc_arr_res.error() != simdjson::SUCCESS) {
      return FailJson(ctx, doc_arr_res.error());
    }
    for (auto elem_res : doc_arr_res.value()) {
      if (elem_res.error() != simdjson::SUCCESS) {
        return FailJson(ctx, elem_res.error());
      }
      auto elem_obj_res = elem_res.value().get_object();
      if (elem_obj_res.error() != simdjson::SUCCESS) {
        return FailJson(ctx, elem_obj_res.error());
      }
      auto elem_obj = elem_obj_res.value();
      if (!ExtractNativeLogObjectJson(ctx, s, elem_obj, processed_at)) {
        return false;
      }
    }
  } else {
    ctx.error.Set(ErrorCode::Json, "root JSON value must be an object or array");
    return false;
  }

  emitter.Flush();
  return !emitter.HasError();
}

bool ExtractNativeLogsTable(OperationContext& ctx) {
  NativeLogsScratch& s = Scratch();
  TableScratch& t = TScratch();
  ResetScratch(s);
  core::ReserveScratchString(t.res_schema, 128);
  core::ReserveScratchString(t.service, 128);
  core::ReserveScratchString(t.scope_schema, 128);
  core::ReserveScratchString(t.scope_name, 128);
  core::ReserveScratchString(t.scope_version, 64);
  struct Guard {
    Guard(lua_State* l, int top, NativeLogsScratch& scratch, TableScratch& table)
        : L(l), entry_top(top), s(scratch), t(table) {}
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    ~Guard() noexcept {
      lua_settop(L, entry_top);
      TrimScratch(s);
      core::ResetScratchString(t.res_schema);
      core::ResetScratchString(t.service);
      core::ResetScratchString(t.scope_schema);
      core::ResetScratchString(t.scope_name);
      core::ResetScratchString(t.scope_version);
    }
    lua_State* L;
    int entry_top;
    NativeLogsScratch& s;
    TableScratch& t;
  } guard{ctx.L, lua_gettop(ctx.L), s, t};

  lua_State* L = ctx.L;
  core::RowEmitter emitter(ctx, s.rows[1]);
  std::array<char, 32> proc_buf;
  core::FormatIso8601Nano(core::CurrentTimeUnixNano(), proc_buf.data());
  std::string_view processed_at(proc_buf.data(), 30);

  const int abs_idx = lua_absindex(L, ctx.input_index);

  auto extract_array = [&](int arr_idx, size_t len) -> bool {
    for (size_t i = 1; i <= len; ++i) {
      lua_rawgeti(L, arr_idx, static_cast<lua_Integer>(i));
      if (lua_istable(L, -1)) {
        int item_idx = lua_absindex(L, -1);
        if (HasNativeLogFields(L, item_idx)) {
          if (!ExtractSingleNativeLogTable(ctx, s, t, item_idx, processed_at)) {
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
      return false;
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
            return false;
          }
        } else if (HasNativeLogFields(L, log_idx)) {
          handled_log_wrapper = true;
          if (!ExtractSingleNativeLogTable(ctx, s, t, log_idx, processed_at)) {
            lua_pop(L, 1);
            return false;
          }
        }
      }
      lua_pop(L, 1);
    }
    if (!handled_log_wrapper && HasNativeLogFields(L, abs_idx)) {
      if (!ExtractSingleNativeLogTable(ctx, s, t, abs_idx, processed_at)) {
        return false;
      }
    }
  }

  emitter.Flush();
  return !emitter.HasError();
}

namespace {
constexpr core::SignalExtractors kNativeLogsExtractors{ExtractNativeLogsJson,
                                                       ExtractNativeLogsTable};
}

ExtractionResult ProcessNativeLogs(lua_State* L, std::string_view raw_json, int emit_idx) {
  return core::RunCompatJson(L, core::ModuleKind::Native, core::Signal::Logs, kNativeLogsExtractors,
                             raw_json, emit_idx);
}

ExtractionResult ProcessNativeLogsTable(lua_State* L, int table_idx, int emit_idx) {
  return core::RunCompatTable(L, core::ModuleKind::Native, core::Signal::Logs,
                              kNativeLogsExtractors, table_idx, emit_idx);
}

const core::SignalExtractors& NativeLogsExtractors() { return kNativeLogsExtractors; }

}  // namespace vector_otel::native_extractors
