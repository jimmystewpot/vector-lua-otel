#include "vector_otel/extractors/metrics_extractor.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vector_otel/core/fast_json_writer.hpp"
#include "vector_otel/core/lua_bridge.hpp"
#include "vector_otel/core/otlp_common.hpp"
#include "vector_otel/core/parser.hpp"
#include "vector_otel/core/table_reader.hpp"
#include "vector_otel/core/timestamp.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace vector_otel {
namespace extractors {
namespace {

int ParseExemplars(lua_State* L, simdjson::ondemand::value& val,
                   core::FastJsonWriter& json_writer) {
  if (L == nullptr) {
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

    char time_iso_buf[32];
    size_t time_iso_len = 0;
    std::string_view span_id;
    std::string_view trace_id;
    enum class ExValKind {
      kNone,
      kDouble,
      kInt64
    } ex_val_kind = ExValKind::kNone;
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
          core::FormatIso8601Nano(nanos, time_iso_buf);
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
        ex_double_val = core::ParseDouble(fv);
        ex_val_kind = ExValKind::kDouble;
      } else if (k == "asInt" || k == "as_int") {
        ex_int_val = core::ParseInt64(fv);
        ex_val_kind = ExValKind::kInt64;
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
    lua_pushlstring(L, time_iso_buf, time_iso_len);
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
    lua_pushlstring(L, span_id.data(), span_id.size());
    lua_setfield(L, -2, "SpanId");

    // Set TraceId
    lua_pushlstring(L, trace_id.data(), trace_id.size());
    lua_setfield(L, -2, "TraceId");

    // Put into exemplars array table at ex_idx
    lua_rawseti(L, -2, ex_idx++);
  }

  return luaL_ref(L, LUA_REGISTRYINDEX);
}

}  // namespace

ExtractionResult ProcessMetrics(lua_State* L, std::string_view raw_json,
                                int emit_idx) {
  if (raw_json.empty()) {
    return ExtractionResult{false, "Empty JSON input", 0};
  }

  static thread_local core::OtelParser parser;
  static thread_local core::FastJsonWriter json_writer;
  json_writer.Reset();

  auto doc_res = parser.Iterate(raw_json);
  if (doc_res.error() != simdjson::SUCCESS) {
    return ExtractionResult{
        false,
        "JSON parsing error: " +
            std::string(simdjson::error_message(doc_res.error())),
        0};
  }

  auto doc = std::move(doc_res.value());
  auto root_obj_res = doc.get_object();
  if (root_obj_res.error() != simdjson::SUCCESS) {
    return ExtractionResult{false, "Expected root JSON object", 0};
  }
  auto root_obj = root_obj_res.value();

  core::LuaEmitter emitter(L, emit_idx);

  try {
    auto res_metrics_res = root_obj.find_field_unordered("resourceMetrics");
    if (res_metrics_res.error() != simdjson::SUCCESS) {
      res_metrics_res = root_obj.find_field_unordered("resource_metrics");
    }
    if (res_metrics_res.error() != simdjson::SUCCESS) {
      return ExtractionResult{true, "", 0};
    }

    auto res_metrics_val = res_metrics_res.value();
    auto res_metrics_arr_res = res_metrics_val.get_array();
    if (res_metrics_arr_res.error() != simdjson::SUCCESS) {
      return ExtractionResult{true, "", 0};
    }

    for (auto res_metric_elem : res_metrics_arr_res.value()) {
      if (res_metric_elem.error() != simdjson::SUCCESS) continue;
      auto res_metric_obj_res = res_metric_elem.value().get_object();
      if (res_metric_obj_res.error() != simdjson::SUCCESS) continue;
      auto res_metric_obj = res_metric_obj_res.value();

      std::string_view service_name = "unknown_service";
      core::ScopedLuaRef res_attrs_ref(L, LUA_NOREF);

      auto resource_field_res = res_metric_obj.find_field_unordered("resource");
      if (resource_field_res.error() == simdjson::SUCCESS) {
        auto res_val = resource_field_res.value();
        auto res_obj_res = res_val.get_object();
        if (res_obj_res.error() == simdjson::SUCCESS) {
          auto res_obj = res_obj_res.value();
          service_name = core::ExtractServiceName(res_obj);
          auto attrs_res = res_obj.find_field_unordered("attributes");
          if (attrs_res.error() == simdjson::SUCCESS && L != nullptr) {
            auto attrs_val = attrs_res.value();
            core::PushAttributesTable(L, attrs_val, json_writer);
            res_attrs_ref.reset(luaL_ref(L, LUA_REGISTRYINDEX));
          }
        }
      }

      if (L != nullptr && !res_attrs_ref.valid()) {
        lua_newtable(L);
        res_attrs_ref.reset(luaL_ref(L, LUA_REGISTRYINDEX));
      }

      auto scope_metrics_res =
          res_metric_obj.find_field_unordered("scopeMetrics");
      if (scope_metrics_res.error() != simdjson::SUCCESS) {
        scope_metrics_res =
            res_metric_obj.find_field_unordered("scope_metrics");
      }
      if (scope_metrics_res.error() != simdjson::SUCCESS) {
        continue;
      }

      auto scope_metrics_arr_res = scope_metrics_res.value().get_array();
      if (scope_metrics_arr_res.error() != simdjson::SUCCESS) {
        continue;
      }

      for (auto scope_metric_elem : scope_metrics_arr_res.value()) {
        if (scope_metric_elem.error() != simdjson::SUCCESS) continue;
        auto scope_metric_obj_res = scope_metric_elem.value().get_object();
        if (scope_metric_obj_res.error() != simdjson::SUCCESS) continue;
        auto scope_metric_obj = scope_metric_obj_res.value();

        std::string scope_name;
        std::string scope_version;
        core::ScopedLuaRef scope_attrs_ref(L, LUA_NOREF);

        auto scope_field_res = scope_metric_obj.find_field_unordered("scope");
        if (scope_field_res.error() == simdjson::SUCCESS) {
          auto scope_val = scope_field_res.value();
          auto scope_obj_res = scope_val.get_object();
          if (scope_obj_res.error() == simdjson::SUCCESS) {
            auto scope_obj = scope_obj_res.value();
            scope_name =
                std::string(core::ExtractStringField(scope_obj, "name"));
            scope_version =
                std::string(core::ExtractStringField(scope_obj, "version"));
            auto attrs_res = scope_obj.find_field_unordered("attributes");
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
        if (metrics_res.error() != simdjson::SUCCESS) {
          continue;
        }

        auto metrics_arr_res = metrics_res.value().get_array();
        if (metrics_arr_res.error() != simdjson::SUCCESS) {
          continue;
        }

        for (auto metric_elem : metrics_arr_res.value()) {
          if (metric_elem.error() != simdjson::SUCCESS) continue;
          auto metric_obj_res = metric_elem.value().get_object();
          if (metric_obj_res.error() != simdjson::SUCCESS) continue;
          auto metric_obj = metric_obj_res.value();

          std::string metric_name(core::ExtractStringField(metric_obj, "name"));
          std::string metric_desc(
              core::ExtractStringField(metric_obj, "description"));
          std::string metric_unit(core::ExtractStringField(metric_obj, "unit"));

          std::string metric_type;
          simdjson::ondemand::value type_val;

          auto gauge_res = metric_obj.find_field_unordered("gauge");
          if (gauge_res.error() == simdjson::SUCCESS) {
            metric_type = "gauge";
            type_val = gauge_res.value();
          } else {
            auto sum_res = metric_obj.find_field_unordered("sum");
            if (sum_res.error() == simdjson::SUCCESS) {
              metric_type = "sum";
              type_val = sum_res.value();
            } else {
              auto hist_res = metric_obj.find_field_unordered("histogram");
              if (hist_res.error() == simdjson::SUCCESS) {
                metric_type = "histogram";
                type_val = hist_res.value();
              } else {
                auto summ_res = metric_obj.find_field_unordered("summary");
                if (summ_res.error() == simdjson::SUCCESS) {
                  metric_type = "summary";
                  type_val = summ_res.value();
                }
              }
            }
          }

          if (metric_type.empty()) {
            continue;
          }

          auto type_obj_res = type_val.get_object();
          if (type_obj_res.error() != simdjson::SUCCESS) continue;
          auto type_obj = type_obj_res.value();

          auto dps_res = type_obj.find_field_unordered("dataPoints");
          if (dps_res.error() != simdjson::SUCCESS) {
            dps_res = type_obj.find_field_unordered("data_points");
          }
          if (dps_res.error() != simdjson::SUCCESS) continue;

          auto dps_arr_res = dps_res.value().get_array();
          if (dps_arr_res.error() != simdjson::SUCCESS) continue;

          bool is_histogram = (metric_type == "histogram");
          bool is_summary = (metric_type == "summary");

          for (auto dp_elem : dps_arr_res.value()) {
            if (dp_elem.error() != simdjson::SUCCESS) continue;
            auto dp_obj_res = dp_elem.value().get_object();
            if (dp_obj_res.error() != simdjson::SUCCESS) continue;
            auto dp_obj = dp_obj_res.value();

            char start_ts_buf[32];
            size_t start_ts_len = 0;
            char time_ts_buf[32];
            size_t time_ts_len = 0;
            enum class ValueKind { kNone, kDouble, kInt64 };
            ValueKind val_kind = ValueKind::kNone;
            double double_val = 0.0;
            int64_t int_val = 0;
            uint32_t flags = 0;
            core::ScopedLuaRef dp_attrs_ref(L, LUA_NOREF);
            core::ScopedLuaRef exemplars_ref(L, LUA_NOREF);

            // Histogram fields
            uint64_t hist_count = 0;
            double hist_sum = 0.0;
            double hist_min = 0.0;
            bool has_min = false;
            double hist_max = 0.0;
            bool has_max = false;
            static thread_local std::vector<int64_t> tls_bucket_counts;
            static thread_local std::vector<double> tls_explicit_bounds;
            tls_bucket_counts.clear();
            tls_explicit_bounds.clear();

            for (auto field_res : dp_obj) {
              if (field_res.error() != simdjson::SUCCESS) continue;
              auto k_res = field_res.unescaped_key();
              if (k_res.error() != simdjson::SUCCESS) continue;
              std::string_view k = k_res.value();
              auto val_field_res = field_res.value();
              if (val_field_res.error() != simdjson::SUCCESS) continue;
              auto fv = val_field_res.value();

              if (k == "startTimeUnixNano" || k == "start_time_unix_nano") {
                uint64_t nanos = core::ParseUint64(fv);
                if (nanos > 0) {
                  core::FormatIso8601Nano(nanos, start_ts_buf);
                  start_ts_len = 30;
                }
              } else if (k == "timeUnixNano" || k == "time_unix_nano") {
                uint64_t nanos = core::ParseUint64(fv);
                if (nanos > 0) {
                  core::FormatIso8601Nano(nanos, time_ts_buf);
                  time_ts_len = 30;
                }
              } else if (k == "asDouble" || k == "as_double") {
                double_val = core::ParseDouble(fv);
                val_kind = ValueKind::kDouble;
              } else if (k == "asInt" || k == "as_int") {
                int_val = core::ParseInt64(fv);
                val_kind = ValueKind::kInt64;
              } else if (k == "flags") {
                flags = static_cast<uint32_t>(core::ParseUint64(fv));
              } else if (k == "attributes") {
                if (L != nullptr) {
                  core::PushAttributesTable(L, fv, json_writer);
                  dp_attrs_ref.reset(luaL_ref(L, LUA_REGISTRYINDEX));
                }
              } else if (k == "count") {
                hist_count = core::ParseUint64(fv);
              } else if (k == "sum") {
                hist_sum = core::ParseDouble(fv);
              } else if (k == "min") {
                hist_min = core::ParseDouble(fv);
                has_min = true;
              } else if (k == "max") {
                hist_max = core::ParseDouble(fv);
                has_max = true;
              } else if (k == "bucketCounts" || k == "bucket_counts") {
                auto arr_res = fv.get_array();
                if (arr_res.error() == simdjson::SUCCESS) {
                  for (auto b_elem : arr_res.value()) {
                    if (b_elem.error() == simdjson::SUCCESS) {
                      auto b_val = b_elem.value();
                      tls_bucket_counts.push_back(core::ParseInt64(b_val));
                    }
                  }
                }
              } else if (k == "explicitBounds" || k == "explicit_bounds") {
                auto arr_res = fv.get_array();
                if (arr_res.error() == simdjson::SUCCESS) {
                  for (auto eb_elem : arr_res.value()) {
                    if (eb_elem.error() == simdjson::SUCCESS) {
                      auto eb_val = eb_elem.value();
                      tls_explicit_bounds.push_back(core::ParseDouble(eb_val));
                    }
                  }
                }
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
              if (dp_attrs_ref.valid()) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, dp_attrs_ref.get());
                dp_attrs_ref.reset();
              } else {
                lua_newtable(L);
              }
              emitter.SetTableFromStack("Attributes");
            }

            emitter.SetString("StartTimeUnix",
                              std::string_view(start_ts_buf, start_ts_len));
            emitter.SetString("TimeUnix",
                              std::string_view(time_ts_buf, time_ts_len));

            // Value: set for gauge/sum, omit for histogram/summary
            if (!is_histogram && !is_summary) {
              if (val_kind == ValueKind::kDouble) {
                emitter.SetDouble("Value", double_val);
              } else if (val_kind == ValueKind::kInt64) {
                emitter.SetInt64("Value", int_val);
              }
            }

            // Histogram
            if (is_histogram && L != nullptr) {
              lua_newtable(L);
              lua_pushinteger(L, static_cast<lua_Integer>(hist_count));
              lua_setfield(L, -2, "count");
              lua_pushinteger(L, static_cast<lua_Integer>(hist_count));
              lua_setfield(L, -2, "Count");

              lua_pushnumber(L, hist_sum);
              lua_setfield(L, -2, "sum");
              lua_pushnumber(L, hist_sum);
              lua_setfield(L, -2, "Sum");

              if (has_min) {
                lua_pushnumber(L, hist_min);
                lua_setfield(L, -2, "min");
                lua_pushnumber(L, hist_min);
                lua_setfield(L, -2, "Min");
              }
              if (has_max) {
                lua_pushnumber(L, hist_max);
                lua_setfield(L, -2, "max");
                lua_pushnumber(L, hist_max);
                lua_setfield(L, -2, "Max");
              }

              // bucketCounts
              lua_createtable(L, static_cast<int>(tls_bucket_counts.size()), 0);
              for (size_t bi = 0; bi < tls_bucket_counts.size(); ++bi) {
                lua_pushinteger(
                    L, static_cast<lua_Integer>(tls_bucket_counts[bi]));
                lua_rawseti(L, -2, static_cast<lua_Integer>(bi + 1));
              }
              lua_pushvalue(L, -1);
              lua_setfield(L, -3, "BucketCounts");
              lua_setfield(L, -2, "bucketCounts");

              // explicitBounds
              lua_createtable(L, static_cast<int>(tls_explicit_bounds.size()),
                              0);
              for (size_t bi = 0; bi < tls_explicit_bounds.size(); ++bi) {
                lua_pushnumber(L, tls_explicit_bounds[bi]);
                lua_rawseti(L, -2, static_cast<lua_Integer>(bi + 1));
              }
              lua_pushvalue(L, -1);
              lua_setfield(L, -3, "ExplicitBounds");
              lua_setfield(L, -2, "explicitBounds");

              emitter.SetTableFromStack("Histogram");
            }

            emitter.SetInt64("Flags", flags);

            // Exemplars
            if (L != nullptr && exemplars_ref.valid()) {
              lua_rawgeti(L, LUA_REGISTRYINDEX, exemplars_ref.get());
              exemplars_ref.reset();
              emitter.SetTableFromStack("Exemplars");
            }

            char processed_at_buf[32];
            core::FormatIso8601Nano(core::CurrentTimeUnixNano(),
                                    processed_at_buf);
            emitter.SetString("processed_at", processed_at_buf);

            emitter.EndRecord();
            if (emitter.HasError()) {
              return ExtractionResult{false,
                                      std::string(emitter.ErrorMessage()),
                                      emitter.EmittedCount()};
            }
          }
        }
      }
    }

    emitter.Flush();
    if (emitter.HasError()) {
      return ExtractionResult{false, std::string(emitter.ErrorMessage()),
                              emitter.EmittedCount()};
    }
    return ExtractionResult{true, "", emitter.EmittedCount()};
  } catch (const std::exception& e) {
    return ExtractionResult{false,
                            std::string("Extraction exception: ") + e.what(),
                            emitter.EmittedCount()};
  } catch (...) {
    return ExtractionResult{false, "Unknown extraction exception",
                            emitter.EmittedCount()};
  }
}

ExtractionResult ProcessMetricsTable(lua_State* L, int table_idx,
                                     int emit_idx) {
  int abs_idx = lua_absindex(L, table_idx);
  if (!lua_istable(L, abs_idx)) {
    return ExtractionResult{false, "payload must be a table", 0};
  }

  int rm_arr_idx = 0;
  bool pop_rm = false;

  if (core::GetTableField(L, abs_idx, "resourceMetrics", "resource_metrics")) {
    rm_arr_idx = lua_absindex(L, -1);
    pop_rm = true;
  } else if (core::GetTableField(L, abs_idx, "metric")) {
    int m_idx = lua_absindex(L, -1);
    if (core::GetTableField(L, m_idx, "resourceMetrics", "resource_metrics")) {
      rm_arr_idx = lua_absindex(L, -1);
      pop_rm = true;
    }
  } else if (core::GetTableField(L, abs_idx, "log")) {
    int log_idx = lua_absindex(L, -1);
    if (core::GetTableField(L, log_idx, "resourceMetrics",
                            "resource_metrics")) {
      rm_arr_idx = lua_absindex(L, -1);
      pop_rm = true;
    }
  }

  if (rm_arr_idx == 0) {
    rm_arr_idx = abs_idx;
  }

  if (!lua_istable(L, rm_arr_idx)) {
    if (pop_rm) lua_pop(L, 1);
    return ExtractionResult{true, "", 0};
  }

  core::LuaEmitter emitter(L, emit_idx);
  static thread_local core::FastJsonWriter json_writer;

  lua_Integer rm_len = luaL_len(L, rm_arr_idx);
  for (lua_Integer i = 1; i <= rm_len; ++i) {
    lua_rawgeti(L, rm_arr_idx, i);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      continue;
    }
    int rm_obj_idx = lua_absindex(L, -1);

    std::string service_name = "unknown_service";
    core::ScopedLuaRef res_attrs_ref(L, LUA_NOREF);

    if (core::GetTableField(L, rm_obj_idx, "resource")) {
      int res_idx = lua_absindex(L, -1);
      service_name = core::ExtractTableServiceName(L, res_idx);
      if (core::GetTableField(L, res_idx, "attributes")) {
        core::PushTableAttributes(L, -1, json_writer);
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

    lua_Integer sm_len = luaL_len(L, sm_arr_idx);
    for (lua_Integer j = 1; j <= sm_len; ++j) {
      lua_rawgeti(L, sm_arr_idx, j);
      if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        continue;
      }
      int sm_obj_idx = lua_absindex(L, -1);

      std::string scope_name;
      std::string scope_version;
      core::ScopedLuaRef scope_attrs_ref(L, LUA_NOREF);

      if (core::GetTableField(L, sm_obj_idx, "scope")) {
        int s_idx = lua_absindex(L, -1);
        scope_name = core::ReadTableString(L, s_idx, "name");
        scope_version = core::ReadTableString(L, s_idx, "version");
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

      lua_Integer m_len = luaL_len(L, metrics_arr_idx);
      for (lua_Integer k = 1; k <= m_len; ++k) {
        lua_rawgeti(L, metrics_arr_idx, k);
        if (!lua_istable(L, -1)) {
          lua_pop(L, 1);
          continue;
        }
        int metric_obj_idx = lua_absindex(L, -1);

        std::string_view metric_type;
        int type_obj_idx = 0;
        bool pop_type = false;

        if (core::GetTableField(L, metric_obj_idx, "gauge")) {
          metric_type = "gauge";
          type_obj_idx = lua_absindex(L, -1);
          pop_type = true;
        } else if (core::GetTableField(L, metric_obj_idx, "sum")) {
          metric_type = "sum";
          type_obj_idx = lua_absindex(L, -1);
          pop_type = true;
        } else if (core::GetTableField(L, metric_obj_idx, "histogram")) {
          metric_type = "histogram";
          type_obj_idx = lua_absindex(L, -1);
          pop_type = true;
        } else if (core::GetTableField(L, metric_obj_idx, "summary")) {
          metric_type = "summary";
          type_obj_idx = lua_absindex(L, -1);
          pop_type = true;
        }

        if (metric_type.empty() || !lua_istable(L, type_obj_idx)) {
          if (pop_type) lua_pop(L, 1);
          lua_pop(L, 1);
          continue;
        }

        if (core::GetTableField(L, type_obj_idx, "dataPoints", "data_points")) {
          int dps_arr_idx = lua_absindex(L, -1);
          if (lua_istable(L, dps_arr_idx)) {
            bool is_histogram = (metric_type == "histogram");
            bool is_summary = (metric_type == "summary");
            lua_Integer dps_len = luaL_len(L, dps_arr_idx);

            for (lua_Integer dp_i = 1; dp_i <= dps_len; ++dp_i) {
              lua_rawgeti(L, dps_arr_idx, dp_i);
              if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                continue;
              }
              int dp_obj_idx = lua_absindex(L, -1);

              uint64_t start_nanos = core::ReadTableUint64(
                  L, dp_obj_idx, "startTimeUnixNano", "start_time_unix_nano");
              std::string start_time_iso;
              if (start_nanos > 0) {
                char buf[32];
                core::FormatIso8601Nano(start_nanos, buf);
                start_time_iso = buf;
              }

              uint64_t time_nanos = core::ReadTableUint64(
                  L, dp_obj_idx, "timeUnixNano", "time_unix_nano");
              std::string time_iso;
              if (time_nanos > 0) {
                char buf[32];
                core::FormatIso8601Nano(time_nanos, buf);
                time_iso = buf;
              }

              emitter.StartRecord();
              core::CopyStringField(L, metric_obj_idx, "name", nullptr, emitter,
                                    "MetricName");
              core::CopyStringField(L, metric_obj_idx, "description", nullptr,
                                    emitter, "MetricDescription");
              core::CopyStringField(L, metric_obj_idx, "unit", nullptr, emitter,
                                    "MetricUnit");
              emitter.SetString("MetricType", metric_type);
              emitter.SetString("ServiceName", service_name);

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

              emitter.SetString("StartTimeUnix", start_time_iso);
              emitter.SetString("TimeUnix", time_iso);

              if (!is_histogram && !is_summary) {
                if (core::GetTableField(L, dp_obj_idx, "asDouble",
                                        "as_double")) {
                  emitter.SetDouble("Value", lua_tonumber(L, -1));
                  lua_pop(L, 1);
                } else if (core::GetTableField(L, dp_obj_idx, "asInt",
                                               "as_int")) {
                  emitter.SetInt64("Value", lua_tointeger(L, -1));
                  lua_pop(L, 1);
                } else if (core::GetTableField(L, dp_obj_idx, "value")) {
                  if (lua_isinteger(L, -1))
                    emitter.SetInt64("Value", lua_tointeger(L, -1));
                  else
                    emitter.SetDouble("Value", lua_tonumber(L, -1));
                  lua_pop(L, 1);
                }
              }

              if (is_histogram) {
                lua_newtable(L);
                int h_tab_idx = lua_absindex(L, -1);
                uint64_t h_count =
                    core::ReadTableUint64(L, dp_obj_idx, "count");
                lua_pushinteger(L, static_cast<lua_Integer>(h_count));
                lua_setfield(L, h_tab_idx, "count");
                lua_pushinteger(L, static_cast<lua_Integer>(h_count));
                lua_setfield(L, h_tab_idx, "Count");

                double h_sum = core::ReadTableDouble(L, dp_obj_idx, "sum");
                lua_pushnumber(L, h_sum);
                lua_setfield(L, h_tab_idx, "sum");
                lua_pushnumber(L, h_sum);
                lua_setfield(L, h_tab_idx, "Sum");

                if (core::GetTableField(L, dp_obj_idx, "min")) {
                  lua_pushvalue(L, -1);
                  lua_setfield(L, h_tab_idx, "min");
                  lua_pushvalue(L, -1);
                  lua_setfield(L, h_tab_idx, "Min");
                  lua_pop(L, 1);
                }
                if (core::GetTableField(L, dp_obj_idx, "max")) {
                  lua_pushvalue(L, -1);
                  lua_setfield(L, h_tab_idx, "max");
                  lua_pushvalue(L, -1);
                  lua_setfield(L, h_tab_idx, "Max");
                  lua_pop(L, 1);
                }

                if (core::GetTableField(L, dp_obj_idx, "bucketCounts",
                                        "bucket_counts")) {
                  lua_pushvalue(L, -1);
                  lua_setfield(L, h_tab_idx, "bucketCounts");
                  lua_pushvalue(L, -1);
                  lua_setfield(L, h_tab_idx, "BucketCounts");
                  lua_pop(L, 1);
                }
                if (core::GetTableField(L, dp_obj_idx, "explicitBounds",
                                        "explicit_bounds")) {
                  lua_pushvalue(L, -1);
                  lua_setfield(L, h_tab_idx, "explicitBounds");
                  lua_pushvalue(L, -1);
                  lua_setfield(L, h_tab_idx, "ExplicitBounds");
                  lua_pop(L, 1);
                }
                emitter.SetTableFromStack("Histogram");
              }

              emitter.SetInt64("Flags",
                               core::ReadTableInt64(L, dp_obj_idx, "flags"));

              if (core::GetTableField(L, dp_obj_idx, "exemplars")) {
                emitter.SetTableFromStack("Exemplars");
              } else {
                lua_newtable(L);
                emitter.SetTableFromStack("Exemplars");
              }

              char processed_at_buf[32];
              core::FormatIso8601Nano(core::CurrentTimeUnixNano(),
                                      processed_at_buf);
              emitter.SetString("processed_at", processed_at_buf);

              emitter.EndRecord();
              lua_pop(L, 1);  // pop dp_obj
              if (emitter.HasError()) {
                if (pop_rm) lua_pop(L, 1);
                return ExtractionResult{false,
                                        std::string(emitter.ErrorMessage()),
                                        emitter.EmittedCount()};
              }
            }
          }
          lua_pop(L, 1);  // pop dps_arr
        }

        if (pop_type) lua_pop(L, 1);
        lua_pop(L, 1);  // pop metric_obj
      }
      lua_pop(L, 1);  // pop metrics_arr
      lua_pop(L, 1);  // pop sm_obj
    }
    lua_pop(L, 1);  // pop sm_arr
    lua_pop(L, 1);  // pop rm_obj
  }

  if (pop_rm) lua_pop(L, 1);
  emitter.Flush();
  if (emitter.HasError()) {
    return ExtractionResult{false, std::string(emitter.ErrorMessage()),
                            emitter.EmittedCount()};
  }
  return ExtractionResult{true, "", emitter.EmittedCount()};
}

}  // namespace extractors
}  // namespace vector_otel
