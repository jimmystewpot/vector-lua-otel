#include "vector_otel/extractors/traces_extractor.hpp"

#include <array>
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

namespace vector_otel::extractors {
namespace {

void SerializeSpanEventsValue(simdjson::ondemand::value& events_val, core::FastJsonWriter& writer) {
  writer.StartArray();
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

        writer.StartObject();

        // Timestamp
        uint64_t time_nanos = 0;
        auto time_res = ev_obj.find_field_unordered("timeUnixNano");
        if (time_res.error() != simdjson::SUCCESS) {
          time_res = ev_obj.find_field_unordered("time_unix_nano");
        }
        if (time_res.error() == simdjson::SUCCESS) {
          auto tval = time_res.value();
          time_nanos = core::ParseUint64(tval);
        }
        writer.Key("Timestamp");
        std::array<char, 32> ev_ts_buf;
        core::FormatIso8601Nano(time_nanos, ev_ts_buf.data());
        writer.String(std::string_view(ev_ts_buf.data(), 30));

        // Name
        std::string_view name = core::ExtractStringField(ev_obj, "name", "");
        writer.Key("Name");
        writer.String(name);

        // Attributes
        writer.Key("Attributes");
        auto attrs_res = ev_obj.find_field_unordered("attributes");
        if (attrs_res.error() == simdjson::SUCCESS) {
          auto aval = attrs_res.value();
          core::WriteAttributesJson(writer, aval);
        } else {
          writer.StartObject();
          writer.EndObject();
        }

        writer.EndObject();
      }
    }
  }
  writer.EndArray();
}

void SerializeSpanLinksValue(simdjson::ondemand::value& links_val, core::FastJsonWriter& writer) {
  writer.StartArray();
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

        writer.StartObject();

        core::IdHexBuffer link_trace_id_buf;
        std::string_view raw_trace_id =
            core::ExtractStringField(link_obj, "traceId", "trace_id", "");
        std::string_view trace_id = core::NormalizeTraceId(raw_trace_id, link_trace_id_buf);
        writer.Key("TraceId");
        writer.String(trace_id);

        core::IdHexBuffer link_span_id_buf;
        std::string_view raw_span_id = core::ExtractStringField(link_obj, "spanId", "span_id", "");
        std::string_view span_id = core::NormalizeSpanId(raw_span_id, link_span_id_buf);
        writer.Key("SpanId");
        writer.String(span_id);

        std::string_view trace_state =
            core::ExtractStringField(link_obj, "traceState", "trace_state", "");
        writer.Key("TraceState");
        writer.String(trace_state);

        writer.Key("Attributes");
        auto attrs_res = link_obj.find_field_unordered("attributes");
        if (attrs_res.error() == simdjson::SUCCESS) {
          auto aval = attrs_res.value();
          core::WriteAttributesJson(writer, aval);
        } else {
          writer.StartObject();
          writer.EndObject();
        }

        writer.EndObject();
      }
    }
  }
  writer.EndArray();
}

std::string_view ParseSpanKindValue(simdjson::ondemand::value& kval) {
  auto ktype_res = kval.type();
  if (ktype_res.error() != simdjson::SUCCESS) {
    return "SPAN_KIND_UNSPECIFIED";
  }
  if (ktype_res.value() == simdjson::ondemand::json_type::number) {
    int64_t k = core::ParseInt64(kval);
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
  } else if (ktype_res.value() == simdjson::ondemand::json_type::string) {
    auto s_res = kval.get_string();
    if (s_res.error() == simdjson::SUCCESS) {
      std::string_view sv = s_res.value();
      if (sv.starts_with("SPAN_KIND_")) {
        return sv;
      }
      if (sv == "INTERNAL") return "SPAN_KIND_INTERNAL";
      if (sv == "SERVER") return "SPAN_KIND_SERVER";
      if (sv == "CLIENT") return "SPAN_KIND_CLIENT";
      if (sv == "PRODUCER") return "SPAN_KIND_PRODUCER";
      if (sv == "CONSUMER") return "SPAN_KIND_CONSUMER";
      if (sv == "UNSPECIFIED") return "SPAN_KIND_UNSPECIFIED";
      return sv;
    }
  }
  return "SPAN_KIND_UNSPECIFIED";
}

