#include "vector_otel/extractors/metrics_extractor.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "vector_otel/core/fast_json_writer.hpp"
#include "vector_otel/core/lua_bridge.hpp"
#include "vector_otel/core/otlp_common.hpp"
#include "vector_otel/core/parser.hpp"
#include "vector_otel/core/scratch.hpp"
#include "vector_otel/core/table_reader.hpp"
#include "vector_otel/core/timestamp.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace vector_otel::extractors {
namespace {

lua_Integer ClampUint64ToLuaInt(uint64_t val) noexcept {
  constexpr uint64_t kMaxLuaInt = static_cast<uint64_t>(std::numeric_limits<lua_Integer>::max());
  return static_cast<lua_Integer>(std::min(val, kMaxLuaInt));
}

void SetDualIntField(lua_State* L, int tab_idx, const char* k_camel, const char* k_pascal,
                     lua_Integer v) {
  int abs_tab = lua_absindex(L, tab_idx);
  lua_pushinteger(L, v);
  lua_setfield(L, abs_tab, k_camel);
  lua_pushinteger(L, v);
  lua_setfield(L, abs_tab, k_pascal);
}

void SetDualDoubleField(lua_State* L, int tab_idx, const char* k_camel, const char* k_pascal,
                        double v) {
  int abs_tab = lua_absindex(L, tab_idx);
  lua_pushnumber(L, v);
  lua_setfield(L, abs_tab, k_camel);
  lua_pushnumber(L, v);
  lua_setfield(L, abs_tab, k_pascal);
}

void SetDualSubtableField(lua_State* L, int tab_idx, const char* k_camel, const char* k_pascal) {
  int abs_tab = lua_absindex(L, tab_idx);
  lua_pushvalue(L, -1);
  lua_setfield(L, abs_tab, k_pascal);
  lua_setfield(L, abs_tab, k_camel);
}

void PushRefOrEmptyTable(lua_State* L, core::ScopedLuaRef& ref) {
  if (ref.valid()) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref.get());
    ref.reset();
  } else {
    lua_newtable(L);
  }
}

bool TryGetTableSubtable(lua_State* L, int obj_idx, const char* k1, const char* k2 = nullptr) {
  if (core::GetTableField(L, obj_idx, k1, k2)) {
    if (lua_istable(L, -1)) {
      return true;
    }
    lua_pop(L, 1);
  }
  return false;
}

int64_t ParseAggregationTemporalityString(std::string_view sv) noexcept {
  if (sv == "AGGREGATION_TEMPORALITY_DELTA" || sv == "DELTA" || sv == "1") {
    return 1;
  }
  if (sv == "AGGREGATION_TEMPORALITY_CUMULATIVE" || sv == "CUMULATIVE" || sv == "2") {
    return 2;
  }
  return 0;
}

int64_t ExtractJsonAggregationTemporality(simdjson::ondemand::object& type_obj) {
  auto res = type_obj.find_field_unordered("aggregationTemporality");
  if (res.error() != simdjson::SUCCESS) {
    res = type_obj.find_field_unordered("aggregation_temporality");
  }
  if (res.error() != simdjson::SUCCESS) {
    return 0;
  }
  auto val = res.value();
  auto type_res = val.type();
  if (type_res.error() != simdjson::SUCCESS) {
    return 0;
  }
  if (type_res.value() == simdjson::ondemand::json_type::string) {
    auto s_res = val.get_string();
    if (s_res.error() == simdjson::SUCCESS) {
      return ParseAggregationTemporalityString(s_res.value());
    }
    return 0;
  }
  int64_t iv = core::ParseInt64(val);
  return iv >= 0 ? iv : 0;
}

int64_t ReadTableAggregationTemporality(lua_State* L, int type_obj_idx) {
  if (!core::GetTableField(L, type_obj_idx, "aggregationTemporality", "aggregation_temporality")) {
    return 0;
  }
  int64_t result = 0;
  if (lua_type(L, -1) == LUA_TSTRING) {
    size_t len = 0;
    const char* s = lua_tolstring(L, -1, &len);
    if (s != nullptr) {
      result = ParseAggregationTemporalityString(std::string_view(s, len));
    }
  } else {
    int64_t iv = 0;
    if (core::TryParseTableInt64At(L, -1, iv) && iv >= 0) {
      result = iv;
    }
  }
  lua_pop(L, 1);
  return result;
}

int ParseJsonBucketCountsTable(lua_State* L, simdjson::ondemand::value& fv) {
  if (L == nullptr || !lua_checkstack(L, 8)) {
    return LUA_NOREF;
  }
  lua_createtable(L, 16, 0);
  auto arr_res = fv.get_array();
  if (arr_res.error() == simdjson::SUCCESS) {
    lua_Integer bi = 1;
    for (auto b_elem : arr_res.value()) {
      if (b_elem.error() == simdjson::SUCCESS) {
        auto b_val = b_elem.value();
        lua_pushinteger(L, ClampUint64ToLuaInt(core::ParseUint64(b_val)));
        lua_rawseti(L, -2, bi++);
      }
    }
  }
  return luaL_ref(L, LUA_REGISTRYINDEX);
}

int ParseJsonExplicitBoundsTable(lua_State* L, simdjson::ondemand::value& fv) {
  if (L == nullptr || !lua_checkstack(L, 8)) {
    return LUA_NOREF;
  }
  lua_createtable(L, 16, 0);
  auto arr_res = fv.get_array();
  if (arr_res.error() == simdjson::SUCCESS) {
    lua_Integer bi = 1;
    for (auto eb_elem : arr_res.value()) {
      if (eb_elem.error() == simdjson::SUCCESS) {
        auto eb_val = eb_elem.value();
        lua_pushnumber(L, core::ParseDouble(eb_val));
        lua_rawseti(L, -2, bi++);
      }
    }
  }
  return luaL_ref(L, LUA_REGISTRYINDEX);
}

int ParseJsonQuantileValuesTable(lua_State* L, simdjson::ondemand::value& fv) {
  if (L == nullptr || !lua_checkstack(L, 8)) {
    return LUA_NOREF;
  }
  lua_createtable(L, 8, 0);
  auto q_arr_res = fv.get_array();
  if (q_arr_res.error() == simdjson::SUCCESS) {
    lua_Integer qi = 1;
    for (auto q_elem : q_arr_res.value()) {
      if (q_elem.error() != simdjson::SUCCESS) continue;
      auto q_obj_res = q_elem.value().get_object();
      if (q_obj_res.error() != simdjson::SUCCESS) continue;
      double q_quantile = 0.0;
      double q_value = 0.0;
      for (auto qf_res : q_obj_res.value()) {
        if (qf_res.error() != simdjson::SUCCESS) continue;
        auto qk_res = qf_res.unescaped_key();
        if (qk_res.error() != simdjson::SUCCESS) continue;
        std::string_view qk = qk_res.value();
        auto qv_res = qf_res.value();
        if (qv_res.error() != simdjson::SUCCESS) continue;
        auto qfv = qv_res.value();
        if (qk == "quantile") {
          q_quantile = core::ParseDouble(qfv);
        } else if (qk == "value") {
          q_value = core::ParseDouble(qfv);
        }
      }
      lua_createtable(L, 0, 4);
      SetDualDoubleField(L, -1, "quantile", "Quantile", q_quantile);
      SetDualDoubleField(L, -1, "value", "Value", q_value);
      lua_rawseti(L, -2, qi++);
    }
  }
  return luaL_ref(L, LUA_REGISTRYINDEX);
}

void PushNormalizedTableBucketCounts(lua_State* L, int arr_idx) {
  int abs_idx = lua_absindex(L, arr_idx);
  if (!lua_istable(L, abs_idx)) {
    lua_newtable(L);
    return;
  }
  lua_Integer len = static_cast<lua_Integer>(lua_rawlen(L, abs_idx));
  lua_createtable(L, static_cast<int>(len > 0 ? len : 0), 0);
  for (lua_Integer i = 1; i <= len; ++i) {
    lua_rawgeti(L, abs_idx, i);
    uint64_t u_val = 0;
    lua_Integer clamped = 0;
    if (core::TryParseTableUint64At(L, -1, u_val)) {
      clamped = ClampUint64ToLuaInt(u_val);
    }
    lua_pop(L, 1);
    lua_pushinteger(L, clamped);
    lua_rawseti(L, -2, i);
  }
}

