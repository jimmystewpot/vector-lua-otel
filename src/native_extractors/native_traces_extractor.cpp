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

constexpr int kMaxJsonDepth = 16;

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

void WriteAnyValueJson(core::FastJsonWriter& writer, simdjson::ondemand::value& val,
                       int depth = 0) {
  if (depth >= kMaxJsonDepth) {
    writer.String("[max_depth_exceeded]");
    return;
  }
  auto type_res = val.type();
  if (type_res.error() != simdjson::SUCCESS) {
    writer.Null();
    return;
  }
  auto type = type_res.value();
  if (type == simdjson::ondemand::json_type::object) {
    auto obj_res = val.get_object();
    if (obj_res.error() != simdjson::SUCCESS) {
      writer.Null();
      return;
    }
    auto obj = obj_res.value();
    bool matched = false;
    bool has_plain_fields = false;
    for (auto field_res : obj) {
      if (field_res.error() != simdjson::SUCCESS) continue;
      auto k_res = field_res.unescaped_key();
      if (k_res.error() != simdjson::SUCCESS) continue;
      std::string_view k = k_res.value();
      auto v_res = field_res.value();
      if (v_res.error() != simdjson::SUCCESS) continue;
      auto fv = v_res.value();

      if (!has_plain_fields) {
        if (k == "stringValue" || k == "string_value") {
          auto s_res = fv.get_string();
          if (s_res.error() == simdjson::SUCCESS) {
            writer.String(s_res.value());
            matched = true;
            break;
          }
        } else if (k == "intValue" || k == "int_value") {
          writer.Int64(core::ParseInt64(fv));
          matched = true;
          break;
        } else if (k == "uintValue" || k == "uint_value") {
          writer.Uint64(core::ParseUint64(fv));
          matched = true;
          break;
        } else if (k == "doubleValue" || k == "double_value") {
          writer.Double(core::ParseDouble(fv));
          matched = true;
          break;
        } else if (k == "boolValue" || k == "bool_value") {
          auto b_res = fv.get_bool();
          writer.Bool(b_res.error() == simdjson::SUCCESS && b_res.value());
          matched = true;
          break;
        } else if (k == "arrayValue" || k == "array_value" || k == "kvlistValue" ||
                   k == "kvlist_value" || k == "bytesValue" || k == "bytes_value") {
          auto raw_res = fv.raw_json();
          if (raw_res.error() == simdjson::SUCCESS) {
            writer.RawJson(core::TrimJsonWhitespace(raw_res.value()));
          } else {
            writer.Null();
          }
          matched = true;
          break;
        }
      }

      if (!has_plain_fields) {
        writer.StartObject();
        has_plain_fields = true;
      }
      writer.Key(k);
      WriteAnyValueJson(writer, fv, depth + 1);
    }
    if (!matched) {
      if (!has_plain_fields) {
        writer.StartObject();
      }
      writer.EndObject();
    }
  } else if (type == simdjson::ondemand::json_type::array) {
    auto arr_res = val.get_array();
    if (arr_res.error() != simdjson::SUCCESS) {
      writer.Null();
      return;
    }
    writer.StartArray();
    for (auto elem_res : arr_res.value()) {
      if (elem_res.error() != simdjson::SUCCESS) continue;
      auto elem_val = elem_res.value();
      WriteAnyValueJson(writer, elem_val, depth + 1);
    }
    writer.EndArray();
  } else if (type == simdjson::ondemand::json_type::string) {
    auto s_res = val.get_string();
    if (s_res.error() == simdjson::SUCCESS) {
      writer.String(s_res.value());
    } else {
      writer.Null();
    }
  } else if (type == simdjson::ondemand::json_type::number) {
    auto num_res = val.get_number();
    if (num_res.error() == simdjson::SUCCESS) {
      auto num = num_res.value();
      if (num.is_int64()) {
        writer.Int64(num.get_int64());
      } else if (num.is_uint64()) {
        writer.Uint64(num.get_uint64());
      } else {
        writer.Double(num.get_double());
      }
    } else {
      writer.Null();
    }
  } else if (type == simdjson::ondemand::json_type::boolean) {
    auto b_res = val.get_bool();
    writer.Bool(b_res.error() == simdjson::SUCCESS && b_res.value());
  } else {
    writer.Null();
  }
}

