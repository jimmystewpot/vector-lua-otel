#include "vector_otel/native_extractors/native_traces_extractor.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

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

// Reusable per-thread staging buffers (capacity retained between invocations). A wrapper nests at
// most one level, so two rows (and event/link writers) cover an enclosing object and its record.
// The attribute writers are shared across the two levels: an unrecognised wrapper writes none of
// them, and a recognised one ends the enclosing object's extraction.
struct NativeTracesScratch {
  core::FastJsonWriter writer;  // span event / link attribute serialisation, table-mode events
  core::FastJsonWriter res_attrs_writer;   // ResourceAttributes JSON
  core::FastJsonWriter span_attrs_writer;  // SpanAttributes JSON
  core::FastJsonWriter events[2];
  core::FastJsonWriter links[2];
  core::StagedRow rows[2];
};

NativeTracesScratch& Scratch() {
  static thread_local NativeTracesScratch scratch;
  return scratch;
}

struct TableScratch {
  std::string service;
  std::string scope_name;
  std::string scope_version;
  std::string status_msg;
  std::string item;
};

TableScratch& TScratch() {
  static thread_local TableScratch scratch;
  return scratch;
}

bool FailJson(OperationContext& ctx, simdjson::error_code code) {
  ctx.error.Set(ErrorCode::Json, simdjson::error_message(code));
  return false;
}

void ResetWriters(NativeTracesScratch& s) {
  s.writer.Reset();
  s.res_attrs_writer.Reset();
  s.span_attrs_writer.Reset();
  for (auto& w : s.events) w.Reset();
  for (auto& w : s.links) w.Reset();
}

void TrimWriters(NativeTracesScratch& s) noexcept {
  s.writer.Trim();
  s.res_attrs_writer.Trim();
  s.span_attrs_writer.Trim();
  for (auto& w : s.events) w.Trim();
  for (auto& w : s.links) w.Trim();
}

std::string_view SpanKindFromInt(int64_t k) noexcept {
  switch (k) {
    case 1:
      return "SPAN_KIND_INTERNAL";
    case 2:
      return "SPAN_KIND_SERVER";
    case 3:
      return "SPAN_KIND_CLIENT";
    case 4:
      return "SPAN_KIND_PRODUCER";
    case 5:
      return "SPAN_KIND_CONSUMER";
    default:
      return "SPAN_KIND_UNSPECIFIED";
  }
}

std::string_view NormalizeSpanKindString(std::string_view sv) noexcept {
  sv = core::TrimJsonWhitespace(sv);
  if (sv.empty()) {
    return "SPAN_KIND_UNSPECIFIED";
  }
  if (core::EqualsIgnoreCaseAscii(sv, "INTERNAL") ||
      core::EqualsIgnoreCaseAscii(sv, "SPAN_KIND_INTERNAL") || sv == "1") {
    return "SPAN_KIND_INTERNAL";
  }
  if (core::EqualsIgnoreCaseAscii(sv, "SERVER") ||
      core::EqualsIgnoreCaseAscii(sv, "SPAN_KIND_SERVER") || sv == "2") {
    return "SPAN_KIND_SERVER";
  }
  if (core::EqualsIgnoreCaseAscii(sv, "CLIENT") ||
      core::EqualsIgnoreCaseAscii(sv, "SPAN_KIND_CLIENT") || sv == "3") {
    return "SPAN_KIND_CLIENT";
  }
  if (core::EqualsIgnoreCaseAscii(sv, "PRODUCER") ||
      core::EqualsIgnoreCaseAscii(sv, "SPAN_KIND_PRODUCER") || sv == "4") {
    return "SPAN_KIND_PRODUCER";
  }
  if (core::EqualsIgnoreCaseAscii(sv, "CONSUMER") ||
      core::EqualsIgnoreCaseAscii(sv, "SPAN_KIND_CONSUMER") || sv == "5") {
    return "SPAN_KIND_CONSUMER";
  }
  return "SPAN_KIND_UNSPECIFIED";
}

std::string_view StatusCodeFromInt(int64_t c) noexcept {
  switch (c) {
    case 1:
      return "STATUS_CODE_OK";
    case 2:
      return "STATUS_CODE_ERROR";
    default:
      return "STATUS_CODE_UNSET";
  }
}

std::string_view NormalizeStatusCodeString(std::string_view sv) noexcept {
  sv = core::TrimJsonWhitespace(sv);
  if (sv.empty()) {
    return "STATUS_CODE_UNSET";
  }
  if (core::EqualsIgnoreCaseAscii(sv, "OK") || core::EqualsIgnoreCaseAscii(sv, "STATUS_CODE_OK") ||
      sv == "1") {
    return "STATUS_CODE_OK";
  }
  if (core::EqualsIgnoreCaseAscii(sv, "ERROR") ||
      core::EqualsIgnoreCaseAscii(sv, "STATUS_CODE_ERROR") || sv == "2") {
    return "STATUS_CODE_ERROR";
  }
  return "STATUS_CODE_UNSET";
}