void PushNormalizedTableExplicitBounds(lua_State* L, int arr_idx) {
  int abs_idx = lua_absindex(L, arr_idx);
  if (!lua_istable(L, abs_idx)) {
    lua_newtable(L);
    return;
  }
  lua_Integer len = static_cast<lua_Integer>(lua_rawlen(L, abs_idx));
  lua_createtable(L, static_cast<int>(len > 0 ? len : 0), 0);
  for (lua_Integer i = 1; i <= len; ++i) {
    lua_rawgeti(L, abs_idx, i);
    double d_val = 0.0;
    (void)core::TryParseTableDoubleAt(L, -1, d_val);
    lua_pop(L, 1);
    lua_pushnumber(L, d_val);
    lua_rawseti(L, -2, i);
  }
}

bool ReadTableGenericMetricValue(lua_State* L, int obj_idx, bool& is_int, int64_t& i_out,
                                 double& d_out) {
  int abs_idx = lua_absindex(L, obj_idx);
  if (core::GetTableField(L, abs_idx, "asDouble", "as_double")) {
    bool ok = core::TryParseTableDoubleAt(L, -1, d_out);
    lua_pop(L, 1);
    if (ok) {
      is_int = false;
      return true;
    }
    return false;
  }
  if (core::GetTableField(L, abs_idx, "asInt", "as_int")) {
    bool ok = core::TryParseTableInt64At(L, -1, i_out);
    lua_pop(L, 1);
    if (ok) {
      is_int = true;
      return true;
    }
    return false;
  }
  if (core::GetTableField(L, abs_idx, "value", "Value")) {
    if (lua_isinteger(L, -1)) {
      i_out = static_cast<int64_t>(lua_tointeger(L, -1));
      is_int = true;
      lua_pop(L, 1);
      return true;
    }
    if (lua_type(L, -1) == LUA_TNUMBER) {
      bool ok = core::TryParseTableDoubleAt(L, -1, d_out);
      lua_pop(L, 1);
      if (ok) {
        is_int = false;
        return true;
      }
      return false;
    }
    if (lua_type(L, -1) == LUA_TSTRING) {
      if (core::TryParseTableInt64At(L, -1, i_out)) {
        is_int = true;
        lua_pop(L, 1);
        return true;
      }
      if (core::TryParseTableDoubleAt(L, -1, d_out)) {
        is_int = false;
        lua_pop(L, 1);
        return true;
      }
    }
    lua_pop(L, 1);
    return false;
  }
  return false;
}

int ParseExemplars(lua_State* L, simdjson::ondemand::value& val,
                   core::FastJsonWriter& json_writer) {
  if (L == nullptr || !lua_checkstack(L, 12)) {
    return LUA_NOREF;
  }
  auto arr_res = val.get_array();
  if (arr_res.error() != simdjson::SUCCESS) {
    return LUA_NOREF;
  }
  lua_newtable(L);  // array table for exemplars
  int ex_idx = 1;
  for (auto elem_res : arr_res.value()) {
    if (elem_res.error() != simdjson::SUCCESS) continue;
    auto elem_val = elem_res.value();
    auto ex_obj_res = elem_val.get_object();
    if (ex_obj_res.error() != simdjson::SUCCESS) continue;
    auto ex_obj = ex_obj_res.value();

    lua_newtable(L);  // single exemplar table

    std::array<char, 32> time_iso_buf;
    size_t time_iso_len = 0;
    std::string_view span_id;
    std::string_view trace_id;
    core::IdHexBuffer span_id_buf;
    core::IdHexBuffer trace_id_buf;
    enum class ExValKind { kNone, kDouble, kInt64 } ex_val_kind = ExValKind::kNone;
    double ex_double_val = 0.0;
    int64_t ex_int_val = 0;
    core::ScopedLuaRef filtered_attrs_ref(L, LUA_NOREF);

    for (auto field_res : ex_obj) {
      if (field_res.error() != simdjson::SUCCESS) continue;
      auto k_res = field_res.unescaped_key();
      if (k_res.error() != simdjson::SUCCESS) continue;
      std::string_view k = k_res.value();
      auto v_res = field_res.value();
      if (v_res.error() != simdjson::SUCCESS) continue;
      auto fv = v_res.value();

      if (k == "timeUnixNano" || k == "time_unix_nano") {
        uint64_t nanos = core::ParseUint64(fv);
        if (nanos > 0) {
          core::FormatIso8601Nano(nanos, time_iso_buf.data());
          time_iso_len = 30;
        }
      } else if (k == "spanId" || k == "span_id") {
        auto s_res = fv.get_string();
        if (s_res.error() == simdjson::SUCCESS) {
          span_id = s_res.value();
        }
      } else if (k == "traceId" || k == "trace_id") {
        auto s_res = fv.get_string();
        if (s_res.error() == simdjson::SUCCESS) {
          trace_id = s_res.value();
        }
      } else if (k == "asDouble" || k == "as_double" || k == "value") {
        if (core::TryParseDouble(fv, ex_double_val)) {
          ex_val_kind = ExValKind::kDouble;
        }
      } else if (k == "asInt" || k == "as_int") {
        if (core::TryParseInt64(fv, ex_int_val)) {
          ex_val_kind = ExValKind::kInt64;
        }
      } else if (k == "filteredAttributes" || k == "filtered_attributes") {
        core::PushAttributesTable(L, fv, json_writer);
        filtered_attrs_ref.reset(luaL_ref(L, LUA_REGISTRYINDEX));
      }
    }

    // Set FilteredAttributes
    if (filtered_attrs_ref.valid()) {
      lua_rawgeti(L, LUA_REGISTRYINDEX, filtered_attrs_ref.get());
      filtered_attrs_ref.reset();
    } else {
      lua_newtable(L);
    }
    lua_setfield(L, -2, "FilteredAttributes");

    // Set TimeUnix
    lua_pushlstring(L, time_iso_buf.data(), time_iso_len);
    lua_setfield(L, -2, "TimeUnix");

    // Set Value
    if (ex_val_kind == ExValKind::kDouble) {
      lua_pushnumber(L, ex_double_val);
      lua_setfield(L, -2, "Value");
    } else if (ex_val_kind == ExValKind::kInt64) {
      lua_pushinteger(L, static_cast<lua_Integer>(ex_int_val));
      lua_setfield(L, -2, "Value");
    }

    // Set SpanId
    std::string_view norm_span_id = core::NormalizeSpanId(span_id, span_id_buf);
    lua_pushlstring(L, norm_span_id.data(), norm_span_id.size());
    lua_setfield(L, -2, "SpanId");

    // Set TraceId
    std::string_view norm_trace_id = core::NormalizeTraceId(trace_id, trace_id_buf);
    lua_pushlstring(L, norm_trace_id.data(), norm_trace_id.size());
    lua_setfield(L, -2, "TraceId");

    // Put into exemplars array table at ex_idx
    lua_rawseti(L, -2, ex_idx++);
  }

  return luaL_ref(L, LUA_REGISTRYINDEX);
}

}  // namespace

