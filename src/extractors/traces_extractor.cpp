#include "vector_otel/extractors/traces_extractor.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "vector_otel/core/fast_json_writer.hpp"
#include "vector_otel/core/lua_bridge.hpp"
#include "vector_otel/core/otlp_common.hpp"
#include "vector_otel/core/parser.hpp"
#include "vector_otel/core/scratch.hpp"
#include "vector_otel/core/table_reader.hpp"
#include "vector_otel/core/timestamp.hpp"

namespace vector_otel::extractors {
namespace {

void WriteAnyValueJson(core::FastJsonWriter& writer, simdjson::ondemand::value& val) {
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
    for (auto field_res : obj) {
      if (field_res.error() != simdjson::SUCCESS) continue;
      auto k_res = field_res.unescaped_key();
      if (k_res.error() != simdjson::SUCCESS) continue;
      std::string_view k = k_res.value();
      auto v_res = field_res.value();
      if (v_res.error() != simdjson::SUCCESS) continue;
      auto fv = v_res.value();

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
          writer.RawJson(raw_res.value());
        } else {
          writer.Null();
        }
        matched = true;
        break;
      }
    }
    if (!matched) {
      writer.Null();
    }
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
          WriteAttributesJson(writer, aval);
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
          WriteAttributesJson(writer, aval);
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

}  // namespace