std::string_view ParseJsonSpanKind(simdjson::ondemand::value& kval) {
  auto ktype_res = kval.type();
  if (ktype_res.error() != simdjson::SUCCESS) {
    return "SPAN_KIND_UNSPECIFIED";
  }
  if (ktype_res.value() == simdjson::ondemand::json_type::number) {
    return SpanKindFromInt(core::ParseInt64(kval));
  }
  if (ktype_res.value() == simdjson::ondemand::json_type::string) {
    auto s_res = kval.get_string();
    if (s_res.error() == simdjson::SUCCESS) {
      return NormalizeSpanKindString(s_res.value());
    }
  }
  return "SPAN_KIND_UNSPECIFIED";
}

std::string_view ParseJsonStatusCode(simdjson::ondemand::value& code_val) {
  auto ctype_res = code_val.type();
  if (ctype_res.error() != simdjson::SUCCESS) {
    return "STATUS_CODE_UNSET";
  }
  if (ctype_res.value() == simdjson::ondemand::json_type::number) {
    return StatusCodeFromInt(core::ParseInt64(code_val));
  }
  if (ctype_res.value() == simdjson::ondemand::json_type::string) {
    auto s_res = code_val.get_string();
    if (s_res.error() == simdjson::SUCCESS) {
      return NormalizeStatusCodeString(s_res.value());
    }
  }
  return "STATUS_CODE_UNSET";
}

void SerializeNativeSpanEventsJson(simdjson::ondemand::value& events_val,
                                   core::FastJsonWriter& events_writer,
                                   core::FastJsonWriter& attr_writer) {
  events_writer.StartArray();
  auto type_res = events_val.type();
  if (type_res.error() == simdjson::SUCCESS &&
      type_res.value() == simdjson::ondemand::json_type::array) {
    auto arr_res = events_val.get_array();
    if (arr_res.error() == simdjson::SUCCESS) {
      for (auto elem_res : arr_res.value()) {
        if (elem_res.error() != simdjson::SUCCESS) continue;
        auto item_val = elem_res.value();
        auto ev_obj_res = item_val.get_object();
        if (ev_obj_res.error() != simdjson::SUCCESS) continue;
        auto ev_obj = ev_obj_res.value();

        uint64_t time_nanos = 0;
        std::string_view name;
        bool has_ev_attrs = false;
        attr_writer.Reset();

        for (auto f_res : ev_obj) {
          if (f_res.error() != simdjson::SUCCESS) continue;
          auto k_res = f_res.unescaped_key();
          if (k_res.error() != simdjson::SUCCESS) continue;
          std::string_view k = k_res.value();
          auto fv_res = f_res.value();
          if (fv_res.error() != simdjson::SUCCESS) continue;
          auto fv = fv_res.value();

          if (k == "time_unix_nano" || k == "timeUnixNano" || k == "timestamp") {
            uint64_t parsed = 0;
            if (core::ParseJsonTimestamp(fv, parsed)) {
              time_nanos = parsed;
            }
          } else if (k == "name") {
            auto s_res = fv.get_string();
            if (s_res.error() == simdjson::SUCCESS) {
              name = s_res.value();
            }
          } else if (k == "attributes") {
            attr_writer.Reset();
            core::WriteAttributesJson(attr_writer, fv);
            has_ev_attrs = true;
          }
        }

        events_writer.StartObject();
        events_writer.Key("Timestamp");
        std::array<char, 32> ev_ts_buf;
        core::FormatIso8601Nano(time_nanos, ev_ts_buf.data());
        events_writer.String(std::string_view(ev_ts_buf.data(), 30));

        events_writer.Key("Name");
        events_writer.String(name);

        events_writer.Key("Attributes");
        if (has_ev_attrs) {
          events_writer.RawJson(attr_writer.Str());
        } else {
          events_writer.StartObject();
          events_writer.EndObject();
        }
        events_writer.EndObject();
      }
    }
  }
  events_writer.EndArray();
}