void WriteAttributesJson(core::FastJsonWriter& writer, simdjson::ondemand::value& attrs_val) {
  writer.StartObject();
  auto type_res = attrs_val.type();
  if (type_res.error() == simdjson::SUCCESS) {
    if (type_res.value() == simdjson::ondemand::json_type::array) {
      auto arr_res = attrs_val.get_array();
      if (arr_res.error() == simdjson::SUCCESS) {
        for (auto elem_res : arr_res.value()) {
          if (elem_res.error() != simdjson::SUCCESS) continue;
          auto item_val = elem_res.value();
          auto obj_res = item_val.get_object();
          if (obj_res.error() != simdjson::SUCCESS) continue;
          auto kv = obj_res.value();
          auto k_res = kv.find_field_unordered("key");
          if (k_res.error() != simdjson::SUCCESS) continue;
          auto k_str_res = k_res.value().get_string();
          if (k_str_res.error() != simdjson::SUCCESS) continue;
          std::string_view k = k_str_res.value();

          auto v_res = kv.find_field_unordered("value");
          if (v_res.error() != simdjson::SUCCESS) continue;
          auto v = v_res.value();

          writer.Key(k);
          WriteAnyValueJson(writer, v);
        }
      }
    } else if (type_res.value() == simdjson::ondemand::json_type::object) {
      auto obj_res = attrs_val.get_object();
      if (obj_res.error() == simdjson::SUCCESS) {
        for (auto field_res : obj_res.value()) {
          if (field_res.error() != simdjson::SUCCESS) continue;
          auto k_res = field_res.unescaped_key();
          if (k_res.error() != simdjson::SUCCESS) continue;
          auto v_res = field_res.value();
          if (v_res.error() != simdjson::SUCCESS) continue;
          auto fv = v_res.value();

          writer.Key(k_res.value());
          WriteAnyValueJson(writer, fv);
        }
      }
    }
  }
  writer.EndObject();
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
            WriteAttributesJson(attr_writer, fv);
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
            WriteAttributesJson(attr_writer, fv);
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

bool ExtractNativeTraceObjectJson(lua_State* L, simdjson::ondemand::object& rec_obj,
                                  std::string_view processed_at,
                                  core::FastJsonWriter& events_writer,
                                  core::FastJsonWriter& links_writer,
                                  core::FastJsonWriter& attr_writer, core::LuaEmitter& emitter,
                                  std::string& err_out, int depth = 0,
                                  bool* out_matched = nullptr) {
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
  bool has_recognized_field = false;

  ScopedLuaRef res_attrs_scoped(L, LUA_NOREF);
  ScopedLuaRef span_attrs_scoped(L, LUA_NOREF);

  events_writer.Reset();
  links_writer.Reset();

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

    if ((k == "log" || k == "trace") && depth == 0) {
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
        if (!ExtractNativeTraceObjectJson(L, inner_obj, processed_at, events_writer, links_writer,
                                          attr_writer, emitter, err_out, depth + 1,
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
          if (!ExtractNativeTraceObjectJson(L, elem_obj, processed_at, events_writer, links_writer,
                                            attr_writer, emitter, err_out, depth + 1,
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
      if (L != nullptr) {
        res_service_name = "unknown_service";
        core::PushAttributesTable(L, fv, attr_writer, &res_service_name);
        res_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
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
      if (L != nullptr) {
        core::PushAttributesTable(L, fv, attr_writer);
        span_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
      }
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

  emitter.StartRecord();
  emitter.SetString("Timestamp", timestamp_sv);
  emitter.SetString("TraceId", trace_id);
  emitter.SetString("SpanId", span_id);
  emitter.SetString("ParentSpanId", parent_span_id);
  emitter.SetString("TraceState", trace_state);
  emitter.SetString("SpanName", span_name);
  emitter.SetString("SpanKind", span_kind);
  emitter.SetString("ServiceName", service_name);

  if (L != nullptr) {
    if (res_attrs_scoped.get() != LUA_NOREF) {
      lua_rawgeti(L, LUA_REGISTRYINDEX, res_attrs_scoped.get());
      res_attrs_scoped.reset();
    } else {
      lua_newtable(L);
    }
    emitter.SetTableFromStack("ResourceAttributes");
  }

  emitter.SetString("ScopeName", scope_name);
  emitter.SetString("ScopeVersion", scope_version);

  if (L != nullptr) {
    if (span_attrs_scoped.get() != LUA_NOREF) {
      lua_rawgeti(L, LUA_REGISTRYINDEX, span_attrs_scoped.get());
      span_attrs_scoped.reset();
    } else {
      lua_newtable(L);
    }
    emitter.SetTableFromStack("SpanAttributes");
  }

  emitter.SetInt64("Duration", duration);
  emitter.SetString("StatusCode", status_code);
  emitter.SetString("StatusMessage", status_msg);
  emitter.SetString("Events", events_writer.Str());
  emitter.SetString("Links", links_writer.Str());
  emitter.SetString("processed_at", processed_at);

  emitter.EndRecord();
  if (emitter.HasError()) {
    err_out = std::string(emitter.ErrorMessage());
    return false;
  }
  return true;
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

void SerializeTableAttributesToJson(lua_State* L, int attrs_idx, core::FastJsonWriter& writer) {
  int abs_idx = lua_absindex(L, attrs_idx);
  writer.StartObject();
  if (!lua_istable(L, abs_idx)) {
    writer.EndObject();
    return;
  }

  bool is_kv_array = false;
  const size_t len = lua_rawlen(L, abs_idx);
  if (len > 0) {
    lua_rawgeti(L, abs_idx, 1);
    if (lua_istable(L, -1)) {
      if (core::GetTableFieldRaw(L, -1, "key")) {
        if (lua_type(L, -1) == LUA_TSTRING) {
          is_kv_array = true;
        }
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }

  if (is_kv_array) {
    for (size_t i = 1; i <= len; ++i) {
      lua_rawgeti(L, abs_idx, static_cast<lua_Integer>(i));
      if (lua_istable(L, -1)) {
        int item_idx = lua_absindex(L, -1);
        if (core::GetTableFieldRaw(L, item_idx, "key")) {
          if (lua_type(L, -1) == LUA_TSTRING) {
            size_t klen = 0;
            const char* ks = lua_tolstring(L, -1, &klen);
            if (klen > 0 && core::GetTableFieldRaw(L, item_idx, "value")) {
              writer.Key(std::string_view(ks, klen));
              core::SerializeLuaValueToJson(L, -1, writer);
              lua_pop(L, 1);
            }
          }
          lua_pop(L, 1);
        }
      }
      lua_pop(L, 1);
    }
  } else {
    lua_pushnil(L);
    while (lua_next(L, abs_idx) != 0) {
      if (lua_type(L, -2) == LUA_TSTRING) {
        size_t klen = 0;
        const char* ks = lua_tolstring(L, -2, &klen);
        writer.Key(std::string_view(ks, klen));
        core::SerializeLuaValueToJson(L, -1, writer);
      } else if (lua_type(L, -2) == LUA_TNUMBER) {
        lua_pushvalue(L, -2);
        size_t klen = 0;
        const char* ks = lua_tolstring(L, -1, &klen);
        writer.Key(std::string_view(ks, klen));
        lua_pop(L, 1);
        core::SerializeLuaValueToJson(L, -1, writer);
      }
      lua_pop(L, 1);
    }
  }

  writer.EndObject();
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
        SerializeTableAttributesToJson(L, -1, writer);
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
        SerializeTableAttributesToJson(L, -1, writer);
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

bool ExtractSingleNativeTraceTable(lua_State* L, int rec_idx, std::string_view processed_at,
                                   core::FastJsonWriter& writer, std::string& tls_service_scratch,
                                   std::string& tls_scope_name_scratch,
                                   std::string& tls_scope_ver_scratch,
                                   std::string& tls_status_msg_scratch,
                                   std::string& tls_item_scratch, core::LuaEmitter& emitter,
                                   std::string& err_out) {
  if (!lua_checkstack(L, 16)) {
    err_out = "Lua stack overflow during native trace extraction";
    return false;
  }
  int abs_rec_idx = lua_absindex(L, rec_idx);

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
  ScopedLuaRef res_attrs_scoped(L, LUA_NOREF);

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

  // 3. ScopeName & ScopeVersion
  std::string_view scope_name =
      core::ReadTableStringView(L, abs_rec_idx, "scope_name", "scopeName", tls_scope_name_scratch);
  std::string_view scope_version = core::ReadTableStringView(L, abs_rec_idx, "scope_version",
                                                             "scopeVersion", tls_scope_ver_scratch);
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
      status_msg = core::ReadTableStringView(L, st_idx, "message", nullptr, tls_status_msg_scratch);
    }
    lua_pop(L, 1);
  }
  if (status_code == "STATUS_CODE_UNSET" &&
      core::GetTableFieldRaw(L, abs_rec_idx, "status_code", "statusCode")) {
    status_code = ParseLuaStatusCodeValue(L, -1);
    lua_pop(L, 1);
  }
  if (status_msg.empty()) {
    status_msg = core::ReadTableStringView(L, abs_rec_idx, "status_message", "statusMessage",
                                           tls_status_msg_scratch);
  }

  // 5. Emit all 18 columns of traces.schema
  emitter.StartRecord();
  emitter.SetString("Timestamp", timestamp_sv);
  core::CopyStringFieldAlways(L, abs_rec_idx, "trace_id", "traceId", emitter, "TraceId");
  core::CopyStringFieldAlways(L, abs_rec_idx, "span_id", "spanId", emitter, "SpanId");
  core::CopyStringFieldAlways(L, abs_rec_idx, "parent_span_id", "parentSpanId", emitter,
                              "ParentSpanId");
  core::CopyStringFieldAlways(L, abs_rec_idx, "trace_state", "traceState", emitter, "TraceState");

  if (core::GetTableFieldRaw(L, abs_rec_idx, "name", "span_name")) {
    if (lua_isstring(L, -1)) {
      size_t len = 0;
      const char* s = lua_tolstring(L, -1, &len);
      emitter.SetString("SpanName", std::string_view(s, len));
    } else {
      emitter.SetString("SpanName", "");
    }
    lua_pop(L, 1);
  } else {
    core::CopyStringFieldAlways(L, abs_rec_idx, "spanName", nullptr, emitter, "SpanName");
  }

  emitter.SetString("SpanKind", span_kind);
  emitter.SetString("ServiceName", service_name);

  if (res_attrs_scoped.get() != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, res_attrs_scoped.get());
    res_attrs_scoped.reset();
  } else {
    lua_newtable(L);
  }
  emitter.SetTableFromStack("ResourceAttributes");

  emitter.SetString("ScopeName", scope_name);
  emitter.SetString("ScopeVersion", scope_version);

  bool has_span_attrs = core::GetTableFieldRaw(L, abs_rec_idx, "attributes", "span_attributes");
  if (!has_span_attrs) {
    has_span_attrs = core::GetTableFieldRaw(L, abs_rec_idx, "spanAttributes");
  }
  if (has_span_attrs) {
    core::PushTableAttributes(L, -1, writer);
    emitter.SetTableFromStack("SpanAttributes");
    lua_pop(L, 1);
  } else {
    lua_newtable(L);
    emitter.SetTableFromStack("SpanAttributes");
  }

  emitter.SetInt64("Duration", duration);
  emitter.SetString("StatusCode", status_code);
  emitter.SetString("StatusMessage", status_msg);

  writer.Reset();
  if (core::GetTableFieldRaw(L, abs_rec_idx, "events")) {
    SerializeTableSpanEventsToJson(L, -1, writer, tls_item_scratch);
    lua_pop(L, 1);
  } else {
    writer.StartArray();
    writer.EndArray();
  }
  emitter.SetString("Events", writer.Str());

  writer.Reset();
  if (core::GetTableFieldRaw(L, abs_rec_idx, "links")) {
    SerializeTableSpanLinksToJson(L, -1, writer, tls_item_scratch);
    lua_pop(L, 1);
  } else {
    writer.StartArray();
    writer.EndArray();
  }
  emitter.SetString("Links", writer.Str());

  emitter.SetString("processed_at", processed_at);

  emitter.EndRecord();
  if (emitter.HasError()) {
    err_out = std::string(emitter.ErrorMessage());
    return false;
  }
  return true;
}

}  // namespace

extractors::ExtractionResult ProcessNativeTraces(lua_State* L, std::string_view raw_json,
                                                 int emit_idx) {
  if (raw_json.empty()) {
    return ExtractionResult{false, "Empty JSON input", 0};
  }
  core::LuaEmitter emitter(L, emit_idx);
  try {
    auto& parser = core::OtelParser::Instance();
    static thread_local core::FastJsonWriter events_writer;
    static thread_local core::FastJsonWriter links_writer;
    static thread_local core::FastJsonWriter attr_writer;

    events_writer.Reset();
    links_writer.Reset();
    attr_writer.Reset();
    struct WriterTrimGuard {
      explicit WriterTrimGuard(core::FastJsonWriter& w1_in, core::FastJsonWriter& w2_in,
                               core::FastJsonWriter& w3_in)
          : w1(w1_in), w2(w2_in), w3(w3_in) {}
      WriterTrimGuard(const WriterTrimGuard&) = delete;
      WriterTrimGuard& operator=(const WriterTrimGuard&) = delete;
      ~WriterTrimGuard() noexcept {
        w1.Trim();
        w2.Trim();
        w3.Trim();
        core::ResetServiceNameScratch();
      }
      core::FastJsonWriter& w1;
      core::FastJsonWriter& w2;
      core::FastJsonWriter& w3;
    } trim_guard{events_writer, links_writer, attr_writer};

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
      if (!ExtractNativeTraceObjectJson(L, doc_obj, processed_at, events_writer, links_writer,
                                        attr_writer, emitter, err_out)) {
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
        if (!ExtractNativeTraceObjectJson(L, elem_obj, processed_at, events_writer, links_writer,
                                          attr_writer, emitter, err_out)) {
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
    return ExtractionResult{false, "Unknown C++ exception during native traces processing",
                            emitter.EmittedCount()};
  }
}

extractors::ExtractionResult ProcessNativeTracesTable(lua_State* L, int table_idx, int emit_idx) {
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
  static thread_local std::string tls_service_scratch;
  static thread_local std::string tls_scope_name_scratch;
  static thread_local std::string tls_scope_ver_scratch;
  static thread_local std::string tls_status_msg_scratch;
  static thread_local std::string tls_item_scratch;
  writer.Reset();
  core::ReserveScratchString(tls_service_scratch);
  core::ReserveScratchString(tls_scope_name_scratch);
  core::ReserveScratchString(tls_scope_ver_scratch);
  core::ReserveScratchString(tls_status_msg_scratch);
  core::ReserveScratchString(tls_item_scratch);
  struct TableScopeGuard {
    TableScopeGuard(lua_State* l, int t, core::FastJsonWriter& w) : L(l), top(t), writer(w) {}
    TableScopeGuard(const TableScopeGuard&) = delete;
    TableScopeGuard& operator=(const TableScopeGuard&) = delete;
    ~TableScopeGuard() noexcept {
      if (L != nullptr) {
        lua_settop(L, top);
      }
      writer.Trim();
      core::TrimScratchString(tls_service_scratch);
      core::TrimScratchString(tls_scope_name_scratch);
      core::TrimScratchString(tls_scope_ver_scratch);
      core::TrimScratchString(tls_status_msg_scratch);
      core::TrimScratchString(tls_item_scratch);
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
          if (HasNativeTraceFields(L, item_idx)) {
            if (!ExtractSingleNativeTraceTable(L, item_idx, processed_at, writer,
                                               tls_service_scratch, tls_scope_name_scratch,
                                               tls_scope_ver_scratch, tls_status_msg_scratch,
                                               tls_item_scratch, emitter, err_out)) {
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
      bool handled_wrapper = false;
      if (core::GetTableFieldRaw(L, abs_idx, "log", "trace")) {
        int inner_idx = lua_absindex(L, -1);
        if (lua_istable(L, inner_idx)) {
          const size_t inner_len = lua_rawlen(L, inner_idx);
          if (inner_len > 0) {
            handled_wrapper = true;
            if (!extract_array(inner_idx, inner_len)) {
              lua_pop(L, 1);
              return ExtractionResult{false, err_out, emitter.EmittedCount()};
            }
          } else if (HasNativeTraceFields(L, inner_idx)) {
            handled_wrapper = true;
            if (!ExtractSingleNativeTraceTable(L, inner_idx, processed_at, writer,
                                               tls_service_scratch, tls_scope_name_scratch,
                                               tls_scope_ver_scratch, tls_status_msg_scratch,
                                               tls_item_scratch, emitter, err_out)) {
              lua_pop(L, 1);
              return ExtractionResult{false, err_out, emitter.EmittedCount()};
            }
          }
        }
        lua_pop(L, 1);
      }
      if (!handled_wrapper && HasNativeTraceFields(L, abs_idx)) {
        if (!ExtractSingleNativeTraceTable(L, abs_idx, processed_at, writer, tls_service_scratch,
                                           tls_scope_name_scratch, tls_scope_ver_scratch,
                                           tls_status_msg_scratch, tls_item_scratch, emitter,
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
    return ExtractionResult{false, "Unknown C++ exception during native traces table processing",
                            emitter.EmittedCount()};
  }
}

}  // namespace vector_otel::native_extractors
