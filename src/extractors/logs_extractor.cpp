#include "vector_otel/extractors/logs_extractor.hpp"

#include <array>
#include <cstdint>
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

namespace vector_otel::extractors {

namespace {

using core::AnchorSlot;
using core::ErrorCode;
using core::OperationContext;

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
        writer.RawJson(raw_res.value());
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
      return raw_res.value();
    }
    return "";
  }
}

// Reusable per-thread staging buffers: capacity is retained between invocations, so the steady
// state performs no C++ allocation per record.
struct LogsScratch {
  core::FastJsonWriter writer;       // attribute / AnyValue serialisation
  core::FastJsonWriter body_writer;  // structured body serialisation
  core::StagedRow row;               // the record being built
  core::StagedRow resource_tape;     // shared ResourceAttributes table, built once per resource
  core::StagedRow scope_tape;        // shared ScopeAttributes table, built once per scope
};

LogsScratch& Scratch() {
  static thread_local LogsScratch scratch;
  return scratch;
}

bool FailJson(OperationContext& ctx, simdjson::error_code code) {
  ctx.error.Set(ErrorCode::Json, simdjson::error_message(code));
  return false;
}

}  // namespace

bool ExtractLogsJson(OperationContext& ctx, core::OtelParser& parser,
                     simdjson::padded_string_view admitted) {
  LogsScratch& s = Scratch();
  s.writer.Reset();
  s.body_writer.Reset();
  struct TrimGuard {
    explicit TrimGuard(LogsScratch& scratch) : s(scratch) {}
    TrimGuard(const TrimGuard&) = delete;
    TrimGuard& operator=(const TrimGuard&) = delete;
    ~TrimGuard() noexcept {
      s.writer.Trim();
      s.body_writer.Trim();
      core::ResetServiceNameScratch();
    }
    LogsScratch& s;
  } trim_guard{s};

  core::RowEmitter emitter(ctx, s.row);

  auto doc_res = parser.IteratePrepared(admitted);
  if (doc_res.error() != simdjson::SUCCESS) {
    return FailJson(ctx, doc_res.error());
  }
  auto doc = std::move(doc_res.value());
  auto doc_obj_res = doc.get_object();
  if (doc_obj_res.error() != simdjson::SUCCESS) {
    return FailJson(ctx, doc_obj_res.error());
  }
  auto doc_obj = doc_obj_res.value();

  std::array<char, 32> proc_buf;
  core::FormatIso8601Nano(core::CurrentTimeUnixNano(), proc_buf.data());
  std::string_view processed_at(proc_buf.data(), 30);

  auto rl_res = doc_obj.find_field_unordered("resourceLogs");
  if (rl_res.error() != simdjson::SUCCESS) {
    rl_res = doc_obj.find_field_unordered("resource_logs");
  }
  if (rl_res.error() != simdjson::SUCCESS) {
    return true;
  }
  auto rl_val = rl_res.value();
  auto rl_arr_res = rl_val.get_array();
  if (rl_arr_res.error() != simdjson::SUCCESS) {
    return true;
  }

  for (auto rl_elem_res : rl_arr_res.value()) {
    if (rl_elem_res.error() != simdjson::SUCCESS) {
      return FailJson(ctx, rl_elem_res.error());
    }
    auto rl_val_item = rl_elem_res.value();
    auto rl_obj_res = rl_val_item.get_object();
    if (rl_obj_res.error() != simdjson::SUCCESS) {
      continue;
    }
    auto rl_obj = rl_obj_res.value();

    // 1. Resource schema URL
    std::string_view resource_schema_url;
    auto res_schema_res = rl_obj.find_field_unordered("schemaUrl");
    if (res_schema_res.error() != simdjson::SUCCESS) {
      res_schema_res = rl_obj.find_field_unordered("schema_url");
    }
    if (res_schema_res.error() == simdjson::SUCCESS) {
      auto s_res = res_schema_res.value().get_string();
      if (s_res.error() == simdjson::SUCCESS) {
        resource_schema_url = s_res.value();
      }
    }

    // 2. Resource service name and attributes (staged once, materialised lazily)
    std::string_view service_name = "unknown_service";
    s.resource_tape.Reset();
    bool resource_ready = false;
    auto res_res = rl_obj.find_field_unordered("resource");
    if (res_res.error() == simdjson::SUCCESS) {
      auto r_val = res_res.value();
      auto r_obj_res = r_val.get_object();
      if (r_obj_res.error() == simdjson::SUCCESS) {
        auto r_obj = r_obj_res.value();
        auto attrs_res = r_obj.find_field_unordered("attributes");
        if (attrs_res.error() == simdjson::SUCCESS) {
          auto attrs_val = attrs_res.value();
          core::StageAttributeFields(s.resource_tape, attrs_val, s.writer, &service_name);
        }
      }
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
        return FailJson(ctx, sl_elem_res.error());
      }
      auto sl_val_item = sl_elem_res.value();
      auto sl_obj_res = sl_val_item.get_object();
      if (sl_obj_res.error() != simdjson::SUCCESS) {
        continue;
      }
      auto sl_obj = sl_obj_res.value();

      // Scope schema URL
      std::string_view scope_schema_url;
      auto sl_schema_res = sl_obj.find_field_unordered("schemaUrl");
      if (sl_schema_res.error() != simdjson::SUCCESS) {
        sl_schema_res = sl_obj.find_field_unordered("schema_url");
      }
      if (sl_schema_res.error() == simdjson::SUCCESS) {
        auto s_res = sl_schema_res.value().get_string();
        if (s_res.error() == simdjson::SUCCESS) {
          scope_schema_url = s_res.value();
        }
      }

      // Scope metadata and attributes
      std::string_view scope_name;
      std::string_view scope_version;
      s.scope_tape.Reset();
      bool scope_ready = false;
      auto scope_res = sl_obj.find_field_unordered("scope");
      if (scope_res.error() == simdjson::SUCCESS) {
        auto scope_val = scope_res.value();
        auto s_obj_res = scope_val.get_object();
        if (s_obj_res.error() == simdjson::SUCCESS) {
          auto s_obj = s_obj_res.value();
          scope_name = core::ExtractStringField(s_obj, "name", "");
          scope_version = core::ExtractStringField(s_obj, "version", "");
          auto s_attrs_res = s_obj.find_field_unordered("attributes");
          if (s_attrs_res.error() == simdjson::SUCCESS) {
            auto s_attrs_val = s_attrs_res.value();
            core::StageAttributeFields(s.scope_tape, s_attrs_val, s.writer);
          }
        }
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
          return FailJson(ctx, rec_elem_res.error());
        }
        auto rec_val_item = rec_elem_res.value();
        auto rec_obj_res = rec_val_item.get_object();
        if (rec_obj_res.error() != simdjson::SUCCESS) {
          continue;
        }
        auto rec_obj = rec_obj_res.value();

        uint64_t t_ns = 0;
        uint64_t obs_t_ns = 0;
        std::string_view trace_id;
        std::string_view span_id;
        int64_t flags = 0;
        std::string_view sev_text;
        int64_t sev_num = 0;
        std::string_view body_sv;
        std::string_view event_name;
        bool has_log_attrs = false;

        core::IdHexBuffer trace_id_buf;
        core::IdHexBuffer span_id_buf;

        // The row is started first: LogAttributes is staged straight into it when met.
        emitter.StartRecord();

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

          if (k == "timeUnixNano" || k == "time_unix_nano") {
            t_ns = core::ParseUint64(fv);
          } else if (k == "observedTimeUnixNano" || k == "observed_time_unix_nano") {
            obs_t_ns = core::ParseUint64(fv);
          } else if (k == "traceId" || k == "trace_id") {
            auto s_res = fv.get_string();
            if (s_res.error() == simdjson::SUCCESS) {
              trace_id = core::NormalizeTraceId(s_res.value(), trace_id_buf);
            }
          } else if (k == "spanId" || k == "span_id") {
            auto s_res = fv.get_string();
            if (s_res.error() == simdjson::SUCCESS) {
              span_id = core::NormalizeSpanId(s_res.value(), span_id_buf);
            }
          } else if (k == "flags" || k == "traceFlags" || k == "trace_flags") {
            flags = core::ParseInt64(fv);
          } else if (k == "severityText" || k == "severity_text") {
            auto s_res = fv.get_string();
            if (s_res.error() == simdjson::SUCCESS) {
              sev_text = s_res.value();
            }
          } else if (k == "severityNumber" || k == "severity_number") {
            sev_num = core::ParseSeverityNumber(fv);
          } else if (k == "body") {
            body_sv = ExtractBodyView(fv, s.body_writer);
          } else if (k == "eventName" || k == "event_name") {
            auto s_res = fv.get_string();
            if (s_res.error() == simdjson::SUCCESS) {
              event_name = s_res.value();
            }
          } else if (k == "attributes") {
            core::StageAttributes(s.row, "LogAttributes", fv, s.writer);
            has_log_attrs = true;
          }
        }

        if (t_ns == 0) {
          t_ns = obs_t_ns;
        }
        std::array<char, 32> ts_buf;
        core::FormatIso8601Nano(t_ns, ts_buf.data());

        if (!core::EnsureSharedAnchor(ctx, s.resource_tape, AnchorSlot::kResourceAttrs,
                                      resource_ready) ||
            !core::EnsureSharedAnchor(ctx, s.scope_tape, AnchorSlot::kScopeAttrs, scope_ready)) {
          return false;
        }

        emitter.SetString("Timestamp", std::string_view(ts_buf.data(), 30));
        emitter.SetString("TraceId", trace_id);
        emitter.SetString("SpanId", span_id);
        emitter.SetInt64("TraceFlags", flags);
        emitter.SetString("SeverityText", sev_text);
        emitter.SetInt64("SeverityNumber", sev_num);
        emitter.SetString("ServiceName", service_name);
        emitter.SetString("Body", body_sv);
        emitter.SetString("ResourceSchemaUrl", resource_schema_url);
        s.row.Anchor("ResourceAttributes", AnchorSlot::kResourceAttrs);
        emitter.SetString("ScopeSchemaUrl", scope_schema_url);
        emitter.SetString("ScopeName", scope_name);
        emitter.SetString("ScopeVersion", scope_version);
        s.row.Anchor("ScopeAttributes", AnchorSlot::kScopeAttrs);
        if (!has_log_attrs) {
          s.row.BeginMap("LogAttributes");
          s.row.End();
        }
        emitter.SetString("EventName", event_name);
        emitter.SetString("processed_at", processed_at);

        emitter.EndRecord();
        if (emitter.HasError()) {
          return false;
        }
      }
    }
  }

  emitter.Flush();
  return !emitter.HasError();
}