ExtractionResult ProcessTraces(lua_State* L, std::string_view raw_json, int emit_idx) {
  if (L != nullptr && !lua_checkstack(L, 32)) {
    return ExtractionResult{false, "Lua stack overflow in ProcessTraces", 0};
  }
  core::LuaEmitter emitter(L, emit_idx);
  try {
    auto& parser = core::OtelParser::Instance();
    static thread_local core::FastJsonWriter events_writer;
    static thread_local core::FastJsonWriter links_writer;
    static thread_local core::FastJsonWriter attr_writer;

    struct WriterTrimGuard {
      explicit WriterTrimGuard(core::FastJsonWriter& events_in, core::FastJsonWriter& links_in,
                               core::FastJsonWriter& attr_in)
          : events(events_in), links(links_in), attr(attr_in) {}
      WriterTrimGuard(const WriterTrimGuard&) = delete;
      WriterTrimGuard& operator=(const WriterTrimGuard&) = delete;
      ~WriterTrimGuard() noexcept {
        events.Trim();
        links.Trim();
        attr.Trim();
        core::ResetServiceNameScratch();
      }
      core::FastJsonWriter& events;
      core::FastJsonWriter& links;
      core::FastJsonWriter& attr;
    } trim_guard{events_writer, links_writer, attr_writer};

    events_writer.Reset();
    links_writer.Reset();
    attr_writer.Reset();

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

    auto res_spans_field = doc_obj.find_field_unordered("resourceSpans");
    if (res_spans_field.error() == simdjson::NO_SUCH_FIELD) {
      res_spans_field = doc_obj.find_field_unordered("resource_spans");
    }
    if (res_spans_field.error() == simdjson::NO_SUCH_FIELD) {
      return ExtractionResult{true, "", 0};
    }
    if (res_spans_field.error() != simdjson::SUCCESS) {
      return ExtractionResult{false, std::string(simdjson::error_message(res_spans_field.error())),
                              0};
    }
    auto res_spans_val = res_spans_field.value();
    auto res_spans_arr_res = res_spans_val.get_array();
    if (res_spans_arr_res.error() != simdjson::SUCCESS) {
      return ExtractionResult{false,
                              std::string(simdjson::error_message(res_spans_arr_res.error())), 0};
    }

    std::array<char, 32> proc_buf;
    core::FormatIso8601Nano(core::CurrentTimeUnixNano(), proc_buf.data());
    std::string_view processed_at(proc_buf.data(), 30);

    for (auto res_span_elem : res_spans_arr_res.value()) {
      if (res_span_elem.error() != simdjson::SUCCESS) {
        return ExtractionResult{false, std::string(simdjson::error_message(res_span_elem.error())),
                                emitter.EmittedCount()};
      }
      auto res_span_obj_res = res_span_elem.value().get_object();
      if (res_span_obj_res.error() != simdjson::SUCCESS) {
        if (res_span_obj_res.error() == simdjson::INCORRECT_TYPE) {
          continue;
        }
        return ExtractionResult{false,
                                std::string(simdjson::error_message(res_span_obj_res.error())),
                                emitter.EmittedCount()};
      }
      auto res_span_obj = res_span_obj_res.value();

      std::string_view service_name = "unknown_service";
      core::ScopedLuaRef res_attr_ref(L, LUA_NOREF);

      auto res_field = res_span_obj.find_field_unordered("resource");
      if (res_field.error() == simdjson::SUCCESS) {
        auto res_val = res_field.value();
        auto res_obj_res = res_val.get_object();
        if (res_obj_res.error() == simdjson::SUCCESS) {
          auto res_obj = res_obj_res.value();
          if (L != nullptr) {
            auto attrs_field = res_obj.find_field_unordered("attributes");
            if (attrs_field.error() == simdjson::SUCCESS) {
              auto attrs_val = attrs_field.value();
              core::PushAttributesTable(L, attrs_val, attr_writer, &service_name);
              res_attr_ref.reset(luaL_ref(L, LUA_REGISTRYINDEX));
            } else if (attrs_field.error() != simdjson::NO_SUCH_FIELD) {
              return ExtractionResult{false,
                                      std::string(simdjson::error_message(attrs_field.error())),
                                      emitter.EmittedCount()};
            }
          } else {
            service_name = core::ExtractServiceName(res_obj);
          }
        } else if (res_obj_res.error() != simdjson::INCORRECT_TYPE) {
          return ExtractionResult{false, std::string(simdjson::error_message(res_obj_res.error())),
                                  emitter.EmittedCount()};
        }
      } else if (res_field.error() != simdjson::NO_SUCH_FIELD) {
        return ExtractionResult{false, std::string(simdjson::error_message(res_field.error())),
                                emitter.EmittedCount()};
      }

      if (L != nullptr && !res_attr_ref.valid()) {
        lua_newtable(L);
        res_attr_ref.reset(luaL_ref(L, LUA_REGISTRYINDEX));
      }

      auto scope_spans_field = res_span_obj.find_field_unordered("scopeSpans");
      if (scope_spans_field.error() == simdjson::NO_SUCH_FIELD) {
        scope_spans_field = res_span_obj.find_field_unordered("scope_spans");
      }
      if (scope_spans_field.error() == simdjson::NO_SUCH_FIELD) {
        continue;
      }
      if (scope_spans_field.error() != simdjson::SUCCESS) {
        return ExtractionResult{false,
                                std::string(simdjson::error_message(scope_spans_field.error())),
                                emitter.EmittedCount()};
      }
      auto scope_spans_val = scope_spans_field.value();
      auto scope_spans_arr_res = scope_spans_val.get_array();
      if (scope_spans_arr_res.error() != simdjson::SUCCESS) {
        return ExtractionResult{false,
                                std::string(simdjson::error_message(scope_spans_arr_res.error())),
                                emitter.EmittedCount()};
      }

      for (auto scope_span_elem : scope_spans_arr_res.value()) {
        if (scope_span_elem.error() != simdjson::SUCCESS) {
          return ExtractionResult{false,
                                  std::string(simdjson::error_message(scope_span_elem.error())),
                                  emitter.EmittedCount()};
        }
        auto scope_span_obj_res = scope_span_elem.value().get_object();
        if (scope_span_obj_res.error() != simdjson::SUCCESS) {
          if (scope_span_obj_res.error() == simdjson::INCORRECT_TYPE) {
            continue;
          }
          return ExtractionResult{false,
                                  std::string(simdjson::error_message(scope_span_obj_res.error())),
                                  emitter.EmittedCount()};
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
          } else if (scope_obj_res.error() != simdjson::INCORRECT_TYPE) {
            return ExtractionResult{false,
                                    std::string(simdjson::error_message(scope_obj_res.error())),
                                    emitter.EmittedCount()};
          }
        } else if (scope_field.error() != simdjson::NO_SUCH_FIELD) {
          return ExtractionResult{false, std::string(simdjson::error_message(scope_field.error())),
                                  emitter.EmittedCount()};
        }

        auto spans_field = scope_span_obj.find_field_unordered("spans");
        if (spans_field.error() == simdjson::NO_SUCH_FIELD) {
          continue;
        }
        if (spans_field.error() != simdjson::SUCCESS) {
          return ExtractionResult{false, std::string(simdjson::error_message(spans_field.error())),
                                  emitter.EmittedCount()};
        }
        auto spans_val = spans_field.value();
        auto spans_arr_res = spans_val.get_array();
        if (spans_arr_res.error() != simdjson::SUCCESS) {
          return ExtractionResult{false,
                                  std::string(simdjson::error_message(spans_arr_res.error())),
                                  emitter.EmittedCount()};
        }

        for (auto span_elem : spans_arr_res.value()) {
          if (span_elem.error() != simdjson::SUCCESS) {
            return ExtractionResult{false, std::string(simdjson::error_message(span_elem.error())),
                                    emitter.EmittedCount()};
          }
          auto span_obj_res = span_elem.value().get_object();
          if (span_obj_res.error() != simdjson::SUCCESS) {
            if (span_obj_res.error() == simdjson::INCORRECT_TYPE) {
              continue;
            }
            return ExtractionResult{false,
                                    std::string(simdjson::error_message(span_obj_res.error())),
                                    emitter.EmittedCount()};
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
          core::ScopedLuaRef span_attrs_scoped(L, LUA_NOREF);

          core::IdHexBuffer trace_id_buf;
          core::IdHexBuffer span_id_buf;
          core::IdHexBuffer parent_span_id_buf;

          events_writer.Reset();
          links_writer.Reset();

          for (auto field_res : span_obj) {
            if (field_res.error() != simdjson::SUCCESS) {
              return ExtractionResult{false,
                                      std::string(simdjson::error_message(field_res.error())),
                                      emitter.EmittedCount()};
            }
            auto k_res = field_res.unescaped_key();
            if (k_res.error() != simdjson::SUCCESS) {
              return ExtractionResult{false, std::string(simdjson::error_message(k_res.error())),
                                      emitter.EmittedCount()};
            }
            std::string_view k = k_res.value();
            auto fv_res = field_res.value();
            if (fv_res.error() != simdjson::SUCCESS) {
              return ExtractionResult{false, std::string(simdjson::error_message(fv_res.error())),
                                      emitter.EmittedCount()};
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
              SerializeSpanEventsValue(fv, events_writer);
              has_events = true;
            } else if (k == "links") {
              SerializeSpanLinksValue(fv, links_writer);
              has_links = true;
            } else if (k == "attributes") {
              if (L != nullptr) {
                core::PushAttributesTable(L, fv, attr_writer);
                span_attrs_scoped = core::ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
                has_span_attrs = true;
              }
            }
          }

          if (!has_events) {
            events_writer.StartArray();
            events_writer.EndArray();
          }
          if (!has_links) {
            links_writer.StartArray();
            links_writer.EndArray();
          }

          uint64_t delta = (end_time >= start_time && start_time > 0) ? (end_time - start_time) : 0;
          constexpr uint64_t kMaxDuration =
              static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
          int64_t duration = (delta <= kMaxDuration) ? static_cast<int64_t>(delta)
                                                     : std::numeric_limits<int64_t>::max();
          std::array<char, 32> ts_buf;
          core::FormatIso8601Nano(start_time, ts_buf.data());

          emitter.StartRecord();
          emitter.SetString("Timestamp", std::string_view(ts_buf.data(), 30));
          emitter.SetString("TraceId", trace_id);
          emitter.SetString("SpanId", span_id);
          emitter.SetString("ParentSpanId", parent_span_id);
          emitter.SetString("TraceState", trace_state);
          emitter.SetString("SpanName", span_name);
          emitter.SetString("SpanKind", span_kind);
          emitter.SetString("ServiceName", service_name);

          if (L != nullptr) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, res_attr_ref.get());
            emitter.SetTableFromStack("ResourceAttributes");
          }

          emitter.SetString("ScopeName", scope_name);
          emitter.SetString("ScopeVersion", scope_version);

          if (L != nullptr) {
            if (has_span_attrs && span_attrs_scoped.get() != LUA_NOREF) {
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
  } catch (const std::exception& ex) {
    return ExtractionResult{false, ex.what(), emitter.EmittedCount()};
  } catch (...) {
    return ExtractionResult{false, "Unknown C++ exception during traces processing",
                            emitter.EmittedCount()};
  }
}

ExtractionResult ProcessTracesTable(lua_State* L, int table_idx, int emit_idx) {
  if (!lua_checkstack(L, 32)) {
    return ExtractionResult{false, "Lua stack overflow in ProcessTracesTable", 0};
  }
  const int entry_top = lua_gettop(L);
  static thread_local core::FastJsonWriter writer;
  static thread_local std::string tls_service_scratch;
  static thread_local std::string tls_scope_name_scratch;
  static thread_local std::string tls_scope_ver_scratch;
  static thread_local std::string tls_status_msg_scratch;
  static thread_local std::string span_kind_buf;
  static thread_local std::string status_code_buf;
  writer.Reset();
  core::ReserveScratchString(tls_service_scratch, 128);
  core::ReserveScratchString(tls_scope_name_scratch, 128);
  core::ReserveScratchString(tls_scope_ver_scratch, 64);
  core::ReserveScratchString(tls_status_msg_scratch, 128);
  core::ReserveScratchString(span_kind_buf, 64);
  core::ReserveScratchString(status_code_buf, 64);
  struct TableScopeGuard {
    TableScopeGuard(lua_State* l, int t, core::FastJsonWriter& w) : L(l), top(t), writer(w) {}
    TableScopeGuard(const TableScopeGuard&) = delete;
    TableScopeGuard& operator=(const TableScopeGuard&) = delete;
    ~TableScopeGuard() noexcept {
      if (L != nullptr) {
        lua_settop(L, top);
      }
      writer.Trim();
      core::ResetScratchString(tls_service_scratch);
      core::ResetScratchString(tls_scope_name_scratch);
      core::ResetScratchString(tls_scope_ver_scratch);
      core::ResetScratchString(tls_status_msg_scratch);
      core::ResetScratchString(span_kind_buf);
      core::ResetScratchString(status_code_buf);
    }
    lua_State* L;
    int top;
    core::FastJsonWriter& writer;
  } scope_guard{L, entry_top, writer};

  int abs_idx = lua_absindex(L, table_idx);
  int abs_emit_idx = lua_absindex(L, emit_idx);
  if (!lua_istable(L, abs_idx)) {
    return ExtractionResult{false, "payload must be a table", 0};
  }

  core::LuaEmitter emitter(L, abs_emit_idx);

  try {
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
      return ExtractionResult{true, "", 0};
    }

    std::array<char, 32> proc_buf;
    core::FormatIso8601Nano(core::CurrentTimeUnixNano(), proc_buf.data());
    std::string_view processed_at(proc_buf.data(), 30);

    lua_Integer rs_len = static_cast<lua_Integer>(lua_rawlen(L, rs_arr_idx));
    for (lua_Integer i = 1; i <= rs_len; ++i) {
      lua_rawgeti(L, rs_arr_idx, i);
      if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        continue;
      }
      int rs_obj_idx = lua_absindex(L, -1);

      std::string_view service_name = "unknown_service";
      core::ScopedLuaRef res_attr_ref(L, LUA_NOREF);

      if (core::GetTableField(L, rs_obj_idx, "resource")) {
        int res_idx = lua_absindex(L, -1);
        if (core::GetTableField(L, res_idx, "attributes")) {
          core::PushTableAttributes(L, -1, writer, &service_name);
          res_attr_ref.reset(luaL_ref(L, LUA_REGISTRYINDEX));
          lua_pop(L, 1);
        }
        lua_pop(L, 1);
      }
      if (!res_attr_ref.valid()) {
        lua_newtable(L);
        res_attr_ref.reset(luaL_ref(L, LUA_REGISTRYINDEX));
      }

      if (!core::GetTableField(L, rs_obj_idx, "scopeSpans", "scope_spans")) {
        lua_pop(L, 1);
        continue;
      }
      int ss_arr_idx = lua_absindex(L, -1);
      if (!lua_istable(L, ss_arr_idx)) {
        lua_pop(L, 2);
        continue;
      }

      lua_Integer ss_len = static_cast<lua_Integer>(lua_rawlen(L, ss_arr_idx));
      for (lua_Integer j = 1; j <= ss_len; ++j) {
        lua_rawgeti(L, ss_arr_idx, j);
        if (!lua_istable(L, -1)) {
          lua_pop(L, 1);
          continue;
        }
        int ss_obj_idx = lua_absindex(L, -1);

        std::string_view scope_name;
        std::string_view scope_version;
        if (core::GetTableField(L, ss_obj_idx, "scope")) {
          int s_idx = lua_absindex(L, -1);
          scope_name = core::ReadTableStringView(L, s_idx, "name", nullptr, tls_scope_name_scratch);
          scope_version =
              core::ReadTableStringView(L, s_idx, "version", nullptr, tls_scope_ver_scratch);
          lua_pop(L, 1);
        }

        if (core::GetTableField(L, ss_obj_idx, "spans")) {
          int spans_arr_idx = lua_absindex(L, -1);
          if (lua_istable(L, spans_arr_idx)) {
            lua_Integer spans_len = static_cast<lua_Integer>(lua_rawlen(L, spans_arr_idx));
            for (lua_Integer k = 1; k <= spans_len; ++k) {
              lua_rawgeti(L, spans_arr_idx, k);
              if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                continue;
              }
              int span_obj_idx = lua_absindex(L, -1);

              std::string_view span_kind = "SPAN_KIND_UNSPECIFIED";
              if (core::GetTableField(L, span_obj_idx, "kind")) {
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
                      span_kind = "SPAN_KIND_UNSPECIFIED";
                      break;
                  }
                } else if (lua_type(L, -1) == LUA_TSTRING) {
                  size_t slen = 0;
                  const char* s = lua_tolstring(L, -1, &slen);
                  std::string_view sv(s, slen);
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
                    span_kind_buf.assign(sv.data(), sv.size());
                    span_kind = span_kind_buf;
                  }
                }
                lua_pop(L, 1);
              }

              uint64_t start_time = core::ReadTableUint64(L, span_obj_idx, "startTimeUnixNano",
                                                          "start_time_unix_nano");
              uint64_t end_time =
                  core::ReadTableUint64(L, span_obj_idx, "endTimeUnixNano", "end_time_unix_nano");
              uint64_t delta =
                  (end_time >= start_time && start_time > 0) ? (end_time - start_time) : 0;
              constexpr uint64_t kMaxDuration =
                  static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
              int64_t duration = (delta <= kMaxDuration) ? static_cast<int64_t>(delta)
                                                         : std::numeric_limits<int64_t>::max();
              std::array<char, 32> ts_buf;
              core::FormatIso8601Nano(start_time, ts_buf.data());

              std::string_view status_code = "STATUS_CODE_UNSET";
              std::string_view status_msg;
              if (core::GetTableField(L, span_obj_idx, "status")) {
                int st_idx = lua_absindex(L, -1);
                if (core::GetTableField(L, st_idx, "code")) {
                  int64_t c = 0;
                  if (core::TryParseTableInt64At(L, -1, c)) {
                    if (c == 1) {
                      status_code = "STATUS_CODE_OK";
                    } else if (c == 2) {
                      status_code = "STATUS_CODE_ERROR";
                    } else {
                      status_code = "STATUS_CODE_UNSET";
                    }
                  } else if (lua_type(L, -1) == LUA_TSTRING) {
                    size_t slen = 0;
                    const char* s = lua_tolstring(L, -1, &slen);
                    std::string_view sv(s, slen);
                    if (sv == "OK" || sv == "STATUS_CODE_OK") {
                      status_code = "STATUS_CODE_OK";
                    } else if (sv == "ERROR" || sv == "STATUS_CODE_ERROR") {
                      status_code = "STATUS_CODE_ERROR";
                    } else if (sv == "UNSET" || sv == "STATUS_CODE_UNSET") {
                      status_code = "STATUS_CODE_UNSET";
                    } else {
                      status_code_buf.assign(sv.data(), sv.size());
                      status_code = status_code_buf;
                    }
                  }
                  lua_pop(L, 1);
                }
                status_msg = core::ReadTableStringView(L, st_idx, "message", nullptr,
                                                       tls_status_msg_scratch);
                lua_pop(L, 1);  // pop status table BEFORE StartRecord
              }

              emitter.StartRecord();
              emitter.SetString("Timestamp", std::string_view(ts_buf.data(), 30));
              core::CopyIdField(L, span_obj_idx, "traceId", "trace_id", core::kTraceIdBinaryLen,
                                emitter, "TraceId", "TraceId");
              core::CopyIdField(L, span_obj_idx, "spanId", "span_id", core::kSpanIdBinaryLen,
                                emitter, "SpanId", "SpanId");
              core::CopyIdField(L, span_obj_idx, "parentSpanId", "parent_span_id",
                                core::kSpanIdBinaryLen, emitter, "ParentSpanId", "ParentSpanId");
              core::CopyStringField(L, span_obj_idx, "traceState", "trace_state", emitter,
                                    "TraceState");
              core::CopyStringField(L, span_obj_idx, "name", nullptr, emitter, "SpanName");
              emitter.SetString("SpanKind", span_kind);
              emitter.SetString("ServiceName", service_name);

              lua_rawgeti(L, LUA_REGISTRYINDEX, res_attr_ref.get());
              emitter.SetTableFromStack("ResourceAttributes");

              emitter.SetString("ScopeName", scope_name);
              emitter.SetString("ScopeVersion", scope_version);

              if (core::GetTableField(L, span_obj_idx, "attributes")) {
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

              // Events & Links: serialises to JSON string (matching string
              // extractor output)
              writer.Reset();
              if (core::GetTableField(L, span_obj_idx, "events")) {
                core::SerializeTableSpanEventsToJson(L, -1, writer);
                lua_pop(L, 1);
              } else {
                writer.StartArray();
                writer.EndArray();
              }
              emitter.SetString("Events", writer.Str());

              writer.Reset();
              if (core::GetTableField(L, span_obj_idx, "links")) {
                core::SerializeTableSpanLinksToJson(L, -1, writer);
                lua_pop(L, 1);
              } else {
                writer.StartArray();
                writer.EndArray();
              }
              emitter.SetString("Links", writer.Str());

              emitter.SetString("processed_at", processed_at);

              emitter.EndRecord();
              lua_pop(L, 1);  // pop span_obj
              if (emitter.HasError()) {
                return ExtractionResult{false, std::string(emitter.ErrorMessage()),
                                        emitter.EmittedCount()};
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
    if (emitter.HasError()) {
      return ExtractionResult{false, std::string(emitter.ErrorMessage()), emitter.EmittedCount()};
    }
    return ExtractionResult{true, "", emitter.EmittedCount()};
  } catch (const std::exception& e) {
    return ExtractionResult{false, std::string("C++ exception: ") + e.what(),
                            emitter.EmittedCount()};
  } catch (...) {
    return ExtractionResult{false, "Unknown C++ exception", emitter.EmittedCount()};
  }
}

}  // namespace vector_otel::extractors