ExtractionResult ProcessMetrics(lua_State* L, std::string_view raw_json, int emit_idx) {
  if (raw_json.empty()) {
    return ExtractionResult{false, "Empty JSON input", 0};
  }
  if (L != nullptr && !lua_checkstack(L, 32)) {
    return ExtractionResult{false, "Lua stack overflow", 0};
  }

  auto& parser = core::OtelParser::Instance();
  static thread_local core::FastJsonWriter json_writer;
  json_writer.Reset();
  struct WriterTrimGuard {
    explicit WriterTrimGuard(core::FastJsonWriter& w) : writer(w) {}
    WriterTrimGuard(const WriterTrimGuard&) = delete;
    WriterTrimGuard& operator=(const WriterTrimGuard&) = delete;
    ~WriterTrimGuard() noexcept {
      writer.Trim();
      core::ResetServiceNameScratch();
    }
    core::FastJsonWriter& writer;
  } writer_trim_guard{json_writer};

  auto doc_res = parser.Iterate(raw_json);
  if (doc_res.error() != simdjson::SUCCESS) {
    return ExtractionResult{
        false, "JSON parsing error: " + std::string(simdjson::error_message(doc_res.error())), 0};
  }

  auto doc = std::move(doc_res.value());
  auto root_obj_res = doc.get_object();
  if (root_obj_res.error() != simdjson::SUCCESS) {
    return ExtractionResult{false, "Expected root JSON object", 0};
  }
  auto root_obj = root_obj_res.value();

  core::LuaEmitter emitter(L, emit_idx);

  auto check_structural_error = [&](simdjson::error_code err, ExtractionResult& out_err) -> bool {
    if (err != simdjson::SUCCESS && err != simdjson::NO_SUCH_FIELD) {
      out_err = ExtractionResult{false,
                                 std::string("JSON parsing error: ") + simdjson::error_message(err),
                                 emitter.EmittedCount()};
      return true;
    }
    return false;
  };

  try {
    ExtractionResult struct_err{};
    auto res_metrics_res = root_obj.find_field_unordered("resourceMetrics");
    if (res_metrics_res.error() == simdjson::NO_SUCH_FIELD) {
      res_metrics_res = root_obj.find_field_unordered("resource_metrics");
    }
    if (check_structural_error(res_metrics_res.error(), struct_err)) {
      return struct_err;
    }
    if (res_metrics_res.error() != simdjson::SUCCESS) {
      return ExtractionResult{true, "", 0};
    }

    auto res_metrics_val = res_metrics_res.value();
    auto res_metrics_arr_res = res_metrics_val.get_array();
    if (res_metrics_arr_res.error() != simdjson::SUCCESS) {
      if (res_metrics_arr_res.error() != simdjson::INCORRECT_TYPE) {
        return ExtractionResult{false,
                                std::string("JSON parsing error: ") +
                                    simdjson::error_message(res_metrics_arr_res.error()),
                                emitter.EmittedCount()};
      }
      return ExtractionResult{true, "", 0};
    }

    std::array<char, 32> processed_at_buf;
    core::FormatIso8601Nano(core::CurrentTimeUnixNano(), processed_at_buf.data());
    std::string_view processed_at(processed_at_buf.data(), 30);

    for (auto res_metric_elem : res_metrics_arr_res.value()) {
      if (res_metric_elem.error() != simdjson::SUCCESS) {
        return ExtractionResult{
            false,
            std::string("JSON parsing error: ") + simdjson::error_message(res_metric_elem.error()),
            emitter.EmittedCount()};
      }
      auto res_metric_obj_res = res_metric_elem.value().get_object();
      if (res_metric_obj_res.error() != simdjson::SUCCESS) {
        if (res_metric_obj_res.error() != simdjson::INCORRECT_TYPE) {
          return ExtractionResult{false,
                                  std::string("JSON parsing error: ") +
                                      simdjson::error_message(res_metric_obj_res.error()),
                                  emitter.EmittedCount()};
        }
        continue;
      }
      auto res_metric_obj = res_metric_obj_res.value();

      std::string_view service_name = "unknown_service";
      core::ScopedLuaRef res_attrs_ref(L, LUA_NOREF);

      auto resource_field_res = res_metric_obj.find_field_unordered("resource");
      if (check_structural_error(resource_field_res.error(), struct_err)) {
        return struct_err;
      }
      if (resource_field_res.error() == simdjson::SUCCESS) {
        auto res_val = resource_field_res.value();
        auto res_obj_res = res_val.get_object();
        if (res_obj_res.error() == simdjson::SUCCESS) {
          auto res_obj = res_obj_res.value();
          if (L != nullptr) {
            auto attrs_res = res_obj.find_field_unordered("attributes");
            if (check_structural_error(attrs_res.error(), struct_err)) {
              return struct_err;
            }
            if (attrs_res.error() == simdjson::SUCCESS) {
              auto attrs_val = attrs_res.value();
              core::PushAttributesTable(L, attrs_val, json_writer, &service_name);
              res_attrs_ref.reset(luaL_ref(L, LUA_REGISTRYINDEX));
            }
          } else {
            service_name = core::ExtractServiceName(res_obj);
          }
        }
      }

      if (L != nullptr && !res_attrs_ref.valid()) {
        lua_newtable(L);
        res_attrs_ref.reset(luaL_ref(L, LUA_REGISTRYINDEX));
      }

      auto scope_metrics_res = res_metric_obj.find_field_unordered("scopeMetrics");
      if (scope_metrics_res.error() == simdjson::NO_SUCH_FIELD) {
        scope_metrics_res = res_metric_obj.find_field_unordered("scope_metrics");
      }
      if (check_structural_error(scope_metrics_res.error(), struct_err)) {
        return struct_err;
      }
      if (scope_metrics_res.error() != simdjson::SUCCESS) {
        continue;
      }

      auto scope_metrics_arr_res = scope_metrics_res.value().get_array();
      if (scope_metrics_arr_res.error() != simdjson::SUCCESS) {
        if (scope_metrics_arr_res.error() != simdjson::INCORRECT_TYPE) {
          return ExtractionResult{false,
                                  std::string("JSON parsing error: ") +
                                      simdjson::error_message(scope_metrics_arr_res.error()),
                                  emitter.EmittedCount()};
        }
        continue;
      }

      for (auto scope_metric_elem : scope_metrics_arr_res.value()) {
        if (scope_metric_elem.error() != simdjson::SUCCESS) {
          return ExtractionResult{false,
                                  std::string("JSON parsing error: ") +
                                      simdjson::error_message(scope_metric_elem.error()),
                                  emitter.EmittedCount()};
        }
        auto scope_metric_obj_res = scope_metric_elem.value().get_object();
        if (scope_metric_obj_res.error() != simdjson::SUCCESS) {
          if (scope_metric_obj_res.error() != simdjson::INCORRECT_TYPE) {
            return ExtractionResult{false,
                                    std::string("JSON parsing error: ") +
                                        simdjson::error_message(scope_metric_obj_res.error()),
                                    emitter.EmittedCount()};
          }
          continue;
        }
        auto scope_metric_obj = scope_metric_obj_res.value();

        std::string_view scope_name;
        std::string_view scope_version;
        core::ScopedLuaRef scope_attrs_ref(L, LUA_NOREF);

        auto scope_field_res = scope_metric_obj.find_field_unordered("scope");
        if (check_structural_error(scope_field_res.error(), struct_err)) {
          return struct_err;
        }
        if (scope_field_res.error() == simdjson::SUCCESS) {
          auto scope_val = scope_field_res.value();
          auto scope_obj_res = scope_val.get_object();
          if (scope_obj_res.error() == simdjson::SUCCESS) {
            auto scope_obj = scope_obj_res.value();
            scope_name = core::ExtractStringField(scope_obj, "name");
            scope_version = core::ExtractStringField(scope_obj, "version");
            auto attrs_res = scope_obj.find_field_unordered("attributes");
            if (check_structural_error(attrs_res.error(), struct_err)) {
              return struct_err;
            }
            if (attrs_res.error() == simdjson::SUCCESS && L != nullptr) {
              auto attrs_val = attrs_res.value();
              core::PushAttributesTable(L, attrs_val, json_writer);
              scope_attrs_ref.reset(luaL_ref(L, LUA_REGISTRYINDEX));
            }
          }
        }

        if (L != nullptr && !scope_attrs_ref.valid()) {
          lua_newtable(L);
          scope_attrs_ref.reset(luaL_ref(L, LUA_REGISTRYINDEX));
        }

        auto metrics_res = scope_metric_obj.find_field_unordered("metrics");
        if (check_structural_error(metrics_res.error(), struct_err)) {
          return struct_err;
        }
        if (metrics_res.error() != simdjson::SUCCESS) {
          continue;
        }

        auto metrics_arr_res = metrics_res.value().get_array();
        if (metrics_arr_res.error() != simdjson::SUCCESS) {
          if (metrics_arr_res.error() != simdjson::INCORRECT_TYPE) {
            return ExtractionResult{false,
                                    std::string("JSON parsing error: ") +
                                        simdjson::error_message(metrics_arr_res.error()),
                                    emitter.EmittedCount()};
          }
          continue;
        }

        for (auto metric_elem : metrics_arr_res.value()) {
          if (metric_elem.error() != simdjson::SUCCESS) {
            return ExtractionResult{
                false,
                std::string("JSON parsing error: ") + simdjson::error_message(metric_elem.error()),
                emitter.EmittedCount()};
          }
          auto metric_obj_res = metric_elem.value().get_object();
          if (metric_obj_res.error() != simdjson::SUCCESS) {
            if (metric_obj_res.error() != simdjson::INCORRECT_TYPE) {
              return ExtractionResult{false,
                                      std::string("JSON parsing error: ") +
                                          simdjson::error_message(metric_obj_res.error()),
                                      emitter.EmittedCount()};
            }
            continue;
          }
          auto metric_obj = metric_obj_res.value();

          std::string_view metric_name;
          std::string_view metric_desc;
          std::string_view metric_unit;
          std::string_view metric_type;
          std::string_view matched_type_key;
          int best_type_rank = 99;

          for (auto m_field_res : metric_obj) {
            if (m_field_res.error() != simdjson::SUCCESS) {
              return ExtractionResult{false,
                                      std::string("JSON parsing error: ") +
                                          simdjson::error_message(m_field_res.error()),
                                      emitter.EmittedCount()};
            }
            auto mk_res = m_field_res.unescaped_key();
            if (mk_res.error() != simdjson::SUCCESS) {
              return ExtractionResult{
                  false,
                  std::string("JSON parsing error: ") + simdjson::error_message(mk_res.error()),
                  emitter.EmittedCount()};
            }
            std::string_view mk = mk_res.value();
            auto mv_res = m_field_res.value();
            if (mv_res.error() != simdjson::SUCCESS) {
              return ExtractionResult{
                  false,
                  std::string("JSON parsing error: ") + simdjson::error_message(mv_res.error()),
                  emitter.EmittedCount()};
            }
            auto mfv = mv_res.value();

            if (mk == "name") {
              auto s_res = mfv.get_string();
              if (s_res.error() == simdjson::SUCCESS) {
                metric_name = s_res.value();
              }
            } else if (mk == "description") {
              auto s_res = mfv.get_string();
              if (s_res.error() == simdjson::SUCCESS) {
                metric_desc = s_res.value();
              }
            } else if (mk == "unit") {
              auto s_res = mfv.get_string();
              if (s_res.error() == simdjson::SUCCESS) {
                metric_unit = s_res.value();
              }
            } else {
              int rank = 99;
              std::string_view candidate_type;
              if (mk == "gauge") {
                rank = 1;
                candidate_type = "gauge";
              } else if (mk == "sum") {
                rank = 2;
                candidate_type = "sum";
              } else if (mk == "histogram") {
                rank = 3;
                candidate_type = "histogram";
              } else if (mk == "exponentialHistogram" || mk == "exponential_histogram") {
                rank = 4;
                candidate_type = "exponential_histogram";
              } else if (mk == "summary") {
                rank = 5;
                candidate_type = "summary";
              }
              if (rank < best_type_rank) {
                auto mt_res = mfv.type();
                if (mt_res.error() != simdjson::SUCCESS) {
                  return ExtractionResult{
                      false,
                      std::string("JSON parsing error: ") + simdjson::error_message(mt_res.error()),
                      emitter.EmittedCount()};
                }
                if (mt_res.value() == simdjson::ondemand::json_type::object) {
                  best_type_rank = rank;
                  matched_type_key = mk;
                  metric_type = candidate_type;
                }
              }
            }
          }

          if (metric_type.empty() || matched_type_key.empty()) {
            continue;
          }

          auto type_field_res = metric_obj.find_field_unordered(matched_type_key);
          if (check_structural_error(type_field_res.error(), struct_err)) {
            return struct_err;
          }
          if (type_field_res.error() != simdjson::SUCCESS) {
            continue;
          }
          auto type_obj_res = type_field_res.value().get_object();
          if (type_obj_res.error() != simdjson::SUCCESS) {
            if (type_obj_res.error() != simdjson::INCORRECT_TYPE) {
              return ExtractionResult{false,
                                      std::string("JSON parsing error: ") +
                                          simdjson::error_message(type_obj_res.error()),
                                      emitter.EmittedCount()};
            }
            continue;
          }
          auto type_obj = type_obj_res.value();

          bool is_sum = (metric_type == "sum");
          bool is_histogram = (metric_type == "histogram");
          bool is_exp_histogram = (metric_type == "exponential_histogram");
          bool is_summary = (metric_type == "summary");

          int64_t agg_temporality = 0;
          if (is_sum || is_histogram || is_exp_histogram) {
            agg_temporality = ExtractJsonAggregationTemporality(type_obj);
          }

          bool has_is_monotonic = false;
          bool is_monotonic = false;
          if (is_sum) {
            auto mono_res = type_obj.find_field_unordered("isMonotonic");
            if (mono_res.error() != simdjson::SUCCESS) {
              mono_res = type_obj.find_field_unordered("is_monotonic");
            }
            if (mono_res.error() == simdjson::SUCCESS) {
              auto b_res = mono_res.value().get_bool();
              if (b_res.error() == simdjson::SUCCESS) {
                is_monotonic = b_res.value();
                has_is_monotonic = true;
              }
            }
          }

          auto dps_res = type_obj.find_field_unordered("dataPoints");
          if (dps_res.error() == simdjson::NO_SUCH_FIELD) {
            dps_res = type_obj.find_field_unordered("data_points");
          }
          if (check_structural_error(dps_res.error(), struct_err)) {
            return struct_err;
          }
          if (dps_res.error() != simdjson::SUCCESS) continue;

          auto dps_arr_res = dps_res.value().get_array();
          if (dps_arr_res.error() != simdjson::SUCCESS) {
            if (dps_arr_res.error() != simdjson::INCORRECT_TYPE) {
              return ExtractionResult{false,
                                      std::string("JSON parsing error: ") +
                                          simdjson::error_message(dps_arr_res.error()),
                                      emitter.EmittedCount()};
            }
            continue;
          }

          for (auto dp_elem : dps_arr_res.value()) {
            if (dp_elem.error() != simdjson::SUCCESS) {
              return ExtractionResult{
                  false,
                  std::string("JSON parsing error: ") + simdjson::error_message(dp_elem.error()),
                  emitter.EmittedCount()};
            }
            auto dp_obj_res = dp_elem.value().get_object();
            if (dp_obj_res.error() != simdjson::SUCCESS) {
              if (dp_obj_res.error() != simdjson::INCORRECT_TYPE) {
                return ExtractionResult{false,
                                        std::string("JSON parsing error: ") +
                                            simdjson::error_message(dp_obj_res.error()),
                                        emitter.EmittedCount()};
              }
              continue;
            }
            auto dp_obj = dp_obj_res.value();

            std::array<char, 32> start_ts_buf;
            size_t start_ts_len = 0;
            std::array<char, 32> time_ts_buf;
            size_t time_ts_len = 0;
            enum class ValueKind { kNone, kDouble, kInt64 };
            ValueKind val_kind = ValueKind::kNone;
            double double_val = 0.0;
            int64_t int_val = 0;
            uint32_t flags = 0;
            core::ScopedLuaRef dp_attrs_ref(L, LUA_NOREF);
            core::ScopedLuaRef exemplars_ref(L, LUA_NOREF);
            core::ScopedLuaRef bucket_counts_ref(L, LUA_NOREF);
            core::ScopedLuaRef explicit_bounds_ref(L, LUA_NOREF);
            core::ScopedLuaRef pos_bucket_counts_ref(L, LUA_NOREF);
            core::ScopedLuaRef neg_bucket_counts_ref(L, LUA_NOREF);
            core::ScopedLuaRef quantiles_ref(L, LUA_NOREF);

            // Shared Histogram / ExponentialHistogram / Summary fields
            uint64_t dp_count = 0;
            double dp_sum = 0.0;
            bool has_sum = false;
            double dp_min = 0.0;
            bool has_min = false;
            double dp_max = 0.0;
            bool has_max = false;
            int64_t exp_scale = 0;
            uint64_t exp_zero_count = 0;
            double exp_zero_threshold = 0.0;
            int64_t pos_offset = 0;
            int64_t neg_offset = 0;

            for (auto field_res : dp_obj) {
              if (field_res.error() != simdjson::SUCCESS) {
                return ExtractionResult{false,
                                        std::string("JSON parsing error: ") +
                                            simdjson::error_message(field_res.error()),
                                        emitter.EmittedCount()};
              }
              auto k_res = field_res.unescaped_key();
              if (k_res.error() != simdjson::SUCCESS) {
                return ExtractionResult{
                    false,
                    std::string("JSON parsing error: ") + simdjson::error_message(k_res.error()),
                    emitter.EmittedCount()};
              }
              std::string_view k = k_res.value();
              auto val_field_res = field_res.value();
              if (val_field_res.error() != simdjson::SUCCESS) {
                return ExtractionResult{false,
                                        std::string("JSON parsing error: ") +
                                            simdjson::error_message(val_field_res.error()),
                                        emitter.EmittedCount()};
              }
              auto fv = val_field_res.value();
              auto fv_type_res = fv.type();
              if (fv_type_res.error() != simdjson::SUCCESS) {
                return ExtractionResult{false,
                                        std::string("JSON parsing error: ") +
                                            simdjson::error_message(fv_type_res.error()),
                                        emitter.EmittedCount()};
              }

              if (k == "startTimeUnixNano" || k == "start_time_unix_nano") {
                uint64_t nanos = core::ParseUint64(fv);
                if (nanos > 0) {
                  core::FormatIso8601Nano(nanos, start_ts_buf.data());
                  start_ts_len = 30;
                }
              } else if (k == "timeUnixNano" || k == "time_unix_nano") {
                uint64_t nanos = core::ParseUint64(fv);
                if (nanos > 0) {
                  core::FormatIso8601Nano(nanos, time_ts_buf.data());
                  time_ts_len = 30;
                }
              } else if (k == "asDouble" || k == "as_double") {
                if (core::TryParseDouble(fv, double_val)) {
                  val_kind = ValueKind::kDouble;
                }
              } else if (k == "asInt" || k == "as_int") {
                if (core::TryParseInt64(fv, int_val)) {
                  val_kind = ValueKind::kInt64;
                }
              } else if (k == "flags") {
                flags = static_cast<uint32_t>(core::ParseUint64(fv));
              } else if (k == "attributes") {
                if (L != nullptr) {
                  core::PushAttributesTable(L, fv, json_writer);
                  dp_attrs_ref.reset(luaL_ref(L, LUA_REGISTRYINDEX));
                }
              } else if (k == "count") {
                dp_count = core::ParseUint64(fv);
              } else if (k == "sum") {
                if (core::TryParseDouble(fv, dp_sum)) {
                  has_sum = true;
                }
              } else if (k == "min") {
                if (core::TryParseDouble(fv, dp_min)) {
                  has_min = true;
                }
              } else if (k == "max") {
                if (core::TryParseDouble(fv, dp_max)) {
                  has_max = true;
                }
              } else if (k == "scale") {
                exp_scale = core::ParseInt64(fv);
              } else if (k == "zeroCount" || k == "zero_count") {
                exp_zero_count = core::ParseUint64(fv);
              } else if (k == "zeroThreshold" || k == "zero_threshold") {
                (void)core::TryParseDouble(fv, exp_zero_threshold);
              } else if (k == "positive") {
                auto pos_obj_res = fv.get_object();
                if (pos_obj_res.error() == simdjson::SUCCESS) {
                  for (auto pf_res : pos_obj_res.value()) {
                    if (pf_res.error() != simdjson::SUCCESS) continue;
                    auto pk_res = pf_res.unescaped_key();
                    if (pk_res.error() != simdjson::SUCCESS) continue;
                    std::string_view pk = pk_res.value();
                    auto pv_res = pf_res.value();
                    if (pv_res.error() != simdjson::SUCCESS) continue;
                    auto pfv = pv_res.value();
                    if (pk == "offset") {
                      pos_offset = core::ParseInt64(pfv);
                    } else if (pk == "bucketCounts" || pk == "bucket_counts") {
                      pos_bucket_counts_ref.reset(ParseJsonBucketCountsTable(L, pfv));
                    }
                  }
                }
              } else if (k == "negative") {
                auto neg_obj_res = fv.get_object();
                if (neg_obj_res.error() == simdjson::SUCCESS) {
                  for (auto nf_res : neg_obj_res.value()) {
                    if (nf_res.error() != simdjson::SUCCESS) continue;
                    auto nk_res = nf_res.unescaped_key();
                    if (nk_res.error() != simdjson::SUCCESS) continue;
                    std::string_view nk = nk_res.value();
                    auto nv_res = nf_res.value();
                    if (nv_res.error() != simdjson::SUCCESS) continue;
                    auto nfv = nv_res.value();
                    if (nk == "offset") {
                      neg_offset = core::ParseInt64(nfv);
                    } else if (nk == "bucketCounts" || nk == "bucket_counts") {
                      neg_bucket_counts_ref.reset(ParseJsonBucketCountsTable(L, nfv));
                    }
                  }
                }
              } else if (k == "quantileValues" || k == "quantile_values") {
                quantiles_ref.reset(ParseJsonQuantileValuesTable(L, fv));
              } else if (k == "bucketCounts" || k == "bucket_counts") {
                bucket_counts_ref.reset(ParseJsonBucketCountsTable(L, fv));
              } else if (k == "explicitBounds" || k == "explicit_bounds") {
                explicit_bounds_ref.reset(ParseJsonExplicitBoundsTable(L, fv));
              } else if (k == "exemplars") {
                if (L != nullptr) {
                  exemplars_ref.reset(ParseExemplars(L, fv, json_writer));
                }
              }
            }

            emitter.StartRecord();
            emitter.SetString("MetricName", metric_name);
            emitter.SetString("MetricDescription", metric_desc);
            emitter.SetString("MetricUnit", metric_unit);
            emitter.SetString("MetricType", metric_type);
            emitter.SetString("ServiceName", service_name);

            if (is_sum || is_histogram || is_exp_histogram) {
              emitter.SetInt64("AggregationTemporality", agg_temporality);
            }
            if (is_sum && has_is_monotonic) {
              emitter.SetBool("IsMonotonic", is_monotonic);
            }

            // ResourceAttributes
            if (L != nullptr && res_attrs_ref.valid()) {
              lua_rawgeti(L, LUA_REGISTRYINDEX, res_attrs_ref.get());
              emitter.SetTableFromStack("ResourceAttributes");
            }

            emitter.SetString("ScopeName", scope_name);
            emitter.SetString("ScopeVersion", scope_version);

            // ScopeAttributes
            if (L != nullptr && scope_attrs_ref.valid()) {
              lua_rawgeti(L, LUA_REGISTRYINDEX, scope_attrs_ref.get());
              emitter.SetTableFromStack("ScopeAttributes");
            }

            // Attributes
            if (L != nullptr) {
              PushRefOrEmptyTable(L, dp_attrs_ref);
              emitter.SetTableFromStack("Attributes");
            }

            emitter.SetString("StartTimeUnix", std::string_view(start_ts_buf.data(), start_ts_len));
            emitter.SetString("TimeUnix", std::string_view(time_ts_buf.data(), time_ts_len));

            // Value: set for gauge/sum, omit for histogram/exponential_histogram/summary
            if (!is_histogram && !is_exp_histogram && !is_summary) {
              if (val_kind == ValueKind::kDouble) {
                emitter.SetDouble("Value", double_val);
              } else if (val_kind == ValueKind::kInt64) {
                emitter.SetInt64("Value", int_val);
              }
            }

            // Histogram
            if (is_histogram && L != nullptr) {
              lua_createtable(L, 0, 14);
              SetDualIntField(L, -1, "aggregationTemporality", "AggregationTemporality",
                              static_cast<lua_Integer>(agg_temporality));
              SetDualIntField(L, -1, "count", "Count", ClampUint64ToLuaInt(dp_count));
              SetDualDoubleField(L, -1, "sum", "Sum", has_sum ? dp_sum : 0.0);
              if (has_min) {
                SetDualDoubleField(L, -1, "min", "Min", dp_min);
              }
              if (has_max) {
                SetDualDoubleField(L, -1, "max", "Max", dp_max);
              }

              PushRefOrEmptyTable(L, bucket_counts_ref);
              SetDualSubtableField(L, -2, "bucketCounts", "BucketCounts");

              PushRefOrEmptyTable(L, explicit_bounds_ref);
              SetDualSubtableField(L, -2, "explicitBounds", "ExplicitBounds");

              emitter.SetTableFromStack("Histogram");
            }

            // ExponentialHistogram
            if (is_exp_histogram && L != nullptr) {
              lua_createtable(L, 0, 20);
              SetDualIntField(L, -1, "aggregationTemporality", "AggregationTemporality",
                              static_cast<lua_Integer>(agg_temporality));
              SetDualIntField(L, -1, "count", "Count", ClampUint64ToLuaInt(dp_count));
              if (has_sum) {
                SetDualDoubleField(L, -1, "sum", "Sum", dp_sum);
              }
              if (has_min) {
                SetDualDoubleField(L, -1, "min", "Min", dp_min);
              }
              if (has_max) {
                SetDualDoubleField(L, -1, "max", "Max", dp_max);
              }
              SetDualIntField(L, -1, "scale", "Scale", static_cast<lua_Integer>(exp_scale));
              SetDualIntField(L, -1, "zeroCount", "ZeroCount", ClampUint64ToLuaInt(exp_zero_count));
              SetDualDoubleField(L, -1, "zeroThreshold", "ZeroThreshold", exp_zero_threshold);

              lua_createtable(L, 0, 4);
              SetDualIntField(L, -1, "offset", "Offset", static_cast<lua_Integer>(pos_offset));
              PushRefOrEmptyTable(L, pos_bucket_counts_ref);
              SetDualSubtableField(L, -2, "bucketCounts", "BucketCounts");
              SetDualSubtableField(L, -2, "positive", "Positive");

              lua_createtable(L, 0, 4);
              SetDualIntField(L, -1, "offset", "Offset", static_cast<lua_Integer>(neg_offset));
              PushRefOrEmptyTable(L, neg_bucket_counts_ref);
              SetDualSubtableField(L, -2, "bucketCounts", "BucketCounts");
              SetDualSubtableField(L, -2, "negative", "Negative");

              emitter.SetTableFromStack("ExponentialHistogram");
            }

            // Summary
            if (is_summary && L != nullptr) {
              lua_createtable(L, 0, 6);
              SetDualIntField(L, -1, "count", "Count", ClampUint64ToLuaInt(dp_count));
              SetDualDoubleField(L, -1, "sum", "Sum", has_sum ? dp_sum : 0.0);
              PushRefOrEmptyTable(L, quantiles_ref);
              SetDualSubtableField(L, -2, "quantileValues", "QuantileValues");
              emitter.SetTableFromStack("Summary");
            }

            emitter.SetInt64("Flags", flags);

            // Exemplars
            if (L != nullptr && exemplars_ref.valid()) {
              lua_rawgeti(L, LUA_REGISTRYINDEX, exemplars_ref.get());
              exemplars_ref.reset();
              emitter.SetTableFromStack("Exemplars");
            }

            emitter.SetString("processed_at", processed_at);

            emitter.EndRecord();
            if (emitter.HasError()) {
              return ExtractionResult{false, std::string(emitter.ErrorMessage()),
                                      emitter.EmittedCount()};
            }
          }
        }
      }
    }

    emitter.Flush();
    if (emitter.HasError()) {
      return ExtractionResult{false, std::string(emitter.ErrorMessage()), emitter.EmittedCount()};
    }
    return ExtractionResult{true, "", emitter.EmittedCount()};
  } catch (const std::exception& e) {
    return ExtractionResult{false, std::string("Extraction exception: ") + e.what(),
                            emitter.EmittedCount()};
  } catch (...) {
    return ExtractionResult{false, "Unknown extraction exception", emitter.EmittedCount()};
  }
}