void SerializeNativeSpanLinksJson(simdjson::ondemand::value& links_val,
                                  core::FastJsonWriter& links_writer,
                                  core::FastJsonWriter& attr_writer) {
  links_writer.StartArray();
  auto type_res = links_val.type();
  if (type_res.error() == simdjson::SUCCESS &&
      type_res.value() == simdjson::ondemand::json_type::array) {
    auto arr_res = links_val.get_array();
    if (arr_res.error() == simdjson::SUCCESS) {
      for (auto elem_res : arr_res.value()) {
        if (elem_res.error() != simdjson::SUCCESS) continue;
        auto item_val = elem_res.value();
        auto link_obj_res = item_val.get_object();
        if (link_obj_res.error() != simdjson::SUCCESS) continue;
        auto link_obj = link_obj_res.value();

        std::string_view trace_id;
        std::string_view span_id;
        std::string_view trace_state;
        bool has_link_attrs = false;
        attr_writer.Reset();

        for (auto f_res : link_obj) {
          if (f_res.error() != simdjson::SUCCESS) continue;
          auto k_res = f_res.unescaped_key();
          if (k_res.error() != simdjson::SUCCESS) continue;
          std::string_view k = k_res.value();
          auto fv_res = f_res.value();
          if (fv_res.error() != simdjson::SUCCESS) continue;
          auto fv = fv_res.value();

          if (k == "trace_id" || k == "traceId") {
            auto s_res = fv.get_string();
            if (s_res.error() == simdjson::SUCCESS) {
              trace_id = s_res.value();
            }
          } else if (k == "span_id" || k == "spanId") {
            auto s_res = fv.get_string();
            if (s_res.error() == simdjson::SUCCESS) {
              span_id = s_res.value();
            }
          } else if (k == "trace_state" || k == "traceState") {
            auto s_res = fv.get_string();
            if (s_res.error() == simdjson::SUCCESS) {
              trace_state = s_res.value();
            }
          } else if (k == "attributes") {
            attr_writer.Reset();
            core::WriteAttributesJson(attr_writer, fv);
            has_link_attrs = true;
          }
        }

        links_writer.StartObject();
        links_writer.Key("TraceId");
        links_writer.String(trace_id);

        links_writer.Key("SpanId");
        links_writer.String(span_id);

        links_writer.Key("TraceState");
        links_writer.String(trace_state);

        links_writer.Key("Attributes");
        if (has_link_attrs) {
          links_writer.RawJson(attr_writer.Str());
        } else {
          links_writer.StartObject();
          links_writer.EndObject();
        }
        links_writer.EndObject();
      }
    }
  }
  links_writer.EndArray();
}

