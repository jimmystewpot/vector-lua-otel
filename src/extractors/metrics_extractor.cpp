#include "vector_otel/extractors/metrics_extractor.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

void ParseJsonBucketCountsVector(simdjson::ondemand::value& fv, std::vector<uint64_t>& out) {
  out.clear();
  auto arr_res = fv.get_array();
  if (arr_res.error() == simdjson::INCORRECT_TYPE) {
    return;
  }
  if (arr_res.error() != simdjson::SUCCESS) {
    throw std::runtime_error(std::string("JSON parsing error: ") +
                             simdjson::error_message(arr_res.error()));
  }
  for (auto b_elem : arr_res.value()) {
    if (b_elem.error() != simdjson::SUCCESS) {
      throw std::runtime_error(std::string("JSON parsing error: ") +
                               simdjson::error_message(b_elem.error()));
    }
    auto b_val = b_elem.value();
    out.push_back(core::ParseUint64(b_val));
  }
}

void ParseJsonExplicitBoundsVector(simdjson::ondemand::value& fv, std::vector<double>& out) {
  out.clear();
  auto arr_res = fv.get_array();
  if (arr_res.error() == simdjson::INCORRECT_TYPE) {
    return;
  }
  if (arr_res.error() != simdjson::SUCCESS) {
    throw std::runtime_error(std::string("JSON parsing error: ") +
                             simdjson::error_message(arr_res.error()));
  }
  for (auto eb_elem : arr_res.value()) {
    if (eb_elem.error() != simdjson::SUCCESS) {
      throw std::runtime_error(std::string("JSON parsing error: ") +
                               simdjson::error_message(eb_elem.error()));
    }
    auto eb_val = eb_elem.value();
    out.push_back(core::ParseDouble(eb_val));
  }
}

void ParseJsonQuantilesVector(simdjson::ondemand::value& fv,
                              std::vector<std::pair<double, double>>& out) {
  out.clear();
  auto q_arr_res = fv.get_array();
  if (q_arr_res.error() == simdjson::INCORRECT_TYPE) {
    return;
  }
  if (q_arr_res.error() != simdjson::SUCCESS) {
    throw std::runtime_error(std::string("JSON parsing error: ") +
                             simdjson::error_message(q_arr_res.error()));
  }
  for (auto q_elem : q_arr_res.value()) {
    if (q_elem.error() != simdjson::SUCCESS) {
      throw std::runtime_error(std::string("JSON parsing error: ") +
                               simdjson::error_message(q_elem.error()));
    }
    auto q_obj_res = q_elem.value().get_object();
    if (q_obj_res.error() == simdjson::INCORRECT_TYPE) {
      continue;
    }
    if (q_obj_res.error() != simdjson::SUCCESS) {
      throw std::runtime_error(std::string("JSON parsing error: ") +
                               simdjson::error_message(q_obj_res.error()));
    }
    double q_quantile = 0.0;
    double q_value = 0.0;
    for (auto qf_res : q_obj_res.value()) {
      if (qf_res.error() != simdjson::SUCCESS) {
        throw std::runtime_error(std::string("JSON parsing error: ") +
                                 simdjson::error_message(qf_res.error()));
      }
      auto qk_res = qf_res.unescaped_key();
      if (qk_res.error() != simdjson::SUCCESS) {
        throw std::runtime_error(std::string("JSON parsing error: ") +
                                 simdjson::error_message(qk_res.error()));
      }
      std::string_view qk = qk_res.value();
      auto qv_res = qf_res.value();
      if (qv_res.error() != simdjson::SUCCESS) {
        throw std::runtime_error(std::string("JSON parsing error: ") +
                                 simdjson::error_message(qv_res.error()));
      }
      auto qfv = qv_res.value();
      if (qk == "quantile") {
        q_quantile = core::ParseDouble(qfv);
      } else if (qk == "value") {
        q_value = core::ParseDouble(qfv);
      }
    }
    out.emplace_back(q_quantile, q_value);
  }
}

void ReadTableBucketCountsVector(lua_State* L, int arr_idx, std::vector<uint64_t>& out) {
  out.clear();
  int abs_idx = lua_absindex(L, arr_idx);
  if (!lua_istable(L, abs_idx)) {
    return;
  }
  lua_Integer len = static_cast<lua_Integer>(lua_rawlen(L, abs_idx));
  for (lua_Integer i = 1; i <= len; ++i) {
    lua_rawgeti(L, abs_idx, i);
    uint64_t u_val = 0;
    if (!core::TryParseTableUint64At(L, -1, u_val)) {
      u_val = 0;
    }
    lua_pop(L, 1);
    out.push_back(u_val);
  }
}