std::string_view ParseStatusCodeValue(simdjson::ondemand::value& code_val) {
  auto ctype_res = code_val.type();
  if (ctype_res.error() != simdjson::SUCCESS) {
    return "STATUS_CODE_UNSET";
  }
  if (ctype_res.value() == simdjson::ondemand::json_type::number) {
    int64_t c = core::ParseInt64(code_val);
    switch (c) {
      case 1:
        return "STATUS_CODE_OK";
      case 2:
        return "STATUS_CODE_ERROR";
      default:
        return "STATUS_CODE_UNSET";
    }
  } else if (ctype_res.value() == simdjson::ondemand::json_type::string) {
    auto s_res = code_val.get_string();
    if (s_res.error() == simdjson::SUCCESS) {
      std::string_view sv = s_res.value();
      if (sv.starts_with("STATUS_CODE_")) {
        return sv;
      }
      if (sv == "OK") return "STATUS_CODE_OK";
      if (sv == "ERROR") return "STATUS_CODE_ERROR";
      if (sv == "UNSET") return "STATUS_CODE_UNSET";
      return sv;
    }
  }
  return "STATUS_CODE_UNSET";
}

using core::ErrorCode;
using core::OperationContext;

// Reusable per-thread staging buffers: capacity is retained between invocations, so the steady
// state performs no C++ allocation per span.
struct TracesScratch {
  core::FastJsonWriter res_attrs_writer;   // ResourceAttributes JSON, built once per resource
  core::FastJsonWriter span_attrs_writer;  // SpanAttributes JSON, rebuilt per span
  core::FastJsonWriter events_writer;      // span events as a JSON string
  core::FastJsonWriter links_writer;       // span links as a JSON string
  core::StagedRow row;                     // the record being built
};

TracesScratch& Scratch() {
  static thread_local TracesScratch scratch;
  return scratch;
}

bool FailJson(OperationContext& ctx, simdjson::error_code code) {
  ctx.error.Set(ErrorCode::Json, simdjson::error_message(code));
  return false;
}

int64_t SpanDuration(uint64_t start_time, uint64_t end_time) {
  const uint64_t delta = (end_time >= start_time && start_time > 0) ? (end_time - start_time) : 0;
  constexpr uint64_t kMaxDuration = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  return (delta <= kMaxDuration) ? static_cast<int64_t>(delta)
                                 : std::numeric_limits<int64_t>::max();
}

}  // namespace