bool HasNativeTraceFields(lua_State* L, int table_idx) {
  int abs_idx = lua_absindex(L, table_idx);
  if (!lua_istable(L, abs_idx) || core::IsLuaTableEmpty(L, abs_idx)) {
    return false;
  }
  static constexpr core::ReadKey kKeys[] = {
      "trace_id",
      "traceId",
      "span_id",
      "spanId",
      "parent_span_id",
      "parentSpanId",
      "trace_state",
      "traceState",
      "name",
      "span_name",
      "spanName",
      "kind",
      "span_kind",
      "spanKind",
      "start_time_unix_nano",
      "startTimeUnixNano",
      "start_timestamp",
      "timestamp",
      "end_time_unix_nano",
      "endTimeUnixNano",
      "end_timestamp",
      "ingest_timestamp",
      "ingestTimestamp",
      "status",
      "status_code",
      "statusCode",
      "status_message",
      "statusMessage",
      "events",
      "links",
      "resources",
      "resource_attributes",
      "resourceAttributes",
      "resource",
      "attributes",
      "span_attributes",
      "spanAttributes",
      "scope",
      "scope_name",
      "scopeName",
      "scope_version",
      "scopeVersion",
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

std::string_view ParseLuaSpanKindValue(lua_State* L, int val_idx) {
  int abs_idx = lua_absindex(L, val_idx);
  int t = lua_type(L, abs_idx);
  if (t == LUA_TNUMBER) {
    if (lua_isinteger(L, abs_idx)) {
      return SpanKindFromInt(lua_tointeger(L, abs_idx));
    }
    return SpanKindFromInt(static_cast<int64_t>(lua_tonumber(L, abs_idx)));
  }
  if (t == LUA_TSTRING) {
    size_t len = 0;
    const char* s = lua_tolstring(L, abs_idx, &len);
    return NormalizeSpanKindString(std::string_view(s, len));
  }
  return "SPAN_KIND_UNSPECIFIED";
}

std::string_view ParseLuaStatusCodeValue(lua_State* L, int val_idx) {
  int abs_idx = lua_absindex(L, val_idx);
  int t = lua_type(L, abs_idx);
  if (t == LUA_TNUMBER) {
    if (lua_isinteger(L, abs_idx)) {
      return StatusCodeFromInt(lua_tointeger(L, abs_idx));
    }
    return StatusCodeFromInt(static_cast<int64_t>(lua_tonumber(L, abs_idx)));
  }
  if (t == LUA_TSTRING) {
    size_t len = 0;
    const char* s = lua_tolstring(L, abs_idx, &len);
    return NormalizeStatusCodeString(std::string_view(s, len));
  }
  return "STATUS_CODE_UNSET";
}

void SerializeTableSpanEventsToJson(lua_State* L, int events_idx, core::FastJsonWriter& writer,
                                    std::string& scratch) {
  int abs_idx = lua_absindex(L, events_idx);
  writer.StartArray();
  if (!lua_istable(L, abs_idx)) {
    writer.EndArray();
    return;
  }

  const size_t len = lua_rawlen(L, abs_idx);
  for (size_t i = 1; i <= len; ++i) {
    lua_rawgeti(L, abs_idx, static_cast<lua_Integer>(i));
    if (lua_istable(L, -1)) {
      int ev_idx = lua_absindex(L, -1);

      uint64_t ev_ns = 0;
      if (!core::ReadTableTimestampField(L, ev_idx, "time_unix_nano", "timeUnixNano", ev_ns)) {
        core::ReadTableTimestampField(L, ev_idx, "timestamp", nullptr, ev_ns);
      }
      std::array<char, 32> ev_ts_buf;
      core::FormatIso8601Nano(ev_ns, ev_ts_buf.data());

      std::string_view ev_name = core::ReadTableStringView(L, ev_idx, "name", nullptr, scratch);

      writer.StartObject();
      writer.Key("Timestamp");
      writer.String(std::string_view(ev_ts_buf.data(), 30));

      writer.Key("Name");
      writer.String(ev_name);

      writer.Key("Attributes");
      if (core::GetTableFieldRaw(L, ev_idx, "attributes")) {
        core::WriteTableAttributesJson(L, -1, writer);
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

void SerializeTableSpanLinksToJson(lua_State* L, int links_idx, core::FastJsonWriter& writer,
                                   std::string& scratch) {
  int abs_idx = lua_absindex(L, links_idx);
  writer.StartArray();
  if (!lua_istable(L, abs_idx)) {
    writer.EndArray();
    return;
  }

  const size_t len = lua_rawlen(L, abs_idx);
  for (size_t i = 1; i <= len; ++i) {
    lua_rawgeti(L, abs_idx, static_cast<lua_Integer>(i));
    if (lua_istable(L, -1)) {
      int link_idx = lua_absindex(L, -1);

      writer.StartObject();

      std::string_view trace_id =
          core::ReadTableStringView(L, link_idx, "trace_id", "traceId", scratch);
      writer.Key("TraceId");
      writer.String(trace_id);

      std::string_view span_id =
          core::ReadTableStringView(L, link_idx, "span_id", "spanId", scratch);
      writer.Key("SpanId");
      writer.String(span_id);

      std::string_view trace_state =
          core::ReadTableStringView(L, link_idx, "trace_state", "traceState", scratch);
      writer.Key("TraceState");
      writer.String(trace_state);

      writer.Key("Attributes");
      if (core::GetTableFieldRaw(L, link_idx, "attributes")) {
        core::WriteTableAttributesJson(L, -1, writer);
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

bool ExtractNativeTraceObjectJson(OperationContext& ctx, NativeTracesScratch& s,
                                  simdjson::ondemand::object& rec_obj,
                                  std::string_view processed_at, int depth = 0,
                                  bool* out_matched = nullptr) {
  // A nested wrapper object stages into its own row and writers, so an unrecognised wrapper never
  // disturbs what the enclosing object has staged so far.
  core::StagedRow& row = s.rows[depth];
  core::FastJsonWriter& events_writer = s.events[depth];
  core::FastJsonWriter& links_writer = s.links[depth];
  core::FastJsonWriter& attr_writer = s.writer;
  core::FastJsonWriter& res_attrs_writer = s.res_attrs_writer;
  core::FastJsonWriter& span_attrs_writer = s.span_attrs_writer;
  core::RowEmitter emitter(ctx, row);
  emitter.StartRecord();
  uint64_t start_ns = 0;
  uint64_t end_ns = 0;
  uint64_t ingest_ns = 0;
  bool has_start = false;
  bool has_end = false;
  bool has_ingest = false;

  std::string_view trace_id;
  std::string_view span_id;
  std::string_view parent_span_id;
  std::string_view trace_state;
  std::string_view span_name;
  std::string_view span_kind = "SPAN_KIND_UNSPECIFIED";
  std::string_view res_service_name = "unknown_service";
  std::string_view top_service_name;
  std::string_view scope_name;
  std::string_view scope_version;
  std::string_view status_code = "STATUS_CODE_UNSET";
  std::string_view status_msg;

  bool has_events = false;
  bool has_links = false;
  bool has_res_attrs = false;
  bool has_span_attrs = false;
  bool has_recognized_field = false;

  events_writer.Reset();
  links_writer.Reset();

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

    if ((k == "log" || k == "trace") && depth == 0) {
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
        if (!ExtractNativeTraceObjectJson(ctx, s, inner_obj, processed_at, depth + 1,
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
          if (!ExtractNativeTraceObjectJson(ctx, s, elem_obj, processed_at, depth + 1,
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

    if (k == "start_time_unix_nano" || k == "startTimeUnixNano" || k == "start_timestamp" ||
        k == "timestamp") {
      has_recognized_field = true;
      uint64_t parsed = 0;
      if (core::ParseJsonTimestamp(fv, parsed)) {
        start_ns = parsed;
        has_start = true;
      }
    } else if (k == "end_time_unix_nano" || k == "endTimeUnixNano" || k == "end_timestamp") {
      has_recognized_field = true;
      uint64_t parsed = 0;
      if (core::ParseJsonTimestamp(fv, parsed)) {
        end_ns = parsed;
        has_end = true;
      }
    } else if (k == "ingest_timestamp" || k == "ingestTimestamp") {
      has_recognized_field = true;
      uint64_t parsed = 0;
      if (core::ParseJsonTimestamp(fv, parsed)) {
        ingest_ns = parsed;
        has_ingest = true;
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
    } else if (k == "parent_span_id" || k == "parentSpanId") {
      has_recognized_field = true;
      auto s_res = fv.get_string();
      if (s_res.error() == simdjson::SUCCESS) {
        parent_span_id = s_res.value();
      }
    } else if (k == "trace_state" || k == "traceState") {
      has_recognized_field = true;
      auto s_res = fv.get_string();
      if (s_res.error() == simdjson::SUCCESS) {
        trace_state = s_res.value();
      }
    } else if (k == "name" || k == "span_name" || k == "spanName") {
      has_recognized_field = true;
      auto s_res = fv.get_string();
      if (s_res.error() == simdjson::SUCCESS) {
        span_name = s_res.value();
      }
    } else if (k == "kind" || k == "span_kind" || k == "spanKind") {
      has_recognized_field = true;
      span_kind = ParseJsonSpanKind(fv);
    } else if (k == "status") {
      has_recognized_field = true;
      auto status_obj_res = fv.get_object();
      if (status_obj_res.error() == simdjson::SUCCESS) {
        for (auto st_res : status_obj_res.value()) {
          if (st_res.error() != simdjson::SUCCESS) continue;
          auto stk_res = st_res.unescaped_key();
          if (stk_res.error() != simdjson::SUCCESS) continue;
          std::string_view stk = stk_res.value();
          auto stv_res = st_res.value();
          if (stv_res.error() != simdjson::SUCCESS) continue;
          auto stv = stv_res.value();
          if (stk == "code") {
            status_code = ParseJsonStatusCode(stv);
          } else if (stk == "message") {
            auto m_res = stv.get_string();
            if (m_res.error() == simdjson::SUCCESS) {
              status_msg = m_res.value();
            }
          }
        }
      }
    } else if (k == "status_code" || k == "statusCode") {
      has_recognized_field = true;
      if (status_code == "STATUS_CODE_UNSET") {
        status_code = ParseJsonStatusCode(fv);
      }
    } else if (k == "status_message" || k == "statusMessage") {
      has_recognized_field = true;
      if (status_msg.empty()) {
        auto m_res = fv.get_string();
        if (m_res.error() == simdjson::SUCCESS) {
          status_msg = m_res.value();
        }
      }
    } else if (k == "resources" || k == "resource_attributes" || k == "resourceAttributes" ||
               k == "resource") {
      has_recognized_field = true;
      res_service_name = "unknown_service";
      res_attrs_writer.Reset();
      core::WriteAttributesJson(res_attrs_writer, fv, &res_service_name);
      has_res_attrs = true;
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
          }
        }
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
    } else if (k == "attributes" || k == "span_attributes" || k == "spanAttributes") {
      has_recognized_field = true;
      span_attrs_writer.Reset();
      core::WriteAttributesJson(span_attrs_writer, fv);
      has_span_attrs = true;
    } else if (k == "events") {
      has_recognized_field = true;
      events_writer.Reset();
      SerializeNativeSpanEventsJson(fv, events_writer, attr_writer);
      has_events = true;
    } else if (k == "links") {
      has_recognized_field = true;
      links_writer.Reset();
      SerializeNativeSpanLinksJson(fv, links_writer, attr_writer);
      has_links = true;
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

  if (!has_events) {
    events_writer.Reset();
    events_writer.StartArray();
    events_writer.EndArray();
  }
  if (!has_links) {
    links_writer.Reset();
    links_writer.StartArray();
    links_writer.EndArray();
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
  if (has_start && start_ns > 0) {
    core::FormatIso8601Nano(start_ns, ts_buf.data());
    timestamp_sv = std::string_view(ts_buf.data(), 30);
  } else if (has_ingest && ingest_ns > 0) {
    core::FormatIso8601Nano(ingest_ns, ts_buf.data());
    timestamp_sv = std::string_view(ts_buf.data(), 30);
  } else if (has_start || has_ingest) {
    core::FormatIso8601Nano(0, ts_buf.data());
    timestamp_sv = std::string_view(ts_buf.data(), 30);
  }

  uint64_t delta =
      (has_start && has_end && end_ns >= start_ns && start_ns > 0) ? (end_ns - start_ns) : 0;
  constexpr uint64_t kMaxDuration = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  int64_t duration = static_cast<int64_t>(std::min<uint64_t>(delta, kMaxDuration));

  emitter.SetString("Timestamp", timestamp_sv);
  emitter.SetString("TraceId", trace_id);
  emitter.SetString("SpanId", span_id);
  emitter.SetString("ParentSpanId", parent_span_id);
  emitter.SetString("TraceState", trace_state);
  emitter.SetString("SpanName", span_name);
  emitter.SetString("SpanKind", span_kind);
  emitter.SetString("ServiceName", service_name);
  emitter.SetString("ResourceAttributes", has_res_attrs ? res_attrs_writer.Str() : "{}");
  emitter.SetString("ScopeName", scope_name);
  emitter.SetString("ScopeVersion", scope_version);
  emitter.SetString("SpanAttributes", has_span_attrs ? span_attrs_writer.Str() : "{}");
  emitter.SetInt64("Duration", duration);
  emitter.SetString("StatusCode", status_code);
  emitter.SetString("StatusMessage", status_msg);
  emitter.SetString("Events", events_writer.Str());
  emitter.SetString("Links", links_writer.Str());
  emitter.SetString("processed_at", processed_at);

  emitter.EndRecord();
  return !emitter.HasError();
}

// Table-mode extraction of one native trace record. Every Lua access is a raw, allocation-free
// read (see read_keys.hpp): this runs in the dispatcher's outer frame and must never see a Lua
// error.
bool ExtractSingleNativeTraceTable(OperationContext& ctx, NativeTracesScratch& s, TableScratch& t,
                                   int rec_idx, std::string_view processed_at) {
  lua_State* L = ctx.L;
  core::FastJsonWriter& res_attrs_writer = s.res_attrs_writer;
  core::FastJsonWriter& span_attrs_writer = s.span_attrs_writer;
  core::StagedRow& row = s.rows[0];
  core::RowEmitter emitter(ctx, row);
  const int abs_rec_idx = lua_absindex(L, rec_idx);
  emitter.StartRecord();

  // 1. Timestamps & Duration
  uint64_t start_ns = 0;
  bool has_start = core::ReadTableTimestampField(L, abs_rec_idx, "start_time_unix_nano",
                                                 "startTimeUnixNano", start_ns);
  if (!has_start) {
    has_start =
        core::ReadTableTimestampField(L, abs_rec_idx, "start_timestamp", "timestamp", start_ns);
  }

  uint64_t end_ns = 0;
  bool has_end = core::ReadTableTimestampField(L, abs_rec_idx, "end_time_unix_nano",
                                               "endTimeUnixNano", end_ns);
  if (!has_end) {
    has_end = core::ReadTableTimestampField(L, abs_rec_idx, "end_timestamp", nullptr, end_ns);
  }

  uint64_t ingest_ns = 0;
  bool has_ingest = core::ReadTableTimestampField(L, abs_rec_idx, "ingest_timestamp",
                                                  "ingestTimestamp", ingest_ns);

  std::array<char, 32> ts_buf;
  std::string_view timestamp_sv = processed_at;
  if (has_start && start_ns > 0) {
    core::FormatIso8601Nano(start_ns, ts_buf.data());
    timestamp_sv = std::string_view(ts_buf.data(), 30);
  } else if (has_ingest && ingest_ns > 0) {
    core::FormatIso8601Nano(ingest_ns, ts_buf.data());
    timestamp_sv = std::string_view(ts_buf.data(), 30);
  } else if (has_start || has_ingest) {
    core::FormatIso8601Nano(0, ts_buf.data());
    timestamp_sv = std::string_view(ts_buf.data(), 30);
  }

  uint64_t delta =
      (has_start && has_end && end_ns >= start_ns && start_ns > 0) ? (end_ns - start_ns) : 0;
  constexpr uint64_t kMaxDuration = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  int64_t duration = static_cast<int64_t>(std::min<uint64_t>(delta, kMaxDuration));

  // 2. ResourceAttributes & ServiceName
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

  // 3. ScopeName & ScopeVersion
  std::string_view scope_name =
      core::ReadTableStringView(L, abs_rec_idx, "scope_name", "scopeName", t.scope_name);
  std::string_view scope_version =
      core::ReadTableStringView(L, abs_rec_idx, "scope_version", "scopeVersion", t.scope_version);
  if (core::GetTableFieldRaw(L, abs_rec_idx, "scope")) {
    int s_idx = lua_absindex(L, -1);
    if (lua_istable(L, s_idx)) {
      if (scope_name.empty()) {
        scope_name = core::ReadTableStringView(L, s_idx, "name", nullptr, t.scope_name);
      }
      if (scope_version.empty()) {
        scope_version = core::ReadTableStringView(L, s_idx, "version", nullptr, t.scope_version);
      }
    }
    lua_pop(L, 1);
  }

  // 4. SpanKind, StatusCode, StatusMessage
  std::string_view span_kind = "SPAN_KIND_UNSPECIFIED";
  bool has_kind = core::GetTableFieldRaw(L, abs_rec_idx, "kind", "span_kind");
  if (!has_kind) {
    has_kind = core::GetTableFieldRaw(L, abs_rec_idx, "spanKind");
  }
  if (has_kind) {
    span_kind = ParseLuaSpanKindValue(L, -1);
    lua_pop(L, 1);
  }

  std::string_view status_code = "STATUS_CODE_UNSET";
  std::string_view status_msg;
  if (core::GetTableFieldRaw(L, abs_rec_idx, "status")) {
    int st_idx = lua_absindex(L, -1);
    if (lua_istable(L, st_idx)) {
      if (core::GetTableFieldRaw(L, st_idx, "code")) {
        status_code = ParseLuaStatusCodeValue(L, -1);
        lua_pop(L, 1);
      }
      status_msg = core::ReadTableStringView(L, st_idx, "message", nullptr, t.status_msg);
    }
    lua_pop(L, 1);
  }
  if (status_code == "STATUS_CODE_UNSET" &&
      core::GetTableFieldRaw(L, abs_rec_idx, "status_code", "statusCode")) {
    status_code = ParseLuaStatusCodeValue(L, -1);
    lua_pop(L, 1);
  }
  if (status_msg.empty()) {
    status_msg =
        core::ReadTableStringView(L, abs_rec_idx, "status_message", "statusMessage", t.status_msg);
  }

  // 5. Emit all 18 columns of traces.schema
  emitter.SetString("Timestamp", timestamp_sv);
  core::CopyStringFieldAlways(L, abs_rec_idx, "trace_id", "traceId", emitter, "TraceId");
  core::CopyStringFieldAlways(L, abs_rec_idx, "span_id", "spanId", emitter, "SpanId");
  core::CopyStringFieldAlways(L, abs_rec_idx, "parent_span_id", "parentSpanId", emitter,
                              "ParentSpanId");
  core::CopyStringFieldAlways(L, abs_rec_idx, "trace_state", "traceState", emitter, "TraceState");

  if (core::GetTableFieldRaw(L, abs_rec_idx, "name", "span_name")) {
    char num[core::kScalarTextBuffer];
    std::string_view text;
    if (core::ScalarText(L, -1, num, text)) {
      emitter.SetString("SpanName", text);
    } else {
      emitter.SetString("SpanName", "");
    }
    lua_pop(L, 1);
  } else {
    core::CopyStringFieldAlways(L, abs_rec_idx, "spanName", nullptr, emitter, "SpanName");
  }

  emitter.SetString("SpanKind", span_kind);
  emitter.SetString("ServiceName", service_name);
  emitter.SetString("ResourceAttributes", has_res_attrs ? res_attrs_writer.Str() : "{}");
  emitter.SetString("ScopeName", scope_name);
  emitter.SetString("ScopeVersion", scope_version);

  span_attrs_writer.Reset();
  bool has_span_attrs = core::GetTableFieldRaw(L, abs_rec_idx, "attributes", "span_attributes");
  if (!has_span_attrs) {
    has_span_attrs = core::GetTableFieldRaw(L, abs_rec_idx, "spanAttributes");
  }
  if (has_span_attrs) {
    core::WriteTableAttributesJson(L, -1, span_attrs_writer);
    lua_pop(L, 1);
  }
  emitter.SetString("SpanAttributes", has_span_attrs ? span_attrs_writer.Str() : "{}");

  emitter.SetInt64("Duration", duration);
  emitter.SetString("StatusCode", status_code);
  emitter.SetString("StatusMessage", status_msg);

  s.writer.Reset();
  if (core::GetTableFieldRaw(L, abs_rec_idx, "events")) {
    SerializeTableSpanEventsToJson(L, -1, s.writer, t.item);
    lua_pop(L, 1);
  } else {
    s.writer.StartArray();
    s.writer.EndArray();
  }
  emitter.SetString("Events", s.writer.Str());

  s.writer.Reset();
  if (core::GetTableFieldRaw(L, abs_rec_idx, "links")) {
    SerializeTableSpanLinksToJson(L, -1, s.writer, t.item);
    lua_pop(L, 1);
  } else {
    s.writer.StartArray();
    s.writer.EndArray();
  }
  emitter.SetString("Links", s.writer.Str());

  emitter.SetString("processed_at", processed_at);

  emitter.EndRecord();
  return !emitter.HasError();
}

}  // namespace

bool ExtractNativeTracesJson(OperationContext& ctx, core::OtelParser& parser,
                             simdjson::padded_string_view admitted) {
  NativeTracesScratch& s = Scratch();
  ResetWriters(s);
  struct TrimGuard {
    explicit TrimGuard(NativeTracesScratch& scratch) : s(scratch) {}
    TrimGuard(const TrimGuard&) = delete;
    TrimGuard& operator=(const TrimGuard&) = delete;
    ~TrimGuard() noexcept {
      TrimWriters(s);
      core::ResetServiceNameScratch();
    }
    NativeTracesScratch& s;
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
    if (!ExtractNativeTraceObjectJson(ctx, s, doc_obj, processed_at)) {
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
      if (!ExtractNativeTraceObjectJson(ctx, s, elem_obj, processed_at)) {
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

bool ExtractNativeTracesTable(OperationContext& ctx) {
  NativeTracesScratch& s = Scratch();
  TableScratch& t = TScratch();
  ResetWriters(s);
  core::ReserveScratchString(t.service, 128);
  core::ReserveScratchString(t.scope_name, 128);
  core::ReserveScratchString(t.scope_version, 64);
  core::ReserveScratchString(t.status_msg, 128);
  core::ReserveScratchString(t.item, 128);
  struct Guard {
    Guard(lua_State* l, int top, NativeTracesScratch& scratch, TableScratch& table)
        : L(l), entry_top(top), s(scratch), t(table) {}
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    ~Guard() noexcept {
      lua_settop(L, entry_top);
      TrimWriters(s);
      core::ResetScratchString(t.service);
      core::ResetScratchString(t.scope_name);
      core::ResetScratchString(t.scope_version);
      core::ResetScratchString(t.status_msg);
      core::ResetScratchString(t.item);
    }
    lua_State* L;
    int entry_top;
    NativeTracesScratch& s;
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
        if (HasNativeTraceFields(L, item_idx)) {
          if (!ExtractSingleNativeTraceTable(ctx, s, t, item_idx, processed_at)) {
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
    bool handled_wrapper = false;
    if (core::GetTableFieldRaw(L, abs_idx, "log", "trace")) {
      int inner_idx = lua_absindex(L, -1);
      if (lua_istable(L, inner_idx)) {
        const size_t inner_len = lua_rawlen(L, inner_idx);
        if (inner_len > 0) {
          handled_wrapper = true;
          if (!extract_array(inner_idx, inner_len)) {
            lua_pop(L, 1);
            return false;
          }
        } else if (HasNativeTraceFields(L, inner_idx)) {
          handled_wrapper = true;
          if (!ExtractSingleNativeTraceTable(ctx, s, t, inner_idx, processed_at)) {
            lua_pop(L, 1);
            return false;
          }
        }
      }
      lua_pop(L, 1);
    }
    if (!handled_wrapper && HasNativeTraceFields(L, abs_idx)) {
      if (!ExtractSingleNativeTraceTable(ctx, s, t, abs_idx, processed_at)) {
        return false;
      }
    }
  }

  emitter.Flush();
  return !emitter.HasError();
}

namespace {
constexpr core::SignalExtractors kNativeTracesExtractors{ExtractNativeTracesJson,
                                                         ExtractNativeTracesTable};
}

ExtractionResult ProcessNativeTraces(lua_State* L, std::string_view raw_json, int emit_idx) {
  return core::RunCompatJson(L, core::ModuleKind::Native, core::Signal::Traces,
                             kNativeTracesExtractors, raw_json, emit_idx);
}

ExtractionResult ProcessNativeTracesTable(lua_State* L, int table_idx, int emit_idx) {
  return core::RunCompatTable(L, core::ModuleKind::Native, core::Signal::Traces,
                              kNativeTracesExtractors, table_idx, emit_idx);
}

const core::SignalExtractors& NativeTracesExtractors() { return kNativeTracesExtractors; }

}  // namespace vector_otel::native_extractors