void ReadTableExplicitBoundsVector(lua_State* L, int arr_idx, std::vector<double>& out) {
  out.clear();
  int abs_idx = lua_absindex(L, arr_idx);
  if (!lua_istable(L, abs_idx)) {
    return;
  }
  lua_Integer len = static_cast<lua_Integer>(lua_rawlen(L, abs_idx));
  for (lua_Integer i = 1; i <= len; ++i) {
    lua_rawgeti(L, abs_idx, i);
    double d_val = 0.0;
    (void)core::TryParseTableDoubleAt(L, -1, d_val);
    lua_pop(L, 1);
    out.push_back(d_val);
  }
}

void ReadTableQuantilesVector(lua_State* L, int arr_idx,
                              std::vector<std::pair<double, double>>& out) {
  out.clear();
  int abs_idx = lua_absindex(L, arr_idx);
  if (!lua_istable(L, abs_idx)) {
    return;
  }
  lua_Integer qv_len = static_cast<lua_Integer>(lua_rawlen(L, abs_idx));
  for (lua_Integer qi = 1; qi <= qv_len; ++qi) {
    lua_rawgeti(L, abs_idx, qi);
    if (lua_istable(L, -1)) {
      int q_item_idx = lua_absindex(L, -1);
      double q_val = core::ReadTableDouble(L, q_item_idx, "quantile", "Quantile", 0.0);
      double v_val = core::ReadTableDouble(L, q_item_idx, "value", "Value", 0.0);
      out.emplace_back(q_val, v_val);
    }
    lua_pop(L, 1);
  }
}

void WriteUint64Array(core::FastJsonWriter& w, const std::vector<uint64_t>& vec) {
  w.StartArray();
  for (size_t i = 0; i < vec.size(); ++i) {
    w.Uint64(vec[i]);
  }
  w.EndArray();
}

void WriteDoubleArray(core::FastJsonWriter& w, const std::vector<double>& vec) {
  w.StartArray();
  for (size_t i = 0; i < vec.size(); ++i) {
    w.Double(vec[i]);
  }
  w.EndArray();
}

void SerializeHistogramJson(core::FastJsonWriter& w, int64_t agg_temporality, uint64_t count,
                            bool has_sum, double sum, bool has_min, double min_val, bool has_max,
                            double max_val, const std::vector<uint64_t>& bucket_counts,
                            const std::vector<double>& explicit_bounds) {
  w.Reset();
  w.StartObject();
  w.Key("aggregationTemporality");
  w.Int64(agg_temporality);
  w.Key("count");
  w.Uint64(count);
  w.Key("sum");
  w.Double(has_sum ? sum : 0.0);
  if (has_min) {
    w.Key("min");
    w.Double(min_val);
  }
  if (has_max) {
    w.Key("max");
    w.Double(max_val);
  }
  w.Key("bucketCounts");
  WriteUint64Array(w, bucket_counts);
  w.Key("explicitBounds");
  WriteDoubleArray(w, explicit_bounds);
  w.EndObject();
}

void SerializeExpBucketsSubObject(core::FastJsonWriter& w, int64_t offset,
                                  const std::vector<uint64_t>& bucket_counts) {
  w.StartObject();
  w.Key("offset");
  w.Int64(offset);
  w.Key("bucketCounts");
  WriteUint64Array(w, bucket_counts);
  w.EndObject();
}

void SerializeExponentialHistogramJson(core::FastJsonWriter& w, int64_t agg_temporality,
                                       uint64_t count, bool has_sum, double sum, bool has_min,
                                       double min_val, bool has_max, double max_val, int64_t scale,
                                       uint64_t zero_count, double zero_threshold,
                                       int64_t pos_offset, const std::vector<uint64_t>& pos_buckets,
                                       int64_t neg_offset,
                                       const std::vector<uint64_t>& neg_buckets) {
  w.Reset();
  w.StartObject();
  w.Key("aggregationTemporality");
  w.Int64(agg_temporality);
  w.Key("count");
  w.Uint64(count);
  if (has_sum) {
    w.Key("sum");
    w.Double(sum);
  }
  if (has_min) {
    w.Key("min");
    w.Double(min_val);
  }
  if (has_max) {
    w.Key("max");
    w.Double(max_val);
  }
  w.Key("scale");
  w.Int64(scale);
  w.Key("zeroCount");
  w.Uint64(zero_count);
  w.Key("zeroThreshold");
  w.Double(zero_threshold);
  w.Key("positive");
  SerializeExpBucketsSubObject(w, pos_offset, pos_buckets);
  w.Key("negative");
  SerializeExpBucketsSubObject(w, neg_offset, neg_buckets);
  w.EndObject();
}