bool ExtractTracesJson(OperationContext& ctx, core::OtelParser& parser,
                       simdjson::padded_string_view admitted) {
  TracesScratch& s = Scratch();
  s.res_attrs_writer.Reset();
  s.span_attrs_writer.Reset();
  s.events_writer.Reset();
  s.links_writer.Reset();
  struct TrimGuard {
    explicit TrimGuard(TracesScratch& scratch) : s(scratch) {}
    TrimGuard(const TrimGuard&) = delete;
    TrimGuard& operator=(const TrimGuard&) = delete;
    ~TrimGuard() noexcept {
      s.res_attrs_writer.Trim();
      s.span_attrs_writer.Trim();
      s.events_writer.Trim();
      s.links_writer.Trim();
      core::ResetServiceNameScratch();
    }
    TracesScratch& s;
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

  auto res_spans_field = doc_obj.find_field_unordered("resourceSpans");
  if (res_spans_field.error() == simdjson::NO_SUCH_FIELD) {
    res_spans_field = doc_obj.find_field_unordered("resource_spans");
  }
  if (res_spans_field.error() == simdjson::NO_SUCH_FIELD) {
    return true;
  }
  if (res_spans_field.error() != simdjson::SUCCESS) {
    return FailJson(ctx, res_spans_field.error());
  }
  auto res_spans_val = res_spans_field.value();
  auto res_spans_arr_res = res_spans_val.get_array();
  if (res_spans_arr_res.error() != simdjson::SUCCESS) {
    return FailJson(ctx, res_spans_arr_res.error());
  }

  std::array<char, 32> proc_buf;
  core::FormatIso8601Nano(core::CurrentTimeUnixNano(), proc_buf.data());
  std::string_view processed_at(proc_buf.data(), 30);

  for (auto res_span_elem : res_spans_arr_res.value()) {
    if (res_span_elem.error() != simdjson::SUCCESS) {
      return FailJson(ctx, res_span_elem.error());
    }
    auto res_span_obj_res = res_span_elem.value().get_object();
    if (res_span_obj_res.error() != simdjson::SUCCESS) {
      continue;
    }
    auto res_span_obj = res_span_obj_res.value();

    // Resource service name and attributes (serialised once per resource)
    std::string_view service_name = "unknown_service";
    s.res_attrs_writer.Reset();
    bool has_res_attrs = false;
    auto res_field = res_span_obj.find_field_unordered("resource");
    if (res_field.error() == simdjson::SUCCESS) {
      auto res_val = res_field.value();
      auto res_obj_res = res_val.get_object();
      if (res_obj_res.error() == simdjson::SUCCESS) {
        auto res_obj = res_obj_res.value();
        auto attrs_field = res_obj.find_field_unordered("attributes");
        if (attrs_field.error() == simdjson::SUCCESS) {
          auto attrs_val = attrs_field.value();
          core::WriteAttributesJson(s.res_attrs_writer, attrs_val, &service_name);
          has_res_attrs = true;
        } else if (attrs_field.error() != simdjson::NO_SUCH_FIELD) {
          return FailJson(ctx, attrs_field.error());
        }
      }
    } else if (res_field.error() != simdjson::NO_SUCH_FIELD) {
      return FailJson(ctx, res_field.error());
    }
    const std::string_view res_attrs_json = has_res_attrs ? s.res_attrs_writer.Str() : "{}";

    auto scope_spans_field = res_span_obj.find_field_unordered("scopeSpans");
    if (scope_spans_field.error() == simdjson::NO_SUCH_FIELD) {
      scope_spans_field = res_span_obj.find_field_unordered("scope_spans");
    }
    if (scope_spans_field.error() == simdjson::NO_SUCH_FIELD) {
      continue;
    }
    if (scope_spans_field.error() != simdjson::SUCCESS) {
      return FailJson(ctx, scope_spans_field.error());
    }
    auto scope_spans_val = scope_spans_field.value();
    auto scope_spans_arr_res = scope_spans_val.get_array();
    if (scope_spans_arr_res.error() != simdjson::SUCCESS) {
      return FailJson(ctx, scope_spans_arr_res.error());
    }

    for (auto scope_span_elem : scope_spans_arr_res.value()) {
      if (scope_span_elem.error() != simdjson::SUCCESS) {
        return FailJson(ctx, scope_span_elem.error());
      }
      auto scope_span_obj_res = scope_span_elem.value().get_object();
      if (scope_span_obj_res.error() != simdjson::SUCCESS) {
        continue;
      }
      auto scope_span_obj = scope_span_obj_res.value();

      std::string_view scope_name;
      std::string_view scope_version;
      auto scope_field = scope_span_obj.find_field_unordered("scope");
      if (scope_field.error() == simdjson::SUCCESS) {
        auto scope_val = scope_field.value();
        auto scope_obj_res = scope_val.get_object();
        if (scope_obj_res.error() == simdjson::SUCCESS) {
          auto scope_obj = scope_obj_res.value();
          scope_name = core::ExtractStringField(scope_obj, "name", "");
          scope_version = core::ExtractStringField(scope_obj, "version", "");
        }
      } else if (scope_field.error() != simdjson::NO_SUCH_FIELD) {
        return FailJson(ctx, scope_field.error());
      }

      auto spans_field = scope_span_obj.find_field_unordered("spans");
      if (spans_field.error() == simdjson::NO_SUCH_FIELD) {
        continue;
      }
      if (spans_field.error() != simdjson::SUCCESS) {
        return FailJson(ctx, spans_field.error());
      }
      auto spans_val = spans_field.value();
      auto spans_arr_res = spans_val.get_array();
      if (spans_arr_res.error() != simdjson::SUCCESS) {
        return FailJson(ctx, spans_arr_res.error());
      }

      for (auto span_elem : spans_arr_res.value()) {
        if (span_elem.error() != simdjson::SUCCESS) {
          return FailJson(ctx, span_elem.error());
        }
        auto span_obj_res = span_elem.value().get_object();
        if (span_obj_res.error() != simdjson::SUCCESS) {
          continue;
        }
        auto span_obj = span_obj_res.value();

        std::string_view trace_id;
        std::string_view span_id;
        std::string_view parent_span_id;
        std::string_view trace_state;
        std::string_view span_name;
        std::string_view span_kind = "SPAN_KIND_UNSPECIFIED";
        uint64_t start_time = 0;
        uint64_t end_time = 0;
        std::string_view status_code = "STATUS_CODE_UNSET";
        std::string_view status_msg;
        bool has_events = false;
        bool has_links = false;
        bool has_span_attrs = false;

        core::IdHexBuffer trace_id_buf;
        core::IdHexBuffer span_id_buf;
        core::IdHexBuffer parent_span_id_buf;

        s.events_writer.Reset();
        s.links_writer.Reset();
        s.span_attrs_writer.Reset();

        emitter.StartRecord();

        for (auto field_res : span_obj) {
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

          if (k == "traceId" || k == "trace_id") {
            auto s_res = fv.get_string();
            if (s_res.error() == simdjson::SUCCESS) {
              trace_id = core::NormalizeTraceId(s_res.value(), trace_id_buf);
            }
          } else if (k == "spanId" || k == "span_id") {
            auto s_res = fv.get_string();
            if (s_res.error() == simdjson::SUCCESS) {
              span_id = core::NormalizeSpanId(s_res.value(), span_id_buf);
            }
          } else if (k == "parentSpanId" || k == "parent_span_id") {
            auto s_res = fv.get_string();
            if (s_res.error() == simdjson::SUCCESS) {
              parent_span_id = core::NormalizeSpanId(s_res.value(), parent_span_id_buf);
            }
          } else if (k == "traceState" || k == "trace_state") {
            auto s_res = fv.get_string();
            if (s_res.error() == simdjson::SUCCESS) {
              trace_state = s_res.value();
            }
          } else if (k == "name") {
            auto s_res = fv.get_string();
            if (s_res.error() == simdjson::SUCCESS) {
              span_name = s_res.value();
            }
          } else if (k == "kind") {
            span_kind = ParseSpanKindValue(fv);
          } else if (k == "startTimeUnixNano" || k == "start_time_unix_nano") {
            start_time = core::ParseUint64(fv);
          } else if (k == "endTimeUnixNano" || k == "end_time_unix_nano") {
            end_time = core::ParseUint64(fv);
          } else if (k == "status") {
            auto status_obj_res = fv.get_object();
            if (status_obj_res.error() == simdjson::SUCCESS) {
              auto status_obj = status_obj_res.value();
              for (auto st_res : status_obj) {
                if (st_res.error() != simdjson::SUCCESS) continue;
                auto stk_res = st_res.unescaped_key();
                if (stk_res.error() != simdjson::SUCCESS) continue;
                std::string_view stk = stk_res.value();
                auto stv_res = st_res.value();
                if (stv_res.error() != simdjson::SUCCESS) continue;
                auto stv = stv_res.value();
                if (stk == "code") {
                  status_code = ParseStatusCodeValue(stv);
                } else if (stk == "message") {
                  auto m_res = stv.get_string();
                  if (m_res.error() == simdjson::SUCCESS) {
                    status_msg = m_res.value();
                  }
                }
              }
            }
          } else if (k == "events") {
            SerializeSpanEventsValue(fv, s.events_writer);
            has_events = true;
          } else if (k == "links") {
            SerializeSpanLinksValue(fv, s.links_writer);
            has_links = true;
          } else if (k == "attributes") {
            s.span_attrs_writer.Reset();
            core::WriteAttributesJson(s.span_attrs_writer, fv);
            has_span_attrs = true;
          }
        }

        if (!has_events) {
          s.events_writer.StartArray();
          s.events_writer.EndArray();
        }
        if (!has_links) {
          s.links_writer.StartArray();
          s.links_writer.EndArray();
        }

        std::array<char, 32> ts_buf;
        core::FormatIso8601Nano(start_time, ts_buf.data());

        const std::string_view span_attrs_json = has_span_attrs ? s.span_attrs_writer.Str() : "{}";

        emitter.SetString("Timestamp", std::string_view(ts_buf.data(), 30));
        emitter.SetString("TraceId", trace_id);
        emitter.SetString("SpanId", span_id);
        emitter.SetString("ParentSpanId", parent_span_id);
        emitter.SetString("TraceState", trace_state);
        emitter.SetString("SpanName", span_name);
        emitter.SetString("SpanKind", span_kind);
        emitter.SetString("ServiceName", service_name);
        emitter.SetString("ResourceAttributes", res_attrs_json);
        emitter.SetString("ScopeName", scope_name);
        emitter.SetString("ScopeVersion", scope_version);
        emitter.SetString("SpanAttributes", span_attrs_json);
        emitter.SetInt64("Duration", SpanDuration(start_time, end_time));
        emitter.SetString("StatusCode", status_code);
        emitter.SetString("StatusMessage", status_msg);
        emitter.SetString("Events", s.events_writer.Str());
        emitter.SetString("Links", s.links_writer.Str());
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
  std::string scope_name;
  std::string scope_version;
  std::string status_msg;
  std::string span_kind;
  std::string status_code;
};

TableScratch& TScratch() {
  static thread_local TableScratch scratch;
  return scratch;
}

std::string_view TableSpanKind(lua_State* L, int span_obj_idx, std::string& buf) {
  std::string_view span_kind = "SPAN_KIND_UNSPECIFIED";
  if (!core::GetTableField(L, span_obj_idx, "kind")) {
    return span_kind;
  }
  int64_t ki = 0;
  if (core::TryParseTableInt64At(L, -1, ki)) {
    switch (ki) {
      case 1:
        span_kind = "SPAN_KIND_INTERNAL";
        break;
      case 2:
        span_kind = "SPAN_KIND_SERVER";
        break;
      case 3:
        span_kind = "SPAN_KIND_CLIENT";
        break;
      case 4:
        span_kind = "SPAN_KIND_PRODUCER";
        break;
      case 5:
        span_kind = "SPAN_KIND_CONSUMER";
        break;
      default:
        break;
    }
  } else if (lua_type(L, -1) == LUA_TSTRING) {
    size_t slen = 0;
    const char* str = lua_tolstring(L, -1, &slen);
    std::string_view sv(str, slen);
    if (sv == "INTERNAL" || sv == "SPAN_KIND_INTERNAL") {
      span_kind = "SPAN_KIND_INTERNAL";
    } else if (sv == "SERVER" || sv == "SPAN_KIND_SERVER") {
      span_kind = "SPAN_KIND_SERVER";
    } else if (sv == "CLIENT" || sv == "SPAN_KIND_CLIENT") {
      span_kind = "SPAN_KIND_CLIENT";
    } else if (sv == "PRODUCER" || sv == "SPAN_KIND_PRODUCER") {
      span_kind = "SPAN_KIND_PRODUCER";
    } else if (sv == "CONSUMER" || sv == "SPAN_KIND_CONSUMER") {
      span_kind = "SPAN_KIND_CONSUMER";
    } else if (sv == "UNSPECIFIED" || sv == "SPAN_KIND_UNSPECIFIED") {
      span_kind = "SPAN_KIND_UNSPECIFIED";
    } else {
      buf.assign(sv.data(), sv.size());
      span_kind = buf;
    }
  }
  lua_pop(L, 1);
  return span_kind;
}

void TableStatus(lua_State* L, int span_obj_idx, TableScratch& t, std::string_view& status_code,
                 std::string_view& status_msg) {
  if (!core::GetTableField(L, span_obj_idx, "status")) {
    return;
  }
  const int st_idx = lua_absindex(L, -1);
  if (core::GetTableField(L, st_idx, "code")) {
    int64_t c = 0;
    if (core::TryParseTableInt64At(L, -1, c)) {
      status_code = c == 1 ? "STATUS_CODE_OK" : c == 2 ? "STATUS_CODE_ERROR" : "STATUS_CODE_UNSET";
    } else if (lua_type(L, -1) == LUA_TSTRING) {
      size_t slen = 0;
      const char* str = lua_tolstring(L, -1, &slen);
      std::string_view sv(str, slen);
      if (sv == "OK" || sv == "STATUS_CODE_OK") {
        status_code = "STATUS_CODE_OK";
      } else if (sv == "ERROR" || sv == "STATUS_CODE_ERROR") {
        status_code = "STATUS_CODE_ERROR";
      } else if (sv == "UNSET" || sv == "STATUS_CODE_UNSET") {
        status_code = "STATUS_CODE_UNSET";
      } else {
        t.status_code.assign(sv.data(), sv.size());
        status_code = t.status_code;
      }
    }
    lua_pop(L, 1);
  }
  status_msg = core::ReadTableStringView(L, st_idx, "message", nullptr, t.status_msg);
  lua_pop(L, 1);  // pop status
}

// Table-mode extraction. Every Lua access here is a raw, allocation-free read (see read_keys.hpp):
// this frame is the dispatcher's outer frame and must never see a Lua error. Rows are staged and
// delivered by the protected emission leaves.
bool ExtractTracesTableImpl(OperationContext& ctx, TracesScratch& s, TableScratch& t,
                            std::string_view processed_at) {
  lua_State* L = ctx.L;
  core::RowEmitter emitter(ctx, s.row);
  const int abs_idx = lua_absindex(L, ctx.input_index);
  int rs_arr_idx = 0;

  if (core::GetTableField(L, abs_idx, "resourceSpans", "resource_spans")) {
    rs_arr_idx = lua_absindex(L, -1);
  } else if (core::GetTableField(L, abs_idx, "trace")) {
    int tr_idx = lua_absindex(L, -1);
    if (core::GetTableField(L, tr_idx, "resourceSpans", "resource_spans")) {
      lua_remove(L, tr_idx);
      rs_arr_idx = lua_absindex(L, -1);
    } else {
      lua_pop(L, 1);
    }
  } else if (core::GetTableField(L, abs_idx, "log")) {
    int log_idx = lua_absindex(L, -1);
    if (core::GetTableField(L, log_idx, "resourceSpans", "resource_spans")) {
      lua_remove(L, log_idx);
      rs_arr_idx = lua_absindex(L, -1);
    } else {
      lua_pop(L, 1);
    }
  }
  if (rs_arr_idx == 0) {
    rs_arr_idx = abs_idx;
  }
  if (!lua_istable(L, rs_arr_idx)) {
    return true;
  }

  const lua_Integer rs_len = static_cast<lua_Integer>(lua_rawlen(L, rs_arr_idx));
  for (lua_Integer i = 1; i <= rs_len; ++i) {
    lua_rawgeti(L, rs_arr_idx, i);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      continue;
    }
    const int rs_obj_idx = lua_absindex(L, -1);

    std::string_view service_name = "unknown_service";
    s.res_attrs_writer.Reset();
    bool has_res_attrs = false;
    if (core::GetTableField(L, rs_obj_idx, "resource")) {
      const int res_idx = lua_absindex(L, -1);
      if (core::GetTableField(L, res_idx, "attributes")) {
        core::WriteTableAttributesJson(L, -1, s.res_attrs_writer, &service_name);
        has_res_attrs = true;
        lua_pop(L, 1);
      }
      lua_pop(L, 1);
    }
    const std::string_view res_attrs_json = has_res_attrs ? s.res_attrs_writer.Str() : "{}";

    if (!core::GetTableField(L, rs_obj_idx, "scopeSpans", "scope_spans")) {
      lua_pop(L, 1);
      continue;
    }
    const int ss_arr_idx = lua_absindex(L, -1);
    if (!lua_istable(L, ss_arr_idx)) {
      lua_pop(L, 2);
      continue;
    }

    const lua_Integer ss_len = static_cast<lua_Integer>(lua_rawlen(L, ss_arr_idx));
    for (lua_Integer j = 1; j <= ss_len; ++j) {
      lua_rawgeti(L, ss_arr_idx, j);
      if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        continue;
      }
      const int ss_obj_idx = lua_absindex(L, -1);

      std::string_view scope_name;
      std::string_view scope_version;
      if (core::GetTableField(L, ss_obj_idx, "scope")) {
        const int s_idx = lua_absindex(L, -1);
        scope_name = core::ReadTableStringView(L, s_idx, "name", nullptr, t.scope_name);
        scope_version = core::ReadTableStringView(L, s_idx, "version", nullptr, t.scope_version);
        lua_pop(L, 1);
      }

      if (core::GetTableField(L, ss_obj_idx, "spans")) {
        const int spans_arr_idx = lua_absindex(L, -1);
        if (lua_istable(L, spans_arr_idx)) {
          const lua_Integer spans_len = static_cast<lua_Integer>(lua_rawlen(L, spans_arr_idx));
          for (lua_Integer k = 1; k <= spans_len; ++k) {
            lua_rawgeti(L, spans_arr_idx, k);
            if (!lua_istable(L, -1)) {
              lua_pop(L, 1);
              continue;
            }
            const int span_obj_idx = lua_absindex(L, -1);

            std::string_view span_kind = TableSpanKind(L, span_obj_idx, t.span_kind);
            const uint64_t start_time =
                core::ReadTableUint64(L, span_obj_idx, "startTimeUnixNano", "start_time_unix_nano");
            const uint64_t end_time =
                core::ReadTableUint64(L, span_obj_idx, "endTimeUnixNano", "end_time_unix_nano");
            std::array<char, 32> ts_buf;
            core::FormatIso8601Nano(start_time, ts_buf.data());

            std::string_view status_code = "STATUS_CODE_UNSET";
            std::string_view status_msg;
            TableStatus(L, span_obj_idx, t, status_code, status_msg);

            // Events & Links: serialised to JSON strings (matching the JSON extractor output).
            s.events_writer.Reset();
            if (core::GetTableField(L, span_obj_idx, "events")) {
              core::SerializeTableSpanEventsToJson(L, -1, s.events_writer);
              lua_pop(L, 1);
            } else {
              s.events_writer.StartArray();
              s.events_writer.EndArray();
            }
            s.links_writer.Reset();
            if (core::GetTableField(L, span_obj_idx, "links")) {
              core::SerializeTableSpanLinksToJson(L, -1, s.links_writer);
              lua_pop(L, 1);
            } else {
              s.links_writer.StartArray();
              s.links_writer.EndArray();
            }

            emitter.StartRecord();
            emitter.SetString("Timestamp", std::string_view(ts_buf.data(), 30));
            core::CopyIdField(L, span_obj_idx, "traceId", "trace_id", core::kTraceIdBinaryLen,
                              emitter, "TraceId", "TraceId");
            core::CopyIdField(L, span_obj_idx, "spanId", "span_id", core::kSpanIdBinaryLen, emitter,
                              "SpanId", "SpanId");
            core::CopyIdField(L, span_obj_idx, "parentSpanId", "parent_span_id",
                              core::kSpanIdBinaryLen, emitter, "ParentSpanId", "ParentSpanId");
            core::CopyStringField(L, span_obj_idx, "traceState", "trace_state", emitter,
                                  "TraceState");
            core::CopyStringField(L, span_obj_idx, "name", nullptr, emitter, "SpanName");
            emitter.SetString("SpanKind", span_kind);
            emitter.SetString("ServiceName", service_name);
            emitter.SetString("ResourceAttributes", res_attrs_json);
            emitter.SetString("ScopeName", scope_name);
            emitter.SetString("ScopeVersion", scope_version);
            s.span_attrs_writer.Reset();
            bool has_span_attrs = false;
            if (core::GetTableField(L, span_obj_idx, "attributes")) {
              core::WriteTableAttributesJson(L, -1, s.span_attrs_writer);
              has_span_attrs = true;
              lua_pop(L, 1);
            }
            emitter.SetString("SpanAttributes", has_span_attrs ? s.span_attrs_writer.Str() : "{}");
            emitter.SetInt64("Duration", SpanDuration(start_time, end_time));
            emitter.SetString("StatusCode", status_code);
            emitter.SetString("StatusMessage", status_msg);
            emitter.SetString("Events", s.events_writer.Str());
            emitter.SetString("Links", s.links_writer.Str());
            emitter.SetString("processed_at", processed_at);

            emitter.EndRecord();
            lua_pop(L, 1);  // pop span_obj
            if (emitter.HasError()) {
              return false;
            }
          }
        }
        lua_pop(L, 1);  // pop spans_arr
      }
      lua_pop(L, 1);  // pop ss_obj
    }
    lua_pop(L, 1);  // pop ss_arr
    lua_pop(L, 1);  // pop rs_obj
  }

  emitter.Flush();
  return !emitter.HasError();
}

}  // namespace

bool ExtractTracesTable(OperationContext& ctx) {
  TracesScratch& s = Scratch();
  TableScratch& t = TScratch();
  s.res_attrs_writer.Reset();
  s.span_attrs_writer.Reset();
  core::ReserveScratchString(t.scope_name, 128);
  core::ReserveScratchString(t.scope_version, 64);
  core::ReserveScratchString(t.status_msg, 128);
  core::ReserveScratchString(t.span_kind, 64);
  core::ReserveScratchString(t.status_code, 64);
  struct Guard {
    Guard(lua_State* l, int top, TracesScratch& scratch, TableScratch& table)
        : L(l), entry_top(top), s(scratch), t(table) {}
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    ~Guard() noexcept {
      lua_settop(L, entry_top);
      s.res_attrs_writer.Trim();
      s.span_attrs_writer.Trim();
      s.events_writer.Trim();
      s.links_writer.Trim();
      core::ResetScratchString(t.scope_name);
      core::ResetScratchString(t.scope_version);
      core::ResetScratchString(t.status_msg);
      core::ResetScratchString(t.span_kind);
      core::ResetScratchString(t.status_code);
    }
    lua_State* L;
    int entry_top;
    TracesScratch& s;
    TableScratch& t;
  } guard{ctx.L, lua_gettop(ctx.L), s, t};

  std::array<char, 32> proc_buf;
  core::FormatIso8601Nano(core::CurrentTimeUnixNano(), proc_buf.data());
  return ExtractTracesTableImpl(ctx, s, t, std::string_view(proc_buf.data(), 30));
}

// ---- compatibility entry points ---------------------------------------------------------------

namespace {
constexpr core::SignalExtractors kTracesExtractors{ExtractTracesJson, ExtractTracesTable};
}

ExtractionResult ProcessTraces(lua_State* L, std::string_view raw_json, int emit_idx) {
  return core::RunCompatJson(L, core::ModuleKind::Otel, core::Signal::Traces, kTracesExtractors,
                             raw_json, emit_idx);
}

ExtractionResult ProcessTracesTable(lua_State* L, int table_idx, int emit_idx) {
  return core::RunCompatTable(L, core::ModuleKind::Otel, core::Signal::Traces, kTracesExtractors,
                              table_idx, emit_idx);
}

const core::SignalExtractors& TracesExtractors() { return kTracesExtractors; }

}  // namespace vector_otel::extractors