namespace {

struct TableScratch {
  std::string res_schema;
  std::string scope_schema;
  std::string scope_name;
  std::string scope_version;
};

TableScratch& TScratch() {
  static thread_local TableScratch scratch;
  return scratch;
}

// Table-mode extraction. Every Lua access here is a raw, allocation-free read (see read_keys.hpp):
// this frame is the dispatcher's outer frame and must never see a Lua error. Rows are staged and
// delivered by the protected emission leaves.
bool ExtractLogsTableImpl(OperationContext& ctx, LogsScratch& s, TableScratch& t,
                          std::string_view processed_at) {
  lua_State* L = ctx.L;
  core::RowEmitter emitter(ctx, s.row);

  const int abs_idx = lua_absindex(L, ctx.input_index);
  int rl_arr_idx = 0;
  if (core::GetTableField(L, abs_idx, "resourceLogs", "resource_logs")) {
    rl_arr_idx = lua_absindex(L, -1);
  } else if (core::GetTableField(L, abs_idx, "log")) {
    int log_idx = lua_absindex(L, -1);
    if (core::GetTableField(L, log_idx, "resourceLogs", "resource_logs")) {
      lua_remove(L, log_idx);
      rl_arr_idx = lua_absindex(L, -1);
    } else {
      lua_pop(L, 1);
    }
  }
  if (rl_arr_idx == 0) {
    rl_arr_idx = abs_idx;
  }
  if (!lua_istable(L, rl_arr_idx)) {
    return true;
  }

  const lua_Integer rl_len = static_cast<lua_Integer>(lua_rawlen(L, rl_arr_idx));
  for (lua_Integer i = 1; i <= rl_len; ++i) {
    lua_rawgeti(L, rl_arr_idx, i);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      continue;
    }
    const int rl_obj_idx = lua_absindex(L, -1);

    // 1. Resource schema URL
    std::string_view resource_schema_url =
        core::ReadTableStringView(L, rl_obj_idx, "schemaUrl", "schema_url", t.res_schema);

    // 2. Resource service name & attributes (single pass, staged once per resource)
    std::string_view service_name = "unknown_service";
    s.resource_tape.Reset();
    bool resource_ready = false;
    if (core::GetTableField(L, rl_obj_idx, "resource")) {
      const int res_idx = lua_absindex(L, -1);
      if (core::GetTableField(L, res_idx, "attributes")) {
        core::StageTableAttributeFields(s.resource_tape, L, -1, s.writer, &service_name);
        lua_pop(L, 1);  // pop attributes
      }
      lua_pop(L, 1);  // pop resource
    }

    // 3. Iterate scopeLogs
    if (!core::GetTableField(L, rl_obj_idx, "scopeLogs", "scope_logs")) {
      lua_pop(L, 1);  // pop rl_obj
      continue;
    }
    const int sl_arr_idx = lua_absindex(L, -1);
    if (!lua_istable(L, sl_arr_idx)) {
      lua_pop(L, 2);  // pop sl_arr and rl_obj
      continue;
    }

    const lua_Integer sl_len = static_cast<lua_Integer>(lua_rawlen(L, sl_arr_idx));
    for (lua_Integer j = 1; j <= sl_len; ++j) {
      lua_rawgeti(L, sl_arr_idx, j);
      if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        continue;
      }
      const int sl_obj_idx = lua_absindex(L, -1);

      std::string_view scope_schema_url =
          core::ReadTableStringView(L, sl_obj_idx, "schemaUrl", "schema_url", t.scope_schema);
      std::string_view scope_name;
      std::string_view scope_version;
      s.scope_tape.Reset();
      bool scope_ready = false;

      if (core::GetTableField(L, sl_obj_idx, "scope")) {
        const int s_idx = lua_absindex(L, -1);
        scope_name = core::ReadTableStringView(L, s_idx, "name", nullptr, t.scope_name);
        scope_version = core::ReadTableStringView(L, s_idx, "version", nullptr, t.scope_version);
        if (core::GetTableField(L, s_idx, "attributes")) {
          core::StageTableAttributeFields(s.scope_tape, L, -1, s.writer);
          lua_pop(L, 1);  // pop attributes
        }
        lua_pop(L, 1);  // pop scope
      }

      // 4. Iterate logRecords
      if (core::GetTableField(L, sl_obj_idx, "logRecords", "log_records")) {
        const int recs_arr_idx = lua_absindex(L, -1);
        if (lua_istable(L, recs_arr_idx)) {
          const lua_Integer recs_len = static_cast<lua_Integer>(lua_rawlen(L, recs_arr_idx));
          for (lua_Integer k = 1; k <= recs_len; ++k) {
            lua_rawgeti(L, recs_arr_idx, k);
            if (!lua_istable(L, -1)) {
              lua_pop(L, 1);
              continue;
            }
            const int rec_obj_idx = lua_absindex(L, -1);

            if (!core::EnsureSharedAnchor(ctx, s.resource_tape, AnchorSlot::kResourceAttrs,
                                          resource_ready) ||
                !core::EnsureSharedAnchor(ctx, s.scope_tape, AnchorSlot::kScopeAttrs,
                                          scope_ready)) {
              return false;
            }

            emitter.StartRecord();

            // 1. Timestamp
            const uint64_t t_ns = core::ReadTableTimestampNano(L, rec_obj_idx);
            std::array<char, 32> ts_buf;
            core::FormatIso8601Nano(t_ns, ts_buf.data());
            emitter.SetString("Timestamp", std::string_view(ts_buf.data(), 30));

            // 2. TraceId, 3. SpanId
            core::CopyIdField(L, rec_obj_idx, "traceId", "trace_id", core::kTraceIdBinaryLen,
                              emitter, "TraceId", "TraceId");
            core::CopyIdField(L, rec_obj_idx, "spanId", "span_id", core::kSpanIdBinaryLen, emitter,
                              "SpanId", "SpanId");

            // 4. TraceFlags
            emitter.SetInt64("TraceFlags", core::ReadTableInt64(L, rec_obj_idx, "flags",
                                                                "traceFlags", 0, "trace_flags"));

            // 5. SeverityText
            core::CopyStringField(L, rec_obj_idx, "severityText", "severity_text", emitter,
                                  "SeverityText");

            // 6. SeverityNumber
            int64_t sev_num = 0;
            if (core::GetTableField(L, rec_obj_idx, "severityNumber", "severity_number")) {
              sev_num = core::ParseTableSeverityNumber(L, -1);
              lua_pop(L, 1);
            }
            emitter.SetInt64("SeverityNumber", sev_num);

            // 7. ServiceName, 8. Body, 9. ResourceSchemaUrl
            emitter.SetString("ServiceName", service_name);
            core::CopyTableBody(L, rec_obj_idx, s.writer, emitter);
            emitter.SetString("ResourceSchemaUrl", resource_schema_url);

            // 10-14. shared resource / scope attributes and scope metadata
            s.row.Anchor("ResourceAttributes", AnchorSlot::kResourceAttrs);
            emitter.SetString("ScopeSchemaUrl", scope_schema_url);
            emitter.SetString("ScopeName", scope_name);
            emitter.SetString("ScopeVersion", scope_version);
            s.row.Anchor("ScopeAttributes", AnchorSlot::kScopeAttrs);

            // 15. LogAttributes (an empty table when absent)
            if (core::GetTableField(L, rec_obj_idx, "attributes")) {
              core::StageTableAttributes(s.row, "LogAttributes", L, -1, s.writer);
              lua_pop(L, 1);  // pop attributes
            } else {
              s.row.BeginMap("LogAttributes");
              s.row.End();
            }

            // 16. EventName, 17. processed_at
            core::CopyStringField(L, rec_obj_idx, "eventName", "event_name", emitter, "EventName");
            emitter.SetString("processed_at", processed_at);

            emitter.EndRecord();
            lua_pop(L, 1);  // pop rec_obj
            if (emitter.HasError()) {
              return false;
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

  emitter.Flush();
  return !emitter.HasError();
}

}  // namespace

bool ExtractLogsTable(OperationContext& ctx) {
  LogsScratch& s = Scratch();
  TableScratch& t = TScratch();
  s.writer.Reset();
  core::ReserveScratchString(t.res_schema, 128);
  core::ReserveScratchString(t.scope_schema, 128);
  core::ReserveScratchString(t.scope_name, 128);
  core::ReserveScratchString(t.scope_version, 64);
  struct Guard {
    Guard(lua_State* l, int top, LogsScratch& scratch, TableScratch& table)
        : L(l), entry_top(top), s(scratch), t(table) {}
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    ~Guard() noexcept {
      lua_settop(L, entry_top);
      s.writer.Trim();
      core::ResetScratchString(t.res_schema);
      core::ResetScratchString(t.scope_schema);
      core::ResetScratchString(t.scope_name);
      core::ResetScratchString(t.scope_version);
    }
    lua_State* L;
    int entry_top;
    LogsScratch& s;
    TableScratch& t;
  } guard{ctx.L, lua_gettop(ctx.L), s, t};

  std::array<char, 32> proc_buf;
  core::FormatIso8601Nano(core::CurrentTimeUnixNano(), proc_buf.data());
  return ExtractLogsTableImpl(ctx, s, t, std::string_view(proc_buf.data(), 30));
}

// ---- compatibility entry points ---------------------------------------------------------------

namespace {
constexpr core::SignalExtractors kLogsExtractors{ExtractLogsJson, ExtractLogsTable};
}

ExtractionResult ProcessLogs(lua_State* L, std::string_view raw_json, int emit_idx) {
  return core::RunCompatJson(L, core::ModuleKind::Otel, core::Signal::Logs, kLogsExtractors,
                             raw_json, emit_idx);
}

ExtractionResult ProcessLogsTable(lua_State* L, int table_idx, int emit_idx) {
  return core::RunCompatTable(L, core::ModuleKind::Otel, core::Signal::Logs, kLogsExtractors,
                              table_idx, emit_idx);
}

const core::SignalExtractors& LogsExtractors() { return kLogsExtractors; }

}  // namespace vector_otel::extractors