void WriteQuantilesArray(core::FastJsonWriter& w,
                         const std::vector<std::pair<double, double>>& quantiles) {
  w.StartArray();
  for (size_t i = 0; i < quantiles.size(); ++i) {
    w.StartObject();
    w.Key("quantile");
    w.Double(quantiles[i].first);
    w.Key("value");
    w.Double(quantiles[i].second);
    w.EndObject();
  }
  w.EndArray();
}

void SerializeSummaryJson(core::FastJsonWriter& w, uint64_t count, bool has_sum, double sum,
                          const std::vector<std::pair<double, double>>& quantiles) {
  w.Reset();
  w.StartObject();
  w.Key("count");
  w.Uint64(count);
  w.Key("sum");
  w.Double(has_sum ? sum : 0.0);
  w.Key("quantileValues");
  WriteQuantilesArray(w, quantiles);
  w.EndObject();
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

}  // namespace

ExtractionResult ProcessMetrics(lua_State* L, std::string_view raw_json, int emit_idx) {
  if (raw_json.empty()) {
    return ExtractionResult{false, "Empty JSON input", 0};
  }
  if (L != nullptr && !lua_checkstack(L, 32)) {
    return ExtractionResult{false, "Lua stack overflow", 0};
  }

  auto& parser = core::OtelParser::Instance();
  static thread_local core::FastJsonWriter res_attrs_writer;
  static thread_local core::FastJsonWriter scope_attrs_writer;
  static thread_local core::FastJsonWriter dp_attrs_writer;
  static thread_local core::FastJsonWriter exemplars_writer;
  static thread_local core::FastJsonWriter payload_writer;
  static thread_local std::vector<uint64_t> tls_bucket_counts;
  static thread_local std::vector<double> tls_explicit_bounds;
  static thread_local std::vector<uint64_t> tls_pos_buckets;
  static thread_local std::vector<uint64_t> tls_neg_buckets;
  static thread_local std::vector<std::pair<double, double>> tls_quantiles;

  res_attrs_writer.Reset();
  scope_attrs_writer.Reset();
  dp_attrs_writer.Reset();
  exemplars_writer.Reset();
  payload_writer.Reset();
  core::ReserveScratchVector(tls_bucket_counts);
  core::ReserveScratchVector(tls_explicit_bounds);
  core::ReserveScratchVector(tls_pos_buckets);
  core::ReserveScratchVector(tls_neg_buckets);
  core::ReserveScratchVector(tls_quantiles);

  struct WriterTrimGuard {
    WriterTrimGuard() = default;
    WriterTrimGuard(const WriterTrimGuard&) = delete;
    WriterTrimGuard& operator=(const WriterTrimGuard&) = delete;
    ~WriterTrimGuard() noexcept {
      res_attrs_writer.Trim();
      scope_attrs_writer.Trim();
      dp_attrs_writer.Trim();
      exemplars_writer.Trim();
      payload_writer.Trim();
      core::ResetScratchVector(tls_bucket_counts);
      core::ResetScratchVector(tls_explicit_bounds);
      core::ResetScratchVector(tls_pos_buckets);
      core::ResetScratchVector(tls_neg_buckets);
      core::ResetScratchVector(tls_quantiles);
      core::ResetServiceNameScratch();
    }
  } writer_trim_guard;

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

  try {
    auto res_metrics_res = root_obj.find_field_unordered("resourceMetrics");
    if (res_metrics_res.error() == simdjson::NO_SUCH_FIELD) {
      res_metrics_res = root_obj.find_field_unordered("resource_metrics");
    }
    if (res_metrics_res.error() == simdjson::NO_SUCH_FIELD) {
      return ExtractionResult{true, "", 0};
    }
    if (res_metrics_res.error() != simdjson::SUCCESS) {
      return ExtractionResult{
          false,
          std::string("JSON parsing error: ") + simdjson::error_message(res_metrics_res.error()),
          emitter.EmittedCount()};
    }

    auto res_metrics_val = res_metrics_res.value();
    auto res_metrics_arr_res = res_metrics_val.get_array();
    if (res_metrics_arr_res.error() != simdjson::SUCCESS) {
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
        continue;
      }
      auto res_metric_obj = res_metric_obj_res.value();

      std::string_view service_name = "unknown_service";
      res_attrs_writer.Reset();
      bool has_res_attrs = false;

      auto resource_field_res = res_metric_obj.find_field_unordered("resource");
      if (resource_field_res.error() == simdjson::SUCCESS) {
        auto res_val = resource_field_res.value();
        auto res_obj_res = res_val.get_object();
        if (res_obj_res.error() == simdjson::SUCCESS) {
          auto res_obj = res_obj_res.value();
          auto attrs_res = res_obj.find_field_unordered("attributes");
          if (attrs_res.error() == simdjson::SUCCESS) {
            auto attrs_val = attrs_res.value();
            core::WriteAttributesJson(res_attrs_writer, attrs_val, &service_name);
            has_res_attrs = true;
          }
        }
      }

      if (!has_res_attrs) {
        res_attrs_writer.StartObject();
        res_attrs_writer.EndObject();
      }

      auto scope_metrics_res = res_metric_obj.find_field_unordered("scopeMetrics");
      if (scope_metrics_res.error() == simdjson::NO_SUCH_FIELD) {
        scope_metrics_res = res_metric_obj.find_field_unordered("scope_metrics");
      }
      if (scope_metrics_res.error() != simdjson::SUCCESS) {
        continue;
      }

      auto scope_metrics_arr_res = scope_metrics_res.value().get_array();
      if (scope_metrics_arr_res.error() != simdjson::SUCCESS) {
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
          continue;
        }
        auto scope_metric_obj = scope_metric_obj_res.value();

        std::string_view scope_name;
        std::string_view scope_version;
        scope_attrs_writer.Reset();
        bool has_scope_attrs = false;

        auto scope_field_res = scope_metric_obj.find_field_unordered("scope");
        if (scope_field_res.error() == simdjson::SUCCESS) {
          auto scope_val = scope_field_res.value();
          auto scope_obj_res = scope_val.get_object();
          if (scope_obj_res.error() == simdjson::SUCCESS) {
            auto scope_obj = scope_obj_res.value();
            scope_name = core::ExtractStringField(scope_obj, "name");
            scope_version = core::ExtractStringField(scope_obj, "version");
            auto attrs_res = scope_obj.find_field_unordered("attributes");
            if (attrs_res.error() == simdjson::SUCCESS) {
              auto attrs_val = attrs_res.value();
              core::WriteAttributesJson(scope_attrs_writer, attrs_val);
              has_scope_attrs = true;
            }
          }
        }

        if (!has_scope_attrs) {
          scope_attrs_writer.StartObject();
          scope_attrs_writer.EndObject();
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
          if (metric_elem.error() != simdjson::SUCCESS) {
            return ExtractionResult{
                false,
                std::string("JSON parsing error: ") + simdjson::error_message(metric_elem.error()),
                emitter.EmittedCount()};
          }
          auto metric_obj_res = metric_elem.value().get_object();
          if (metric_obj_res.error() != simdjson::SUCCESS) {
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
                if (mt_res.error() == simdjson::SUCCESS &&
                    mt_res.value() == simdjson::ondemand::json_type::object) {
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
          if (type_field_res.error() != simdjson::SUCCESS) {
            continue;
          }
          auto type_obj_res = type_field_res.value().get_object();
          if (type_obj_res.error() != simdjson::SUCCESS) {
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
          if (dps_res.error() != simdjson::SUCCESS) continue;

          auto dps_arr_res = dps_res.value().get_array();
          if (dps_arr_res.error() != simdjson::SUCCESS) {
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

            dp_attrs_writer.Reset();
            bool has_dp_attrs = false;
            exemplars_writer.Reset();
            bool has_exemplars = false;
            tls_bucket_counts.clear();
            tls_explicit_bounds.clear();
            tls_pos_buckets.clear();
            tls_neg_buckets.clear();
            tls_quantiles.clear();

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
                dp_attrs_writer.Reset();
                core::WriteAttributesJson(dp_attrs_writer, fv);
                has_dp_attrs = true;
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
                  pos_offset = 0;
                  tls_pos_buckets.clear();
                  for (auto pf_res : pos_obj_res.value()) {
                    if (pf_res.error() != simdjson::SUCCESS) {
                      return ExtractionResult{false,
                                              std::string("JSON parsing error: ") +
                                                  simdjson::error_message(pf_res.error()),
                                              emitter.EmittedCount()};
                    }
                    auto pk_res = pf_res.unescaped_key();
                    if (pk_res.error() != simdjson::SUCCESS) {
                      return ExtractionResult{false,
                                              std::string("JSON parsing error: ") +
                                                  simdjson::error_message(pk_res.error()),
                                              emitter.EmittedCount()};
                    }
                    std::string_view pk = pk_res.value();
                    auto pv_res = pf_res.value();
                    if (pv_res.error() != simdjson::SUCCESS) {
                      return ExtractionResult{false,
                                              std::string("JSON parsing error: ") +
                                                  simdjson::error_message(pv_res.error()),
                                              emitter.EmittedCount()};
                    }
                    auto pfv = pv_res.value();
                    if (pk == "offset") {
                      pos_offset = core::ParseInt64(pfv);
                    } else if (pk == "bucketCounts" || pk == "bucket_counts") {
                      ParseJsonBucketCountsVector(pfv, tls_pos_buckets);
                    }
                  }
                } else if (pos_obj_res.error() != simdjson::INCORRECT_TYPE) {
                  return ExtractionResult{false,
                                          std::string("JSON parsing error: ") +
                                              simdjson::error_message(pos_obj_res.error()),
                                          emitter.EmittedCount()};
                }
              } else if (k == "negative") {
                auto neg_obj_res = fv.get_object();
                if (neg_obj_res.error() == simdjson::SUCCESS) {
                  neg_offset = 0;
                  tls_neg_buckets.clear();
                  for (auto nf_res : neg_obj_res.value()) {
                    if (nf_res.error() != simdjson::SUCCESS) {
                      return ExtractionResult{false,
                                              std::string("JSON parsing error: ") +
                                                  simdjson::error_message(nf_res.error()),
                                              emitter.EmittedCount()};
                    }
                    auto nk_res = nf_res.unescaped_key();
                    if (nk_res.error() != simdjson::SUCCESS) {
                      return ExtractionResult{false,
                                              std::string("JSON parsing error: ") +
                                                  simdjson::error_message(nk_res.error()),
                                              emitter.EmittedCount()};
                    }
                    std::string_view nk = nk_res.value();
                    auto nv_res = nf_res.value();
                    if (nv_res.error() != simdjson::SUCCESS) {
                      return ExtractionResult{false,
                                              std::string("JSON parsing error: ") +
                                                  simdjson::error_message(nv_res.error()),
                                              emitter.EmittedCount()};
                    }
                    auto nfv = nv_res.value();
                    if (nk == "offset") {
                      neg_offset = core::ParseInt64(nfv);
                    } else if (nk == "bucketCounts" || nk == "bucket_counts") {
                      ParseJsonBucketCountsVector(nfv, tls_neg_buckets);
                    }
                  }
                } else if (neg_obj_res.error() != simdjson::INCORRECT_TYPE) {
                  return ExtractionResult{false,
                                          std::string("JSON parsing error: ") +
                                              simdjson::error_message(neg_obj_res.error()),
                                          emitter.EmittedCount()};
                }
              } else if (k == "quantileValues" || k == "quantile_values") {
                ParseJsonQuantilesVector(fv, tls_quantiles);
              } else if (k == "bucketCounts" || k == "bucket_counts") {
                ParseJsonBucketCountsVector(fv, tls_bucket_counts);
              } else if (k == "explicitBounds" || k == "explicit_bounds") {
                ParseJsonExplicitBoundsVector(fv, tls_explicit_bounds);
              } else if (k == "exemplars") {
                exemplars_writer.Reset();
                core::SerializeExemplarsJson(fv, exemplars_writer);
                has_exemplars = true;
              }
            }

            if (!has_dp_attrs) {
              dp_attrs_writer.StartObject();
              dp_attrs_writer.EndObject();
            }
            if (!has_exemplars) {
              exemplars_writer.StartArray();
              exemplars_writer.EndArray();
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

            emitter.SetString("ResourceAttributes", res_attrs_writer.Str());
            emitter.SetString("ScopeName", scope_name);
            emitter.SetString("ScopeVersion", scope_version);
            emitter.SetString("ScopeAttributes", scope_attrs_writer.Str());
            emitter.SetString("Attributes", dp_attrs_writer.Str());

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

            if (is_histogram) {
              SerializeHistogramJson(payload_writer, agg_temporality, dp_count, has_sum, dp_sum,
                                     has_min, dp_min, has_max, dp_max, tls_bucket_counts,
                                     tls_explicit_bounds);
              emitter.SetString("Histogram", payload_writer.Str());
            }

            if (is_exp_histogram) {
              SerializeExponentialHistogramJson(payload_writer, agg_temporality, dp_count, has_sum,
                                                dp_sum, has_min, dp_min, has_max, dp_max, exp_scale,
                                                exp_zero_count, exp_zero_threshold, pos_offset,
                                                tls_pos_buckets, neg_offset, tls_neg_buckets);
              emitter.SetString("ExponentialHistogram", payload_writer.Str());
            }

            if (is_summary) {
              SerializeSummaryJson(payload_writer, dp_count, has_sum, dp_sum, tls_quantiles);
              emitter.SetString("Summary", payload_writer.Str());
            }

            emitter.SetInt64("Flags", flags);
            emitter.SetString("Exemplars", exemplars_writer.Str());
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

namespace {

ExtractionResult ProcessMetricsTableImpl(
    lua_State* L, int abs_idx, core::LuaEmitter& emitter, core::FastJsonWriter& res_attrs_writer,
    core::FastJsonWriter& scope_attrs_writer, core::FastJsonWriter& dp_attrs_writer,
    core::FastJsonWriter& exemplars_writer, core::FastJsonWriter& payload_writer,
    std::vector<uint64_t>& tls_bucket_counts, std::vector<double>& tls_explicit_bounds,
    std::vector<uint64_t>& tls_pos_buckets, std::vector<uint64_t>& tls_neg_buckets,
    std::vector<std::pair<double, double>>& tls_quantiles, std::string& tls_scope_name_scratch,
    std::string& tls_scope_ver_scratch, std::string& tls_metric_name_scratch,
    std::string& tls_metric_desc_scratch, std::string& tls_metric_unit_scratch) {
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
    res_attrs_writer.Reset();
    bool has_res_attrs = false;

    if (core::GetTableField(L, rm_obj_idx, "resource")) {
      int res_idx = lua_absindex(L, -1);
      if (core::GetTableField(L, res_idx, "attributes")) {
        core::WriteTableAttributesJson(L, -1, res_attrs_writer, &service_name);
        has_res_attrs = true;
        lua_pop(L, 1);
      }
      lua_pop(L, 1);
    }
    if (!has_res_attrs) {
      res_attrs_writer.StartObject();
      res_attrs_writer.EndObject();
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
      scope_attrs_writer.Reset();
      bool has_scope_attrs = false;

      if (core::GetTableField(L, sm_obj_idx, "scope")) {
        int s_idx = lua_absindex(L, -1);
        scope_name = core::ReadTableStringView(L, s_idx, "name", nullptr, tls_scope_name_scratch);
        scope_version =
            core::ReadTableStringView(L, s_idx, "version", nullptr, tls_scope_ver_scratch);
        if (core::GetTableField(L, s_idx, "attributes")) {
          core::WriteTableAttributesJson(L, -1, scope_attrs_writer);
          has_scope_attrs = true;
          lua_pop(L, 1);
        }
        lua_pop(L, 1);
      }
      if (!has_scope_attrs) {
        scope_attrs_writer.StartObject();
        scope_attrs_writer.EndObject();
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

        std::string_view metric_name =
            core::ReadTableStringView(L, metric_obj_idx, "name", nullptr, tls_metric_name_scratch);
        std::string_view metric_desc = core::ReadTableStringView(L, metric_obj_idx, "description",
                                                                 nullptr, tls_metric_desc_scratch);
        std::string_view metric_unit =
            core::ReadTableStringView(L, metric_obj_idx, "unit", nullptr, tls_metric_unit_scratch);

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

              uint64_t start_nanos =
                  core::ReadTableUint64(L, dp_obj_idx, "startTimeUnixNano", "start_time_unix_nano");
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

              emitter.SetString("ResourceAttributes", res_attrs_writer.Str());
              emitter.SetString("ScopeName", scope_name);
              emitter.SetString("ScopeVersion", scope_version);
              emitter.SetString("ScopeAttributes", scope_attrs_writer.Str());

              dp_attrs_writer.Reset();
              if (core::GetTableField(L, dp_obj_idx, "attributes")) {
                core::WriteTableAttributesJson(L, -1, dp_attrs_writer);
                lua_pop(L, 1);
              } else {
                dp_attrs_writer.StartObject();
                dp_attrs_writer.EndObject();
              }
              emitter.SetString("Attributes", dp_attrs_writer.Str());

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
                uint64_t h_count = core::ReadTableUint64(L, dp_obj_idx, "count", "Count");
                double h_sum = 0.0;
                bool has_sum = core::TryReadTableDouble(L, dp_obj_idx, "sum", "Sum", h_sum);
                double h_min = 0.0;
                bool has_min = core::TryReadTableDouble(L, dp_obj_idx, "min", "Min", h_min);
                double h_max = 0.0;
                bool has_max = core::TryReadTableDouble(L, dp_obj_idx, "max", "Max", h_max);

                tls_bucket_counts.clear();
                if (core::GetTableField(L, dp_obj_idx, "bucketCounts", "bucket_counts",
                                        "BucketCounts")) {
                  ReadTableBucketCountsVector(L, -1, tls_bucket_counts);
                  lua_pop(L, 1);
                }

                tls_explicit_bounds.clear();
                if (core::GetTableField(L, dp_obj_idx, "explicitBounds", "explicit_bounds",
                                        "ExplicitBounds")) {
                  ReadTableExplicitBoundsVector(L, -1, tls_explicit_bounds);
                  lua_pop(L, 1);
                }

                SerializeHistogramJson(payload_writer, agg_temporality, h_count, has_sum, h_sum,
                                       has_min, h_min, has_max, h_max, tls_bucket_counts,
                                       tls_explicit_bounds);
                emitter.SetString("Histogram", payload_writer.Str());
              }

              if (is_exp_histogram) {
                uint64_t eh_count = core::ReadTableUint64(L, dp_obj_idx, "count", "Count");
                double eh_sum = 0.0;
                bool has_sum = core::TryReadTableDouble(L, dp_obj_idx, "sum", "Sum", eh_sum);
                double eh_min = 0.0;
                bool has_min = core::TryReadTableDouble(L, dp_obj_idx, "min", "Min", eh_min);
                double eh_max = 0.0;
                bool has_max = core::TryReadTableDouble(L, dp_obj_idx, "max", "Max", eh_max);

                int64_t eh_scale = core::ReadTableInt64(L, dp_obj_idx, "scale", "Scale", 0);
                uint64_t eh_zero_count =
                    core::ReadTableUint64(L, dp_obj_idx, "zeroCount", "zero_count", 0, "ZeroCount");
                double eh_zero_threshold = core::ReadTableDouble(
                    L, dp_obj_idx, "zeroThreshold", "zero_threshold", 0.0, "ZeroThreshold");

                auto read_exp_buckets = [&](const char* k1, const char* k2, int64_t& out_offset,
                                            std::vector<uint64_t>& out_buckets) {
                  out_offset = 0;
                  out_buckets.clear();
                  if (core::GetTableField(L, dp_obj_idx, k1, k2)) {
                    if (lua_istable(L, -1)) {
                      int b_obj_idx = lua_absindex(L, -1);
                      out_offset = core::ReadTableInt64(L, b_obj_idx, "offset", "Offset", 0);
                      if (core::GetTableField(L, b_obj_idx, "bucketCounts", "bucket_counts",
                                              "BucketCounts")) {
                        ReadTableBucketCountsVector(L, -1, out_buckets);
                        lua_pop(L, 1);
                      }
                    }
                    lua_pop(L, 1);
                  }
                };
                int64_t pos_offset = 0;
                int64_t neg_offset = 0;
                read_exp_buckets("positive", "Positive", pos_offset, tls_pos_buckets);
                read_exp_buckets("negative", "Negative", neg_offset, tls_neg_buckets);

                SerializeExponentialHistogramJson(
                    payload_writer, agg_temporality, eh_count, has_sum, eh_sum, has_min, eh_min,
                    has_max, eh_max, eh_scale, eh_zero_count, eh_zero_threshold, pos_offset,
                    tls_pos_buckets, neg_offset, tls_neg_buckets);
                emitter.SetString("ExponentialHistogram", payload_writer.Str());
              }

              if (is_summary) {
                uint64_t s_count = core::ReadTableUint64(L, dp_obj_idx, "count", "Count");
                double s_sum = 0.0;
                bool has_sum = core::TryReadTableDouble(L, dp_obj_idx, "sum", "Sum", s_sum);

                tls_quantiles.clear();
                if (core::GetTableField(L, dp_obj_idx, "quantileValues", "quantile_values",
                                        "QuantileValues")) {
                  ReadTableQuantilesVector(L, -1, tls_quantiles);
                  lua_pop(L, 1);
                }

                SerializeSummaryJson(payload_writer, s_count, has_sum, s_sum, tls_quantiles);
                emitter.SetString("Summary", payload_writer.Str());
              }

              emitter.SetInt64("Flags",
                               static_cast<int64_t>(static_cast<uint32_t>(
                                   core::ReadTableUint64(L, dp_obj_idx, "flags", "Flags"))));

              exemplars_writer.Reset();
              if (core::GetTableField(L, dp_obj_idx, "exemplars", "Exemplars")) {
                core::SerializeTableExemplarsToJson(L, -1, exemplars_writer);
                lua_pop(L, 1);
              } else {
                exemplars_writer.StartArray();
                exemplars_writer.EndArray();
              }
              emitter.SetString("Exemplars", exemplars_writer.Str());

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
}

}  // namespace

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

  static thread_local core::FastJsonWriter res_attrs_writer;
  static thread_local core::FastJsonWriter scope_attrs_writer;
  static thread_local core::FastJsonWriter dp_attrs_writer;
  static thread_local core::FastJsonWriter exemplars_writer;
  static thread_local core::FastJsonWriter payload_writer;
  static thread_local std::vector<uint64_t> tls_bucket_counts;
  static thread_local std::vector<double> tls_explicit_bounds;
  static thread_local std::vector<uint64_t> tls_pos_buckets;
  static thread_local std::vector<uint64_t> tls_neg_buckets;
  static thread_local std::vector<std::pair<double, double>> tls_quantiles;
  static thread_local std::string tls_scope_name_scratch;
  static thread_local std::string tls_scope_ver_scratch;
  static thread_local std::string tls_metric_name_scratch;
  static thread_local std::string tls_metric_desc_scratch;
  static thread_local std::string tls_metric_unit_scratch;

  res_attrs_writer.Reset();
  scope_attrs_writer.Reset();
  dp_attrs_writer.Reset();
  exemplars_writer.Reset();
  payload_writer.Reset();
  core::ReserveScratchVector(tls_bucket_counts);
  core::ReserveScratchVector(tls_explicit_bounds);
  core::ReserveScratchVector(tls_pos_buckets);
  core::ReserveScratchVector(tls_neg_buckets);
  core::ReserveScratchVector(tls_quantiles);
  core::ReserveScratchString(tls_scope_name_scratch);
  core::ReserveScratchString(tls_scope_ver_scratch);
  core::ReserveScratchString(tls_metric_name_scratch);
  core::ReserveScratchString(tls_metric_desc_scratch);
  core::ReserveScratchString(tls_metric_unit_scratch);

  struct TableExitGuard {
    TableExitGuard(lua_State* l, int t) : L(l), top(t) {}
    TableExitGuard(const TableExitGuard&) = delete;
    TableExitGuard& operator=(const TableExitGuard&) = delete;
    ~TableExitGuard() noexcept {
      if (L != nullptr) {
        lua_settop(L, top);
      }
      res_attrs_writer.Trim();
      scope_attrs_writer.Trim();
      dp_attrs_writer.Trim();
      exemplars_writer.Trim();
      payload_writer.Trim();
      core::ResetScratchVector(tls_bucket_counts);
      core::ResetScratchVector(tls_explicit_bounds);
      core::ResetScratchVector(tls_pos_buckets);
      core::ResetScratchVector(tls_neg_buckets);
      core::ResetScratchVector(tls_quantiles);
      core::ResetScratchString(tls_scope_name_scratch);
      core::ResetScratchString(tls_scope_ver_scratch);
      core::ResetScratchString(tls_metric_name_scratch);
      core::ResetScratchString(tls_metric_desc_scratch);
      core::ResetScratchString(tls_metric_unit_scratch);
    }
    lua_State* L;
    int top;
  } exit_guard{L, entry_top};

  core::LuaEmitter emitter(L, abs_emit_idx);

  try {
    return ProcessMetricsTableImpl(
        L, abs_idx, emitter, res_attrs_writer, scope_attrs_writer, dp_attrs_writer,
        exemplars_writer, payload_writer, tls_bucket_counts, tls_explicit_bounds, tls_pos_buckets,
        tls_neg_buckets, tls_quantiles, tls_scope_name_scratch, tls_scope_ver_scratch,
        tls_metric_name_scratch, tls_metric_desc_scratch, tls_metric_unit_scratch);
  } catch (const std::exception& e) {
    return ExtractionResult{false, std::string("Extraction exception: ") + e.what(),
                            emitter.EmittedCount()};
  } catch (...) {
    return ExtractionResult{false, "Unknown extraction exception", emitter.EmittedCount()};
  }
}

}  // namespace vector_otel::extractors