ExtractionResult ProcessMetricsTable(lua_State* L, int table_idx, int emit_idx) {
  const int entry_top = lua_gettop(L);
  int abs_idx = lua_absindex(L, table_idx);
  int abs_emit_idx = lua_absindex(L, emit_idx);
  if (!lua_istable(L, abs_idx)) {
    return ExtractionResult{false, "payload must be a table", 0};
  }
  if (!lua_checkstack(L, 32)) {
    return ExtractionResult{false, "Lua stack overflow", 0};
  }

  static thread_local core::FastJsonWriter json_writer;
  static thread_local std::string tls_service_scratch;
  static thread_local std::string tls_scope_name_scratch;
  static thread_local std::string tls_scope_ver_scratch;
  static thread_local std::string tls_metric_name_scratch;
  static thread_local std::string tls_metric_desc_scratch;
  static thread_local std::string tls_metric_unit_scratch;
  json_writer.Reset();
  core::ReserveScratchString(tls_service_scratch);
  core::ReserveScratchString(tls_scope_name_scratch);
  core::ReserveScratchString(tls_scope_ver_scratch);
  core::ReserveScratchString(tls_metric_name_scratch);
  core::ReserveScratchString(tls_metric_desc_scratch);
  core::ReserveScratchString(tls_metric_unit_scratch);
  struct TableExitGuard {
    TableExitGuard(lua_State* l, int t, core::FastJsonWriter& w) : L(l), top(t), writer(w) {}
    TableExitGuard(const TableExitGuard&) = delete;
    TableExitGuard& operator=(const TableExitGuard&) = delete;
    ~TableExitGuard() noexcept {
      if (L != nullptr) {
        lua_settop(L, top);
      }
      writer.Trim();
      core::ResetScratchString(tls_service_scratch);
      core::ResetScratchString(tls_scope_name_scratch);
      core::ResetScratchString(tls_scope_ver_scratch);
      core::ResetScratchString(tls_metric_name_scratch);
      core::ResetScratchString(tls_metric_desc_scratch);
      core::ResetScratchString(tls_metric_unit_scratch);
    }
    lua_State* L;
    int top;
    core::FastJsonWriter& writer;
  } exit_guard{L, entry_top, json_writer};

  core::LuaEmitter emitter(L, abs_emit_idx);

  try {
    int rm_arr_idx = 0;

    if (core::GetTableField(L, abs_idx, "resourceMetrics", "resource_metrics")) {
      rm_arr_idx = lua_absindex(L, -1);
    } else if (core::GetTableField(L, abs_idx, "metric")) {
      if (lua_istable(L, -1) && core::GetTableField(L, -1, "resourceMetrics", "resource_metrics")) {
        lua_remove(L, -2);
        rm_arr_idx = lua_absindex(L, -1);
      } else {
        lua_pop(L, 1);
      }
    } else if (core::GetTableField(L, abs_idx, "log")) {
      if (lua_istable(L, -1) && core::GetTableField(L, -1, "resourceMetrics", "resource_metrics")) {
        lua_remove(L, -2);
        rm_arr_idx = lua_absindex(L, -1);
      } else {
        lua_pop(L, 1);
      }
    }

    if (rm_arr_idx == 0) {
      rm_arr_idx = abs_idx;
    }

    if (!lua_istable(L, rm_arr_idx)) {
      return ExtractionResult{true, "", 0};
    }

    std::array<char, 32> processed_at_buf;
    core::FormatIso8601Nano(core::CurrentTimeUnixNano(), processed_at_buf.data());
    std::string_view processed_at(processed_at_buf.data(), 30);

    lua_Integer rm_len = static_cast<lua_Integer>(lua_rawlen(L, rm_arr_idx));
    for (lua_Integer i = 1; i <= rm_len; ++i) {
      lua_rawgeti(L, rm_arr_idx, i);
      if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        continue;
      }
      int rm_obj_idx = lua_absindex(L, -1);

      std::string_view service_name = "unknown_service";
      core::ScopedLuaRef res_attrs_ref(L, LUA_NOREF);

      if (core::GetTableField(L, rm_obj_idx, "resource")) {
        int res_idx = lua_absindex(L, -1);
        if (core::GetTableField(L, res_idx, "attributes")) {
          core::PushTableAttributes(L, -1, json_writer, &service_name);
          res_attrs_ref.reset(luaL_ref(L, LUA_REGISTRYINDEX));
          lua_pop(L, 1);
        }
        lua_pop(L, 1);
      }
      if (!res_attrs_ref.valid()) {
        lua_newtable(L);
        res_attrs_ref.reset(luaL_ref(L, LUA_REGISTRYINDEX));
      }

      if (!core::GetTableField(L, rm_obj_idx, "scopeMetrics", "scope_metrics")) {
        lua_pop(L, 1);
        continue;
      }
      int sm_arr_idx = lua_absindex(L, -1);
      if (!lua_istable(L, sm_arr_idx)) {
        lua_pop(L, 2);
        continue;
      }

      lua_Integer sm_len = static_cast<lua_Integer>(lua_rawlen(L, sm_arr_idx));
      for (lua_Integer j = 1; j <= sm_len; ++j) {
        lua_rawgeti(L, sm_arr_idx, j);
        if (!lua_istable(L, -1)) {
          lua_pop(L, 1);
          continue;
        }
        int sm_obj_idx = lua_absindex(L, -1);

        std::string_view scope_name;
        std::string_view scope_version;
        core::ScopedLuaRef scope_attrs_ref(L, LUA_NOREF);

        if (core::GetTableField(L, sm_obj_idx, "scope")) {
          int s_idx = lua_absindex(L, -1);
          scope_name = core::ReadTableStringView(L, s_idx, "name", nullptr, tls_scope_name_scratch);
          scope_version =
              core::ReadTableStringView(L, s_idx, "version", nullptr, tls_scope_ver_scratch);
          if (core::GetTableField(L, s_idx, "attributes")) {
            core::PushTableAttributes(L, -1, json_writer);
            scope_attrs_ref.reset(luaL_ref(L, LUA_REGISTRYINDEX));
            lua_pop(L, 1);
          }
          lua_pop(L, 1);
        }
        if (!scope_attrs_ref.valid()) {
          lua_newtable(L);
          scope_attrs_ref.reset(luaL_ref(L, LUA_REGISTRYINDEX));
        }

        if (!core::GetTableField(L, sm_obj_idx, "metrics")) {
          lua_pop(L, 1);
          continue;
        }
        int metrics_arr_idx = lua_absindex(L, -1);
        if (!lua_istable(L, metrics_arr_idx)) {
          lua_pop(L, 2);
          continue;
        }

        lua_Integer m_len = static_cast<lua_Integer>(lua_rawlen(L, metrics_arr_idx));
        for (lua_Integer k = 1; k <= m_len; ++k) {
          lua_rawgeti(L, metrics_arr_idx, k);
          if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
          }
          int metric_obj_idx = lua_absindex(L, -1);

          std::string_view metric_type;
          int type_obj_idx = 0;

          if (TryGetTableSubtable(L, metric_obj_idx, "gauge")) {
            metric_type = "gauge";
            type_obj_idx = lua_absindex(L, -1);
          } else if (TryGetTableSubtable(L, metric_obj_idx, "sum")) {
            metric_type = "sum";
            type_obj_idx = lua_absindex(L, -1);
          } else if (TryGetTableSubtable(L, metric_obj_idx, "histogram")) {
            metric_type = "histogram";
            type_obj_idx = lua_absindex(L, -1);
          } else if (TryGetTableSubtable(L, metric_obj_idx, "exponentialHistogram",
                                         "exponential_histogram")) {
            metric_type = "exponential_histogram";
            type_obj_idx = lua_absindex(L, -1);
          } else if (TryGetTableSubtable(L, metric_obj_idx, "summary")) {
            metric_type = "summary";
            type_obj_idx = lua_absindex(L, -1);
          }

          if (metric_type.empty()) {
            lua_pop(L, 1);
            continue;
          }

          bool is_sum = (metric_type == "sum");
          bool is_histogram = (metric_type == "histogram");
          bool is_exp_histogram = (metric_type == "exponential_histogram");
          bool is_summary = (metric_type == "summary");

          int64_t agg_temporality = 0;
          if (is_sum || is_histogram || is_exp_histogram) {
            agg_temporality = ReadTableAggregationTemporality(L, type_obj_idx);
          }

          bool has_is_monotonic = false;
          bool is_monotonic = false;
          if (is_sum) {
            if (core::GetTableField(L, type_obj_idx, "isMonotonic", "is_monotonic")) {
              if (lua_isboolean(L, -1)) {
                is_monotonic = (lua_toboolean(L, -1) != 0);
                has_is_monotonic = true;
              }
              lua_pop(L, 1);
            }
          }

          std::string_view metric_name = core::ReadTableStringView(
              L, metric_obj_idx, "name", nullptr, tls_metric_name_scratch);
          std::string_view metric_desc = core::ReadTableStringView(
              L, metric_obj_idx, "description", nullptr, tls_metric_desc_scratch);
          std::string_view metric_unit = core::ReadTableStringView(
              L, metric_obj_idx, "unit", nullptr, tls_metric_unit_scratch);

          if (core::GetTableField(L, type_obj_idx, "dataPoints", "data_points")) {
            int dps_arr_idx = lua_absindex(L, -1);
            if (lua_istable(L, dps_arr_idx)) {
              lua_Integer dps_len = static_cast<lua_Integer>(lua_rawlen(L, dps_arr_idx));

              for (lua_Integer dp_i = 1; dp_i <= dps_len; ++dp_i) {
                lua_rawgeti(L, dps_arr_idx, dp_i);
                if (!lua_istable(L, -1)) {
                  lua_pop(L, 1);
                  continue;
                }
                int dp_obj_idx = lua_absindex(L, -1);

                uint64_t start_nanos = core::ReadTableUint64(L, dp_obj_idx, "startTimeUnixNano",
                                                             "start_time_unix_nano");
                std::array<char, 32> start_ts_buf;
                size_t start_ts_len = 0;
                if (start_nanos > 0) {
                  core::FormatIso8601Nano(start_nanos, start_ts_buf.data());
                  start_ts_len = 30;
                }

                uint64_t time_nanos =
                    core::ReadTableUint64(L, dp_obj_idx, "timeUnixNano", "time_unix_nano");
                std::array<char, 32> time_ts_buf;
                size_t time_ts_len = 0;
                if (time_nanos > 0) {
                  core::FormatIso8601Nano(time_nanos, time_ts_buf.data());
                  time_ts_len = 30;
                }

                emitter.StartRecord();
                emitter.SetString("MetricName", metric_name);
                emitter.SetString("MetricDescription", metric_desc);
                emitter.SetString("MetricUnit", metric_unit);
                emitter.SetString("MetricType", metric_type);
                emitter.SetString("ServiceName", service_name);

                if (is_sum || is_histogram || is_exp_histogram) {
                  emitter.SetInt64("AggregationTemporality", agg_temporality);
                }
                if (is_sum && has_is_monotonic) {
                  emitter.SetBool("IsMonotonic", is_monotonic);
                }

                lua_rawgeti(L, LUA_REGISTRYINDEX, res_attrs_ref.get());
                emitter.SetTableFromStack("ResourceAttributes");

                emitter.SetString("ScopeName", scope_name);
                emitter.SetString("ScopeVersion", scope_version);

                lua_rawgeti(L, LUA_REGISTRYINDEX, scope_attrs_ref.get());
                emitter.SetTableFromStack("ScopeAttributes");

                if (core::GetTableField(L, dp_obj_idx, "attributes")) {
                  core::PushTableAttributes(L, -1, json_writer);
                  emitter.SetTableFromStack("Attributes");
                  lua_pop(L, 1);
                } else {
                  lua_newtable(L);
                  emitter.SetTableFromStack("Attributes");
                }

                emitter.SetString("StartTimeUnix",
                                  std::string_view(start_ts_buf.data(), start_ts_len));
                emitter.SetString("TimeUnix", std::string_view(time_ts_buf.data(), time_ts_len));

                if (!is_histogram && !is_exp_histogram && !is_summary) {
                  bool is_int = false;
                  int64_t i_val = 0;
                  double d_val = 0.0;
                  if (ReadTableGenericMetricValue(L, dp_obj_idx, is_int, i_val, d_val)) {
                    if (is_int) {
                      emitter.SetInt64("Value", i_val);
                    } else {
                      emitter.SetDouble("Value", d_val);
                    }
                  }
                }

                if (is_histogram) {
                  lua_createtable(L, 0, 14);
                  int h_tab_idx = lua_absindex(L, -1);
                  SetDualIntField(L, h_tab_idx, "aggregationTemporality", "AggregationTemporality",
                                  static_cast<lua_Integer>(agg_temporality));
                  uint64_t h_count = core::ReadTableUint64(L, dp_obj_idx, "count", "Count");
                  SetDualIntField(L, h_tab_idx, "count", "Count", ClampUint64ToLuaInt(h_count));

                  double h_sum = core::ReadTableDouble(L, dp_obj_idx, "sum", "Sum", 0.0);
                  SetDualDoubleField(L, h_tab_idx, "sum", "Sum", h_sum);

                  double h_min = 0.0;
                  if (core::TryReadTableDouble(L, dp_obj_idx, "min", "Min", h_min)) {
                    SetDualDoubleField(L, h_tab_idx, "min", "Min", h_min);
                  }
                  double h_max = 0.0;
                  if (core::TryReadTableDouble(L, dp_obj_idx, "max", "Max", h_max)) {
                    SetDualDoubleField(L, h_tab_idx, "max", "Max", h_max);
                  }

                  if (core::GetTableField(L, dp_obj_idx, "bucketCounts", "bucket_counts",
                                          "BucketCounts")) {
                    PushNormalizedTableBucketCounts(L, -1);
                    lua_remove(L, -2);
                  } else {
                    lua_newtable(L);
                  }
                  SetDualSubtableField(L, h_tab_idx, "bucketCounts", "BucketCounts");

                  if (core::GetTableField(L, dp_obj_idx, "explicitBounds", "explicit_bounds",
                                          "ExplicitBounds")) {
                    PushNormalizedTableExplicitBounds(L, -1);
                    lua_remove(L, -2);
                  } else {
                    lua_newtable(L);
                  }
                  SetDualSubtableField(L, h_tab_idx, "explicitBounds", "ExplicitBounds");

                  emitter.SetTableFromStack("Histogram");
                }

                if (is_exp_histogram) {
                  lua_createtable(L, 0, 20);
                  int eh_tab_idx = lua_absindex(L, -1);
                  SetDualIntField(L, eh_tab_idx, "aggregationTemporality", "AggregationTemporality",
                                  static_cast<lua_Integer>(agg_temporality));
                  uint64_t eh_count = core::ReadTableUint64(L, dp_obj_idx, "count", "Count");
                  SetDualIntField(L, eh_tab_idx, "count", "Count", ClampUint64ToLuaInt(eh_count));

                  double eh_sum = 0.0;
                  if (core::TryReadTableDouble(L, dp_obj_idx, "sum", "Sum", eh_sum)) {
                    SetDualDoubleField(L, eh_tab_idx, "sum", "Sum", eh_sum);
                  }
                  double eh_min = 0.0;
                  if (core::TryReadTableDouble(L, dp_obj_idx, "min", "Min", eh_min)) {
                    SetDualDoubleField(L, eh_tab_idx, "min", "Min", eh_min);
                  }
                  double eh_max = 0.0;
                  if (core::TryReadTableDouble(L, dp_obj_idx, "max", "Max", eh_max)) {
                    SetDualDoubleField(L, eh_tab_idx, "max", "Max", eh_max);
                  }

                  int64_t eh_scale = core::ReadTableInt64(L, dp_obj_idx, "scale", "Scale", 0);
                  SetDualIntField(L, eh_tab_idx, "scale", "Scale",
                                  static_cast<lua_Integer>(eh_scale));

                  uint64_t eh_zero_count = core::ReadTableUint64(L, dp_obj_idx, "zeroCount",
                                                                 "zero_count", 0, "ZeroCount");
                  SetDualIntField(L, eh_tab_idx, "zeroCount", "ZeroCount",
                                  ClampUint64ToLuaInt(eh_zero_count));

                  double eh_zero_threshold = core::ReadTableDouble(
                      L, dp_obj_idx, "zeroThreshold", "zero_threshold", 0.0, "ZeroThreshold");
                  SetDualDoubleField(L, eh_tab_idx, "zeroThreshold", "ZeroThreshold",
                                     eh_zero_threshold);

                  auto push_exp_buckets_subtable = [&](const char* k1, const char* k2) {
                    lua_createtable(L, 0, 4);
                    int sub_idx = lua_absindex(L, -1);
                    int64_t offset = 0;
                    if (core::GetTableField(L, dp_obj_idx, k1, k2)) {
                      if (lua_istable(L, -1)) {
                        int b_obj_idx = lua_absindex(L, -1);
                        offset = core::ReadTableInt64(L, b_obj_idx, "offset", "Offset", 0);
                        if (core::GetTableField(L, b_obj_idx, "bucketCounts", "bucket_counts",
                                                "BucketCounts")) {
                          PushNormalizedTableBucketCounts(L, -1);
                          lua_remove(L, -2);
                        } else {
                          lua_newtable(L);
                        }
                      } else {
                        lua_newtable(L);
                      }
                      lua_remove(L, -2);
                    } else {
                      lua_newtable(L);
                    }
                    SetDualSubtableField(L, sub_idx, "bucketCounts", "BucketCounts");
                    SetDualIntField(L, sub_idx, "offset", "Offset",
                                    static_cast<lua_Integer>(offset));
                    SetDualSubtableField(L, eh_tab_idx, k1, k2);
                  };
                  push_exp_buckets_subtable("positive", "Positive");
                  push_exp_buckets_subtable("negative", "Negative");

                  emitter.SetTableFromStack("ExponentialHistogram");
                }

                if (is_summary) {
                  lua_createtable(L, 0, 6);
                  int s_tab_idx = lua_absindex(L, -1);
                  uint64_t s_count = core::ReadTableUint64(L, dp_obj_idx, "count", "Count");
                  SetDualIntField(L, s_tab_idx, "count", "Count", ClampUint64ToLuaInt(s_count));

                  double s_sum = core::ReadTableDouble(L, dp_obj_idx, "sum", "Sum", 0.0);
                  SetDualDoubleField(L, s_tab_idx, "sum", "Sum", s_sum);

                  if (core::GetTableField(L, dp_obj_idx, "quantileValues", "quantile_values",
                                          "QuantileValues")) {
                    if (lua_istable(L, -1)) {
                      int qv_in_idx = lua_absindex(L, -1);
                      lua_Integer qv_len = static_cast<lua_Integer>(lua_rawlen(L, qv_in_idx));
                      lua_createtable(L, static_cast<int>(qv_len > 0 ? qv_len : 0), 0);
                      lua_Integer out_qi = 1;
                      for (lua_Integer qi = 1; qi <= qv_len; ++qi) {
                        lua_rawgeti(L, qv_in_idx, qi);
                        if (lua_istable(L, -1)) {
                          int q_item_idx = lua_absindex(L, -1);
                          double q_val =
                              core::ReadTableDouble(L, q_item_idx, "quantile", "Quantile", 0.0);
                          double v_val =
                              core::ReadTableDouble(L, q_item_idx, "value", "Value", 0.0);
                          lua_createtable(L, 0, 4);
                          SetDualDoubleField(L, -1, "quantile", "Quantile", q_val);
                          SetDualDoubleField(L, -1, "value", "Value", v_val);
                          lua_rawseti(L, -3, out_qi++);
                        }
                        lua_pop(L, 1);
                      }
                    } else {
                      lua_newtable(L);
                    }
                    lua_remove(L, -2);
                  } else {
                    lua_newtable(L);
                  }
                  SetDualSubtableField(L, s_tab_idx, "quantileValues", "QuantileValues");
                  emitter.SetTableFromStack("Summary");
                }

                emitter.SetInt64("Flags",
                                 static_cast<int64_t>(static_cast<uint32_t>(
                                     core::ReadTableUint64(L, dp_obj_idx, "flags", "Flags"))));

                if (core::GetTableField(L, dp_obj_idx, "exemplars", "Exemplars")) {
                  core::PushTableExemplars(L, -1, json_writer);
                  lua_remove(L, -2);  // remove raw input exemplars table
                  emitter.SetTableFromStack("Exemplars");
                } else {
                  lua_newtable(L);
                  emitter.SetTableFromStack("Exemplars");
                }

                emitter.SetString("processed_at", processed_at);

                emitter.EndRecord();
                lua_pop(L, 1);  // pop dp_obj
                if (emitter.HasError()) {
                  return ExtractionResult{false, std::string(emitter.ErrorMessage()),
                                          emitter.EmittedCount()};
                }
              }
            }
            lua_pop(L, 1);  // pop dps_arr
          }

          lua_pop(L, 1);  // pop type_obj
          lua_pop(L, 1);  // pop metric_obj
        }
        lua_pop(L, 1);  // pop metrics_arr
        lua_pop(L, 1);  // pop sm_obj
      }
      lua_pop(L, 1);  // pop sm_arr
      lua_pop(L, 1);  // pop rm_obj
    }

    emitter.Flush();
    if (emitter.HasError()) {
      return ExtractionResult{false, std::string(emitter.ErrorMessage()), emitter.EmittedCount()};
    }
    return ExtractionResult{true, "", emitter.EmittedCount()};
  } catch (const std::exception& e) {
    return ExtractionResult{false, std::string("Extraction exception: ") + e.what(),
                            emitter.EmittedCount()};
  } catch (...) {
    return ExtractionResult{false, "Unknown extraction exception", emitter.EmittedCount()};
  }
}

}  // namespace vector_otel::extractors
