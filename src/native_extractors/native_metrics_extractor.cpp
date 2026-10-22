#include "vector_otel/native_extractors/native_metrics_extractor.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if (defined(__unix__) || defined(__APPLE__)) && !defined(_WIN32)
#include <dlfcn.h>
#endif

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

#ifndef VECTOR_NATIVE_MODULE_NAME
#define VECTOR_NATIVE_MODULE_NAME "vector_native"
#endif

constexpr uint64_t kMaxLuaInt = static_cast<uint64_t>(std::numeric_limits<lua_Integer>::max());

struct DroppedMetricsSummary {
  uint32_t distribution{0};
  uint32_t set{0};
  uint32_t sketch{0};
  uint32_t other{0};

  constexpr bool HasDropped() const noexcept { return (distribution + set + sketch + other) > 0; }
  constexpr uint32_t Total() const noexcept { return distribution + set + sketch + other; }
  void LogSummary(std::string_view module_name) const noexcept {
    if (!HasDropped()) return;
    std::fprintf(stdout,
                 "[%.*s] [WARN] Dropped %u unsupported metric item(s) in batch "
                 "(distribution: %u, set: %u, sketch: %u, other: %u)\n",
                 static_cast<int>(module_name.size()), module_name.data(), Total(), distribution,
                 set, sketch, other);
    std::fflush(stdout);
  }
};

std::string_view ResolveModuleName() noexcept {
#if (defined(__unix__) || defined(__APPLE__)) && !defined(_WIN32)
  Dl_info info;
  if (dladdr(reinterpret_cast<void*>(&ProcessNativeMetrics), &info) != 0 &&
      info.dli_fname != nullptr && info.dli_fname[0] != '\0') {
    std::string_view fname(info.dli_fname);
    size_t last_slash = fname.rfind('/');
    if (last_slash != std::string_view::npos) {
      fname = fname.substr(last_slash + 1);
    }
    if (fname.ends_with(".so")) {
      fname.remove_suffix(3);
    } else if (fname.ends_with(".dylib")) {
      fname.remove_suffix(6);
    }
    if (!fname.empty() && fname != "vector_otel_tests") {
      return fname;
    }
  }
#endif
  return VECTOR_NATIVE_MODULE_NAME;
}

bool IsInfinityString(std::string_view sv) noexcept {
  sv = core::TrimJsonWhitespace(sv);
  return core::EqualsIgnoreCaseAscii(sv, "inf") || core::EqualsIgnoreCaseAscii(sv, "+inf") ||
         core::EqualsIgnoreCaseAscii(sv, "infinity") ||
         core::EqualsIgnoreCaseAscii(sv, "+infinity");
}

void ThrowIfSimdjsonSyntaxError(simdjson::error_code err) {
  if (err != simdjson::SUCCESS && err != simdjson::INCORRECT_TYPE) {
    throw std::runtime_error(std::string(simdjson::error_message(err)));
  }
}

bool TryParseJsonFiniteBound(simdjson::ondemand::value& fv, double& out_bound) {
  auto t_res = fv.type();
  if (t_res.error() != simdjson::SUCCESS) {
    ThrowIfSimdjsonSyntaxError(t_res.error());
    return false;
  }
  const auto t = t_res.value();
  if (t == simdjson::ondemand::json_type::number) {
    double d = 0.0;
    if (core::TryParseDouble(fv, d) && std::isfinite(d)) {
      out_bound = d;
      return true;
    }
    return false;
  }
  if (t == simdjson::ondemand::json_type::string) {
    auto s_res = fv.get_string();
    if (s_res.error() != simdjson::SUCCESS) {
      ThrowIfSimdjsonSyntaxError(s_res.error());
      return false;
    }
    std::string_view sv = core::TrimJsonWhitespace(s_res.value());
    if (sv.empty() || IsInfinityString(sv)) {
      return false;
    }
    double d = 0.0;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), d);
    if (ec == std::errc() && ptr == sv.data() + sv.size() && std::isfinite(d)) {
      out_bound = d;
      return true;
    }
  }
  return false;
}

bool TryParseLuaFiniteBound(lua_State* L, int idx, double& out_bound) noexcept {
  int abs_idx = lua_absindex(L, idx);
  int t = lua_type(L, abs_idx);
  if (t == LUA_TNUMBER) {
    double d = lua_tonumber(L, abs_idx);
    if (std::isfinite(d)) {
      out_bound = d;
      return true;
    }
    return false;
  }
  if (t == LUA_TSTRING) {
    size_t len = 0;
    const char* s = lua_tolstring(L, abs_idx, &len);
    if (s == nullptr) {
      return false;
    }
    std::string_view sv = core::TrimJsonWhitespace(std::string_view(s, len));
    if (sv.empty() || IsInfinityString(sv)) {
      return false;
    }
    double d = 0.0;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), d);
    if (ec == std::errc() && ptr == sv.data() + sv.size() && std::isfinite(d)) {
      out_bound = d;
      return true;
    }
  }
  return false;
}

int64_t ParseKindToTemporality(std::string_view kind_sv) noexcept {
  kind_sv = core::TrimJsonWhitespace(kind_sv);
  if (core::EqualsIgnoreCaseAscii(kind_sv, "incremental") ||
      core::EqualsIgnoreCaseAscii(kind_sv, "delta") ||
      core::EqualsIgnoreCaseAscii(kind_sv, "AGGREGATION_TEMPORALITY_DELTA") || kind_sv == "1") {
    return 1;
  }
  if (core::EqualsIgnoreCaseAscii(kind_sv, "absolute") ||
      core::EqualsIgnoreCaseAscii(kind_sv, "cumulative") ||
      core::EqualsIgnoreCaseAscii(kind_sv, "AGGREGATION_TEMPORALITY_CUMULATIVE") ||
      kind_sv == "2") {
    return 2;
  }
  return 0;
}

void ParseJsonAggregationTemporality(simdjson::ondemand::value& fv, int64_t& agg_temporality) {
  auto t_res = fv.type();
  if (t_res.error() != simdjson::SUCCESS) {
    ThrowIfSimdjsonSyntaxError(t_res.error());
    return;
  }
  if (t_res.value() == simdjson::ondemand::json_type::string) {
    auto s_res = fv.get_string();
    if (s_res.error() != simdjson::SUCCESS) {
      ThrowIfSimdjsonSyntaxError(s_res.error());
      return;
    }
    agg_temporality = ParseKindToTemporality(s_res.value());
  } else if (t_res.value() == simdjson::ondemand::json_type::number) {
    int64_t iv = core::ParseInt64(fv);
    agg_temporality = iv >= 0 ? iv : 0;
  }
}

void ReadLuaAggregationTemporality(lua_State* L, int obj_idx, int64_t& agg_temporality) {
  int abs_idx = lua_absindex(L, obj_idx);
  bool found = core::GetTableFieldRaw(L, abs_idx, "kind", "aggregationTemporality",
                                      "aggregation_temporality");
  if (!found) {
    found = core::GetTableFieldRaw(L, abs_idx, "AggregationTemporality");
  }
  if (!found) {
    return;
  }
  if (lua_type(L, -1) == LUA_TSTRING) {
    size_t klen = 0;
    const char* ks = lua_tolstring(L, -1, &klen);
    if (ks != nullptr) {
      agg_temporality = ParseKindToTemporality(std::string_view(ks, klen));
    }
  } else if (lua_type(L, -1) == LUA_TNUMBER) {
    int64_t parsed_temp = 0;
    if (core::TryParseTableInt64At(L, -1, parsed_temp) && parsed_temp >= 0) {
      agg_temporality = parsed_temp;
    }
  }
  lua_pop(L, 1);
}

enum class MetricValueKind { kNone, kDouble, kInt64 };

void ParseJsonMetricScalarValue(simdjson::ondemand::value& fv, MetricValueKind& val_kind,
                                double& double_val, int64_t& int_val) {
  auto t_res = fv.type();
  if (t_res.error() != simdjson::SUCCESS) {
    ThrowIfSimdjsonSyntaxError(t_res.error());
    return;
  }
  const auto t = t_res.value();
  if (t == simdjson::ondemand::json_type::number) {
    auto num_res = fv.get_number();
    if (num_res.error() != simdjson::SUCCESS) {
      ThrowIfSimdjsonSyntaxError(num_res.error());
      return;
    }
    const auto num = num_res.value();
    if (num.is_int64()) {
      int_val = num.get_int64();
      val_kind = MetricValueKind::kInt64;
      return;
    }
    if (num.is_uint64()) {
      uint64_t uv = num.get_uint64();
      if (uv <= kMaxLuaInt) {
        int_val = static_cast<int64_t>(uv);
        val_kind = MetricValueKind::kInt64;
      } else {
        double_val = static_cast<double>(uv);
        val_kind = MetricValueKind::kDouble;
      }
      return;
    }
    if (num.is_double()) {
      double d = num.get_double();
      if (std::isfinite(d)) {
        double_val = d;
        val_kind = MetricValueKind::kDouble;
      }
    }
    return;
  }
  if (t == simdjson::ondemand::json_type::string) {
    auto s_res = fv.get_string();
    if (s_res.error() != simdjson::SUCCESS) {
      ThrowIfSimdjsonSyntaxError(s_res.error());
      return;
    }
    std::string_view sv = core::TrimJsonWhitespace(s_res.value());
    if (sv.empty()) {
      return;
    }
    int64_t iv = 0;
    auto [iptr, iec] = std::from_chars(sv.data(), sv.data() + sv.size(), iv);
    if (iec == std::errc() && iptr == sv.data() + sv.size()) {
      int_val = iv;
      val_kind = MetricValueKind::kInt64;
      return;
    }
    double dv = 0.0;
    auto [dptr, dec] = std::from_chars(sv.data(), sv.data() + sv.size(), dv);
    if (dec == std::errc() && dptr == sv.data() + sv.size() && std::isfinite(dv)) {
      double_val = dv;
      val_kind = MetricValueKind::kDouble;
    }
  }
}

void ParseJsonCounterOrGauge(simdjson::ondemand::value& fv, MetricValueKind& val_kind,
                             double& double_val, int64_t& int_val, int64_t& agg_temporality,
                             core::FastJsonWriter& exemplars_writer, bool& has_exemplars) {
  auto t_res = fv.type();
  if (t_res.error() != simdjson::SUCCESS) {
    ThrowIfSimdjsonSyntaxError(t_res.error());
    return;
  }
  if (t_res.value() == simdjson::ondemand::json_type::object) {
    auto obj_res = fv.get_object();
    if (obj_res.error() != simdjson::SUCCESS) {
      ThrowIfSimdjsonSyntaxError(obj_res.error());
      return;
    }
    for (auto field_res : obj_res.value()) {
      if (field_res.error() != simdjson::SUCCESS) {
        ThrowIfSimdjsonSyntaxError(field_res.error());
        continue;
      }
      auto k_res = field_res.unescaped_key();
      if (k_res.error() != simdjson::SUCCESS) {
        ThrowIfSimdjsonSyntaxError(k_res.error());
        continue;
      }
      std::string_view k = k_res.value();
      auto v_res = field_res.value();
      if (v_res.error() != simdjson::SUCCESS) {
        ThrowIfSimdjsonSyntaxError(v_res.error());
        continue;
      }
      auto v = v_res.value();
      if (k == "value" || k == "asDouble" || k == "as_double") {
        ParseJsonMetricScalarValue(v, val_kind, double_val, int_val);
      } else if (k == "asInt" || k == "as_int") {
        int_val = core::ParseInt64(v);
        val_kind = MetricValueKind::kInt64;
      } else if (k == "aggregationTemporality" || k == "aggregation_temporality" ||
                 k == "AggregationTemporality") {
        ParseJsonAggregationTemporality(v, agg_temporality);
      } else if (k == "exemplars" || k == "Exemplars") {
        exemplars_writer.Reset();
        core::SerializeExemplarsJson(v, exemplars_writer);
        has_exemplars = true;
      }
    }
  } else {
    ParseJsonMetricScalarValue(fv, val_kind, double_val, int_val);
  }
}

void ParseJsonBucketCountsArray(simdjson::ondemand::value& v, std::vector<uint64_t>& out) {
  out.clear();
  auto arr_res = v.get_array();
  if (arr_res.error() != simdjson::SUCCESS) {
    ThrowIfSimdjsonSyntaxError(arr_res.error());
    return;
  }
  for (auto elem_res : arr_res.value()) {
    if (elem_res.error() != simdjson::SUCCESS) {
      ThrowIfSimdjsonSyntaxError(elem_res.error());
      continue;
    }
    auto elem_val = elem_res.value();
    out.push_back(core::ParseUint64(elem_val));
  }
}

void ParseJsonHistogram(simdjson::ondemand::value& fv, int64_t& agg_temporality,
                        uint64_t& hist_count, bool& has_sum, double& hist_sum, bool& has_min,
                        double& hist_min, bool& has_max, double& hist_max,
                        std::vector<uint64_t>& bucket_counts, std::vector<double>& explicit_bounds,
                        core::FastJsonWriter& exemplars_writer, bool& has_exemplars) {
  bucket_counts.clear();
  explicit_bounds.clear();
  auto obj_res = fv.get_object();
  if (obj_res.error() != simdjson::SUCCESS) {
    ThrowIfSimdjsonSyntaxError(obj_res.error());
    return;
  }
  for (auto field_res : obj_res.value()) {
    if (field_res.error() != simdjson::SUCCESS) {
      ThrowIfSimdjsonSyntaxError(field_res.error());
      continue;
    }
    auto k_res = field_res.unescaped_key();
    if (k_res.error() != simdjson::SUCCESS) {
      ThrowIfSimdjsonSyntaxError(k_res.error());
      continue;
    }
    std::string_view k = k_res.value();
    auto v_res = field_res.value();
    if (v_res.error() != simdjson::SUCCESS) {
      ThrowIfSimdjsonSyntaxError(v_res.error());
      continue;
    }
    auto v = v_res.value();

    if (k == "count" || k == "Count") {
      hist_count = core::ParseUint64(v);
    } else if (k == "sum" || k == "Sum") {
      if (core::TryParseDouble(v, hist_sum)) {
        has_sum = true;
      }
    } else if (k == "min" || k == "Min") {
      if (core::TryParseDouble(v, hist_min)) {
        has_min = true;
      }
    } else if (k == "max" || k == "Max") {
      if (core::TryParseDouble(v, hist_max)) {
        has_max = true;
      }
    } else if (k == "aggregationTemporality" || k == "aggregation_temporality" ||
               k == "AggregationTemporality") {
      ParseJsonAggregationTemporality(v, agg_temporality);
    } else if (k == "buckets") {
      auto arr_res = v.get_array();
      if (arr_res.error() != simdjson::SUCCESS) {
        ThrowIfSimdjsonSyntaxError(arr_res.error());
      } else {
        for (auto elem_res : arr_res.value()) {
          if (elem_res.error() != simdjson::SUCCESS) {
            ThrowIfSimdjsonSyntaxError(elem_res.error());
            continue;
          }
          auto elem_val = elem_res.value();
          auto et_res = elem_val.type();
          if (et_res.error() != simdjson::SUCCESS) {
            ThrowIfSimdjsonSyntaxError(et_res.error());
            continue;
          }
          if (et_res.value() == simdjson::ondemand::json_type::object) {
            auto b_obj_res = elem_val.get_object();
            if (b_obj_res.error() != simdjson::SUCCESS) {
              ThrowIfSimdjsonSyntaxError(b_obj_res.error());
              continue;
            }
            uint64_t bc = 0;
            for (auto bf_res : b_obj_res.value()) {
              if (bf_res.error() != simdjson::SUCCESS) {
                ThrowIfSimdjsonSyntaxError(bf_res.error());
                continue;
              }
              auto bk_res = bf_res.unescaped_key();
              if (bk_res.error() != simdjson::SUCCESS) {
                ThrowIfSimdjsonSyntaxError(bk_res.error());
                continue;
              }
              std::string_view bk = bk_res.value();
              auto bfv_res = bf_res.value();
              if (bfv_res.error() != simdjson::SUCCESS) {
                ThrowIfSimdjsonSyntaxError(bfv_res.error());
                continue;
              }
              auto bfv = bfv_res.value();
              if (bk == "count" || bk == "Count") {
                bc = core::ParseUint64(bfv);
              } else if (bk == "upper_limit" || bk == "upperLimit") {
                double bound = 0.0;
                if (TryParseJsonFiniteBound(bfv, bound)) {
                  explicit_bounds.push_back(bound);
                }
              }
            }
            bucket_counts.push_back(bc);
          } else {
            double bound = 0.0;
            if (TryParseJsonFiniteBound(elem_val, bound)) {
              explicit_bounds.push_back(bound);
            }
          }
        }
      }
    } else if (k == "counts" || k == "bucketCounts" || k == "bucket_counts" ||
               k == "BucketCounts") {
      ParseJsonBucketCountsArray(v, bucket_counts);
    } else if (k == "explicitBounds" || k == "explicit_bounds" || k == "ExplicitBounds") {
      auto arr_res = v.get_array();
      if (arr_res.error() != simdjson::SUCCESS) {
        ThrowIfSimdjsonSyntaxError(arr_res.error());
      } else {
        for (auto elem_res : arr_res.value()) {
          if (elem_res.error() != simdjson::SUCCESS) {
            ThrowIfSimdjsonSyntaxError(elem_res.error());
            continue;
          }
          auto elem_val = elem_res.value();
          double bound = 0.0;
          if (TryParseJsonFiniteBound(elem_val, bound)) {
            explicit_bounds.push_back(bound);
          }
        }
      }
    } else if (k == "exemplars" || k == "Exemplars") {
      exemplars_writer.Reset();
      core::SerializeExemplarsJson(v, exemplars_writer);
      has_exemplars = true;
    }
  }
}

void ParseJsonExpBucketsSubObject(simdjson::ondemand::value& v, int64_t& out_offset,
                                  std::vector<uint64_t>& out_buckets) {
  out_offset = 0;
  out_buckets.clear();
  auto obj_res = v.get_object();
  if (obj_res.error() != simdjson::SUCCESS) {
    ThrowIfSimdjsonSyntaxError(obj_res.error());
    return;
  }
  for (auto f_res : obj_res.value()) {
    if (f_res.error() != simdjson::SUCCESS) {
      ThrowIfSimdjsonSyntaxError(f_res.error());
      continue;
    }
    auto k_res = f_res.unescaped_key();
    if (k_res.error() != simdjson::SUCCESS) {
      ThrowIfSimdjsonSyntaxError(k_res.error());
      continue;
    }
    std::string_view k = k_res.value();
    auto fv_res = f_res.value();
    if (fv_res.error() != simdjson::SUCCESS) {
      ThrowIfSimdjsonSyntaxError(fv_res.error());
      continue;
    }
    auto fv = fv_res.value();
    if (k == "offset" || k == "Offset") {
      out_offset = core::ParseInt64(fv);
    } else if (k == "counts" || k == "bucketCounts" || k == "bucket_counts" ||
               k == "BucketCounts") {
      ParseJsonBucketCountsArray(fv, out_buckets);
    }
  }
}

void ParseJsonExponentialHistogram(simdjson::ondemand::value& fv, int64_t& agg_temporality,
                                   uint64_t& exp_count, bool& has_sum, double& exp_sum,
                                   bool& has_min, double& exp_min, bool& has_max, double& exp_max,
                                   int64_t& exp_scale, uint64_t& exp_zero_count,
                                   double& exp_zero_threshold, int64_t& pos_offset,
                                   std::vector<uint64_t>& pos_buckets, int64_t& neg_offset,
                                   std::vector<uint64_t>& neg_buckets,
                                   core::FastJsonWriter& exemplars_writer, bool& has_exemplars) {
  pos_offset = 0;
  neg_offset = 0;
  pos_buckets.clear();
  neg_buckets.clear();
  auto obj_res = fv.get_object();
  if (obj_res.error() != simdjson::SUCCESS) {
    ThrowIfSimdjsonSyntaxError(obj_res.error());
    return;
  }
  for (auto field_res : obj_res.value()) {
    if (field_res.error() != simdjson::SUCCESS) {
      ThrowIfSimdjsonSyntaxError(field_res.error());
      continue;
    }
    auto k_res = field_res.unescaped_key();
    if (k_res.error() != simdjson::SUCCESS) {
      ThrowIfSimdjsonSyntaxError(k_res.error());
      continue;
    }
    std::string_view k = k_res.value();
    auto v_res = field_res.value();
    if (v_res.error() != simdjson::SUCCESS) {
      ThrowIfSimdjsonSyntaxError(v_res.error());
      continue;
    }
    auto v = v_res.value();

    if (k == "count" || k == "Count") {
      exp_count = core::ParseUint64(v);
    } else if (k == "sum" || k == "Sum") {
      if (core::TryParseDouble(v, exp_sum)) {
        has_sum = true;
      }
    } else if (k == "min" || k == "Min") {
      if (core::TryParseDouble(v, exp_min)) {
        has_min = true;
      }
    } else if (k == "max" || k == "Max") {
      if (core::TryParseDouble(v, exp_max)) {
        has_max = true;
      }
    } else if (k == "scale" || k == "Scale") {
      exp_scale = core::ParseInt64(v);
    } else if (k == "zeroCount" || k == "zero_count" || k == "ZeroCount") {
      exp_zero_count = core::ParseUint64(v);
    } else if (k == "zeroThreshold" || k == "zero_threshold" || k == "ZeroThreshold") {
      (void)core::TryParseDouble(v, exp_zero_threshold);
    } else if (k == "aggregationTemporality" || k == "aggregation_temporality" ||
               k == "AggregationTemporality") {
      ParseJsonAggregationTemporality(v, agg_temporality);
    } else if (k == "positive" || k == "Positive") {
      ParseJsonExpBucketsSubObject(v, pos_offset, pos_buckets);
    } else if (k == "negative" || k == "Negative") {
      ParseJsonExpBucketsSubObject(v, neg_offset, neg_buckets);
    } else if (k == "exemplars" || k == "Exemplars") {
      exemplars_writer.Reset();
      core::SerializeExemplarsJson(v, exemplars_writer);
      has_exemplars = true;
    }
  }
}

using core::SummaryQuantile;

void ParseJsonSummary(simdjson::ondemand::value& fv, uint64_t& sum_count, bool& has_sum,
                      double& sum_total, std::vector<SummaryQuantile>& quantiles,
                      std::vector<double>& scratch_bounds, core::FastJsonWriter& exemplars_writer,
                      bool& has_exemplars) {
  quantiles.clear();
  scratch_bounds.clear();
  auto obj_res = fv.get_object();
  if (obj_res.error() != simdjson::SUCCESS) {
    ThrowIfSimdjsonSyntaxError(obj_res.error());
    return;
  }
  bool has_parallel_quantiles = false;
  for (auto field_res : obj_res.value()) {
    if (field_res.error() != simdjson::SUCCESS) {
      ThrowIfSimdjsonSyntaxError(field_res.error());
      continue;
    }
    auto k_res = field_res.unescaped_key();
    if (k_res.error() != simdjson::SUCCESS) {
      ThrowIfSimdjsonSyntaxError(k_res.error());
      continue;
    }
    std::string_view k = k_res.value();
    auto v_res = field_res.value();
    if (v_res.error() != simdjson::SUCCESS) {
      ThrowIfSimdjsonSyntaxError(v_res.error());
      continue;
    }
    auto v = v_res.value();

    if (k == "count" || k == "Count") {
      sum_count = core::ParseUint64(v);
    } else if (k == "sum" || k == "Sum") {
      if (core::TryParseDouble(v, sum_total)) {
        has_sum = true;
      }
    } else if (k == "quantiles" || k == "quantileValues" || k == "quantile_values" ||
               k == "QuantileValues") {
      auto arr_res = v.get_array();
      if (arr_res.error() != simdjson::SUCCESS) {
        ThrowIfSimdjsonSyntaxError(arr_res.error());
      } else {
        for (auto elem_res : arr_res.value()) {
          if (elem_res.error() != simdjson::SUCCESS) {
            ThrowIfSimdjsonSyntaxError(elem_res.error());
            continue;
          }
          auto elem_val = elem_res.value();
          auto et_res = elem_val.type();
          if (et_res.error() != simdjson::SUCCESS) {
            ThrowIfSimdjsonSyntaxError(et_res.error());
            continue;
          }
          if (et_res.value() == simdjson::ondemand::json_type::object) {
            auto q_obj_res = elem_val.get_object();
            if (q_obj_res.error() != simdjson::SUCCESS) {
              ThrowIfSimdjsonSyntaxError(q_obj_res.error());
              continue;
            }
            SummaryQuantile sq{};
            for (auto qf_res : q_obj_res.value()) {
              if (qf_res.error() != simdjson::SUCCESS) {
                ThrowIfSimdjsonSyntaxError(qf_res.error());
                continue;
              }
              auto qk_res = qf_res.unescaped_key();
              if (qk_res.error() != simdjson::SUCCESS) {
                ThrowIfSimdjsonSyntaxError(qk_res.error());
                continue;
              }
              std::string_view qk = qk_res.value();
              auto qfv_res = qf_res.value();
              if (qfv_res.error() != simdjson::SUCCESS) {
                ThrowIfSimdjsonSyntaxError(qfv_res.error());
                continue;
              }
              auto qfv = qfv_res.value();
              if (qk == "quantile" || qk == "Quantile") {
                sq.quantile = core::ParseDouble(qfv);
              } else if (qk == "value" || qk == "Value") {
                sq.value = core::ParseDouble(qfv);
              }
            }
            quantiles.push_back(sq);
          } else {
            has_parallel_quantiles = true;
            SummaryQuantile sq{};
            sq.quantile = core::ParseDouble(elem_val);
            quantiles.push_back(sq);
          }
        }
      }
    } else if (k == "values" || k == "Values") {
      auto arr_res = v.get_array();
      if (arr_res.error() != simdjson::SUCCESS) {
        ThrowIfSimdjsonSyntaxError(arr_res.error());
      } else {
        for (auto elem_res : arr_res.value()) {
          if (elem_res.error() != simdjson::SUCCESS) {
            ThrowIfSimdjsonSyntaxError(elem_res.error());
            continue;
          }
          auto elem_val = elem_res.value();
          scratch_bounds.push_back(core::ParseDouble(elem_val));
        }
      }
    } else if (k == "exemplars" || k == "Exemplars") {
      exemplars_writer.Reset();
      core::SerializeExemplarsJson(v, exemplars_writer);
      has_exemplars = true;
    }
  }
  if (has_parallel_quantiles && !scratch_bounds.empty()) {
    const size_t n = std::min(quantiles.size(), scratch_bounds.size());
    quantiles.resize(n);
    for (size_t i = 0; i < n; ++i) {
      quantiles[i].value = scratch_bounds[i];
    }
  }
}

struct JsonTagValue {
  enum class Kind { kNone, kString, kInt64, kUint64, kDouble, kBool, kRawJson } kind = Kind::kNone;
  std::string_view str_val;
  int64_t int_val = 0;
  uint64_t uint_val = 0;
  double dbl_val = 0.0;
  bool bool_val = false;
};

struct DeferredJsonTag {
  enum class Target { kResource, kScope, kDatapoint } target = Target::kDatapoint;
  size_t key_offset = 0;
  size_t key_len = 0;
  JsonTagValue::Kind kind = JsonTagValue::Kind::kNone;
  size_t str_offset = 0;
  size_t str_len = 0;
  int64_t int_val = 0;
  uint64_t uint_val = 0;
  double dbl_val = 0.0;
  bool bool_val = false;
};

// Reusable per-thread staging buffers (capacity retained between invocations). A `metric` / `log`
// wrapper nests at most one level (`depth == 0` only), and the enclosing object has already parsed
// part of its state into its buffers when the wrapper is reached, so level 1 gets its own set:
// an unmatched wrapper must not clobber the enclosing object's staged fields.
struct NativeMetricsScratch {
  core::FastJsonWriter res_attrs_writer;
  core::FastJsonWriter scope_attrs_writer;
  core::FastJsonWriter dp_attrs_writer;
  core::FastJsonWriter exemplars_writer;
  core::FastJsonWriter payload_writer;
  std::string metric_name;
  std::string service;
  std::string scope_name;
  std::string scope_ver;
  std::string metric_desc;
  std::string metric_unit;
  std::vector<uint64_t> bucket_counts;
  std::vector<double> explicit_bounds;
  std::vector<uint64_t> pos_bucket_counts;
  std::vector<uint64_t> neg_bucket_counts;
  std::vector<SummaryQuantile> summary_quantiles;
  std::vector<DeferredJsonTag> deferred_tags;
  std::string tag_arena;
};

constexpr int kScratchLevels = 2;

NativeMetricsScratch& ScratchLevel(int level) {
  static thread_local NativeMetricsScratch levels[kScratchLevels];
  return levels[level];
}

void ResetScratch(NativeMetricsScratch& s) {
  s.res_attrs_writer.Reset();
  s.scope_attrs_writer.Reset();
  s.dp_attrs_writer.Reset();
  s.exemplars_writer.Reset();
  s.payload_writer.Reset();
  core::ReserveScratchString(s.metric_name);
  core::ReserveScratchString(s.service);
  core::ReserveScratchString(s.scope_name);
  core::ReserveScratchString(s.scope_ver);
  core::ReserveScratchString(s.metric_desc);
  core::ReserveScratchString(s.metric_unit);
  core::ReserveScratchVector(s.bucket_counts);
  core::ReserveScratchVector(s.explicit_bounds);
  core::ReserveScratchVector(s.pos_bucket_counts);
  core::ReserveScratchVector(s.neg_bucket_counts);
  core::ReserveScratchVector(s.summary_quantiles);
  core::ReserveScratchVector(s.deferred_tags);
  core::ReserveScratchString(s.tag_arena);
}

void TrimScratch(NativeMetricsScratch& s) noexcept {
  s.res_attrs_writer.Trim();
  s.scope_attrs_writer.Trim();
  s.dp_attrs_writer.Trim();
  s.exemplars_writer.Trim();
  s.payload_writer.Trim();
  core::ResetScratchVector(s.bucket_counts);
  core::ResetScratchVector(s.explicit_bounds);
  core::ResetScratchVector(s.pos_bucket_counts);
  core::ResetScratchVector(s.neg_bucket_counts);
  core::ResetScratchVector(s.summary_quantiles);
  core::ResetScratchVector(s.deferred_tags);
  core::TrimScratchString(s.tag_arena);
  core::TrimScratchString(s.metric_name);
  core::TrimScratchString(s.service);
  core::TrimScratchString(s.scope_name);
  core::TrimScratchString(s.scope_ver);
  core::TrimScratchString(s.metric_desc);
  core::TrimScratchString(s.metric_unit);
}

void TrimAllScratch() noexcept {
  for (int i = 0; i < kScratchLevels; ++i) {
    TrimScratch(ScratchLevel(i));
  }
  core::ResetServiceNameScratch();
}

void ReadSingleJsonTagElement(simdjson::ondemand::value& v, core::FastJsonWriter& scratch_writer,
                              JsonTagValue& out) {
  auto t_res = v.type();
  if (t_res.error() != simdjson::SUCCESS) {
    ThrowIfSimdjsonSyntaxError(t_res.error());
    return;
  }
  const auto t = t_res.value();
  if (t == simdjson::ondemand::json_type::string) {
    auto s_res = v.get_string();
    if (s_res.error() == simdjson::SUCCESS) {
      out.kind = JsonTagValue::Kind::kString;
      out.str_val = s_res.value();
    } else {
      ThrowIfSimdjsonSyntaxError(s_res.error());
    }
  } else if (t == simdjson::ondemand::json_type::number) {
    auto num_res = v.get_number();
    if (num_res.error() == simdjson::SUCCESS) {
      const auto num = num_res.value();
      if (num.is_int64()) {
        out.kind = JsonTagValue::Kind::kInt64;
        out.int_val = num.get_int64();
      } else if (num.is_uint64()) {
        out.kind = JsonTagValue::Kind::kUint64;
        out.uint_val = num.get_uint64();
      } else if (num.is_double()) {
        out.kind = JsonTagValue::Kind::kDouble;
        out.dbl_val = num.get_double();
      }
    } else {
      ThrowIfSimdjsonSyntaxError(num_res.error());
    }
  } else if (t == simdjson::ondemand::json_type::boolean) {
    auto b_res = v.get_bool();
    if (b_res.error() == simdjson::SUCCESS) {
      out.kind = JsonTagValue::Kind::kBool;
      out.bool_val = b_res.value();
    } else {
      ThrowIfSimdjsonSyntaxError(b_res.error());
    }
  } else if (t == simdjson::ondemand::json_type::object) {
    scratch_writer.Reset();
    core::WriteAnyValueJson(scratch_writer, v);
    out.kind = JsonTagValue::Kind::kRawJson;
    out.str_val = scratch_writer.Str();
  }
}

void WriteJsonTagValueToWriter(core::FastJsonWriter& dst_writer, std::string_view key,
                               const JsonTagValue& tv) {
  if (tv.kind == JsonTagValue::Kind::kNone) {
    return;
  }
  dst_writer.Key(key);
  switch (tv.kind) {
    case JsonTagValue::Kind::kString:
      dst_writer.String(tv.str_val);
      break;
    case JsonTagValue::Kind::kInt64:
      dst_writer.Int64(tv.int_val);
      break;
    case JsonTagValue::Kind::kUint64:
      dst_writer.Uint64(tv.uint_val);
      break;
    case JsonTagValue::Kind::kDouble:
      dst_writer.Double(tv.dbl_val);
      break;
    case JsonTagValue::Kind::kBool:
      dst_writer.Bool(tv.bool_val);
      break;
    case JsonTagValue::Kind::kRawJson:
      dst_writer.RawJson(tv.str_val);
      break;
    case JsonTagValue::Kind::kNone:
      dst_writer.Null();
      break;
  }
}

bool ExtractNativeMetricObjectJson(
    lua_State* L, simdjson::ondemand::object& rec_obj, std::string_view processed_at,
    core::FastJsonWriter& res_attrs_writer, core::FastJsonWriter& scope_attrs_writer,
    core::FastJsonWriter& dp_attrs_writer, core::FastJsonWriter& exemplars_writer,
    core::FastJsonWriter& payload_writer, std::string& tls_metric_name_scratch,
    std::string& tls_service_scratch, std::string& tls_scope_name_scratch,
    std::string& tls_scope_ver_scratch, std::string& tls_metric_desc_scratch,
    std::string& tls_metric_unit_scratch, std::vector<uint64_t>& tls_bucket_counts,
    std::vector<double>& tls_explicit_bounds, std::vector<uint64_t>& tls_pos_bucket_counts,
    std::vector<uint64_t>& tls_neg_bucket_counts,
    std::vector<SummaryQuantile>& tls_summary_quantiles, core::RowEmitter& emitter,
    std::string& err_out, DroppedMetricsSummary& dropped_summary, int depth = 0,
    bool* out_matched = nullptr) {
  NativeMetricsScratch& level = ScratchLevel(depth);
  std::vector<DeferredJsonTag>& tls_deferred_tags = level.deferred_tags;
  std::string& tls_tag_arena = level.tag_arena;
  // Recurses into a wrapper's object with the next level's buffers.
  auto extract_wrapped = [&](simdjson::ondemand::object& inner, bool* matched) -> bool {
    NativeMetricsScratch& n = ScratchLevel(depth + 1);
    ResetScratch(n);
    return ExtractNativeMetricObjectJson(
        L, inner, processed_at, n.res_attrs_writer, n.scope_attrs_writer, n.dp_attrs_writer,
        n.exemplars_writer, n.payload_writer, n.metric_name, n.service, n.scope_name, n.scope_ver,
        n.metric_desc, n.metric_unit, n.bucket_counts, n.explicit_bounds, n.pos_bucket_counts,
        n.neg_bucket_counts, n.summary_quantiles, emitter, err_out, dropped_summary, depth + 1,
        matched);
  };
  uint64_t t_ns = 0;
  uint64_t start_ns = 0;
  uint64_t interval_ms = 0;
  bool has_ts = false;
  bool has_start_ts = false;
  bool has_interval_ms = false;

  std::string_view raw_name;
  std::string_view metric_ns;
  std::string_view metric_desc;
  std::string_view metric_unit;
  std::string_view metric_type;
  std::string_view service_name;
  std::string_view top_service_name;
  std::string_view scope_name;
  std::string_view scope_version;

  MetricValueKind val_kind = MetricValueKind::kNone;
  double double_val = 0.0;
  int64_t int_val = 0;
  int64_t flags = 0;
  int64_t agg_temporality = 0;
  bool has_is_monotonic = false;
  bool is_monotonic = false;

  uint64_t hist_count = 0;
  bool has_sum = false;
  double hist_sum = 0.0;
  bool has_min = false;
  double hist_min = 0.0;
  bool has_max = false;
  double hist_max = 0.0;
  int64_t exp_scale = 0;
  uint64_t exp_zero_count = 0;
  double exp_zero_threshold = 0.0;
  int64_t pos_offset = 0;
  int64_t neg_offset = 0;

  tls_bucket_counts.clear();
  tls_explicit_bounds.clear();
  tls_pos_bucket_counts.clear();
  tls_neg_bucket_counts.clear();
  tls_summary_quantiles.clear();

  bool has_res_attrs = false;
  bool has_scope_attrs = false;
  bool has_dp_attrs = false;
  bool has_exemplars = false;
  bool has_seen_tags = false;

  size_t tag_res_svc_off = 0, tag_res_svc_len = 0;
  bool has_tag_res_svc = false;
  size_t tag_dp_svc_off = 0, tag_dp_svc_len = 0;
  bool has_tag_dp_svc = false;
  size_t tag_scope_name_off = 0, tag_scope_name_len = 0;
  bool has_tag_scope_name = false;
  size_t tag_scope_ver_off = 0, tag_scope_ver_len = 0;
  bool has_tag_scope_ver = false;
  size_t tag_otel_scope_name_off = 0, tag_otel_scope_name_len = 0;
  bool has_tag_otel_scope_name = false;
  size_t tag_otel_scope_ver_off = 0, tag_otel_scope_ver_len = 0;
  bool has_tag_otel_scope_ver = false;
  size_t tag_unit_off = 0, tag_unit_len = 0;
  bool has_tag_unit = false;
  size_t tag_desc_off = 0, tag_desc_len = 0;
  bool has_tag_desc = false;

  auto append_to_arena = [&tls_tag_arena](std::string_view sv, size_t& out_off, size_t& out_len) {
    out_off = tls_tag_arena.size();
    out_len = sv.size();
    tls_tag_arena.append(sv.data(), sv.size());
  };

  auto push_deferred_tag = [&](DeferredJsonTag::Target target, std::string_view key,
                               const JsonTagValue& tv) {
    DeferredJsonTag dt{};
    dt.target = target;
    append_to_arena(key, dt.key_offset, dt.key_len);
    dt.kind = tv.kind;
    if (tv.kind == JsonTagValue::Kind::kString || tv.kind == JsonTagValue::Kind::kRawJson) {
      append_to_arena(tv.str_val, dt.str_offset, dt.str_len);
    } else if (tv.kind == JsonTagValue::Kind::kInt64) {
      dt.int_val = tv.int_val;
    } else if (tv.kind == JsonTagValue::Kind::kUint64) {
      dt.uint_val = tv.uint_val;
    } else if (tv.kind == JsonTagValue::Kind::kDouble) {
      dt.dbl_val = tv.dbl_val;
    } else if (tv.kind == JsonTagValue::Kind::kBool) {
      dt.bool_val = tv.bool_val;
    }
    tls_deferred_tags.push_back(dt);
  };

  auto ensure_res_attrs_open = [&]() {
    if (!has_res_attrs) {
      res_attrs_writer.Reset();
      res_attrs_writer.StartObject();
      has_res_attrs = true;
    }
  };
  auto ensure_scope_attrs_open = [&]() {
    if (!has_scope_attrs) {
      scope_attrs_writer.Reset();
      scope_attrs_writer.StartObject();
      has_scope_attrs = true;
    }
  };
  auto ensure_dp_attrs_open = [&]() {
    if (!has_dp_attrs) {
      dp_attrs_writer.Reset();
      dp_attrs_writer.StartObject();
      has_dp_attrs = true;
    }
  };

  bool has_recognized_field = false;
  bool has_any_field = false;

  for (auto field_res : rec_obj) {
    has_any_field = true;
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

    if ((k == "metric" || k == "log") && depth == 0) {
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
        if (!extract_wrapped(inner_obj, &inner_matched)) {
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
          if (!extract_wrapped(elem_obj, &elem_matched)) {
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

    if (k == "distribution" || k == "set" || k == "sketch") {
      auto t_res = fv.type();
      if (t_res.error() == simdjson::SUCCESS &&
          t_res.value() != simdjson::ondemand::json_type::null) {
        if (k == "distribution") {
          dropped_summary.distribution++;
        } else if (k == "set") {
          dropped_summary.set++;
        } else {
          dropped_summary.sketch++;
        }
        if (out_matched != nullptr) {
          *out_matched = true;
        }
        return true;
      }
      continue;
    }

    if (k == "name") {
      has_recognized_field = true;
      auto s_res = fv.get_string();
      if (s_res.error() == simdjson::SUCCESS) {
        raw_name = s_res.value();
      }
    } else if (k == "namespace") {
      has_recognized_field = true;
      auto s_res = fv.get_string();
      if (s_res.error() == simdjson::SUCCESS) {
        metric_ns = s_res.value();
      }
    } else if (k == "description") {
      has_recognized_field = true;
      auto s_res = fv.get_string();
      if (s_res.error() == simdjson::SUCCESS) {
        metric_desc = s_res.value();
      }
    } else if (k == "unit") {
      has_recognized_field = true;
      auto s_res = fv.get_string();
      if (s_res.error() == simdjson::SUCCESS) {
        metric_unit = s_res.value();
      }
    } else if (k == "timestamp" || k == "timeUnixNano" || k == "time_unix_nano") {
      has_recognized_field = true;
      uint64_t parsed = 0;
      if (core::ParseJsonTimestamp(fv, parsed)) {
        t_ns = parsed;
        has_ts = true;
      }
    } else if (k == "start_time_unix_nano" || k == "startTimeUnixNano" || k == "start_timestamp") {
      has_recognized_field = true;
      uint64_t parsed = 0;
      if (core::ParseJsonTimestamp(fv, parsed)) {
        start_ns = parsed;
        has_start_ts = true;
      }
    } else if (k == "interval_ms") {
      has_recognized_field = true;
      uint64_t iv = core::ParseUint64(fv);
      if (iv > 0) {
        interval_ms = iv;
        has_interval_ms = true;
      }
    } else if (k == "flags") {
      has_recognized_field = true;
      flags = core::ParseInt64(fv);
    } else if (k == "kind" || k == "aggregationTemporality" || k == "aggregation_temporality") {
      has_recognized_field = true;
      ParseJsonAggregationTemporality(fv, agg_temporality);
    } else if (k == "isMonotonic" || k == "is_monotonic") {
      has_recognized_field = true;
      auto b_res = fv.get_bool();
      if (b_res.error() == simdjson::SUCCESS) {
        is_monotonic = b_res.value();
        has_is_monotonic = true;
      }
    } else if (k == "counter" || k == "sum") {
      has_recognized_field = true;
      metric_type = "sum";
      ParseJsonCounterOrGauge(fv, val_kind, double_val, int_val, agg_temporality, exemplars_writer,
                              has_exemplars);
    } else if (k == "gauge") {
      has_recognized_field = true;
      metric_type = "gauge";
      ParseJsonCounterOrGauge(fv, val_kind, double_val, int_val, agg_temporality, exemplars_writer,
                              has_exemplars);
    } else if (k == "aggregated_histogram" || k == "histogram") {
      has_recognized_field = true;
      metric_type = "histogram";
      ParseJsonHistogram(fv, agg_temporality, hist_count, has_sum, hist_sum, has_min, hist_min,
                         has_max, hist_max, tls_bucket_counts, tls_explicit_bounds,
                         exemplars_writer, has_exemplars);
    } else if (k == "exponential_histogram" || k == "exponentialHistogram" ||
               k == "aggregated_exponential_histogram") {
      has_recognized_field = true;
      metric_type = "exponential_histogram";
      ParseJsonExponentialHistogram(
          fv, agg_temporality, hist_count, has_sum, hist_sum, has_min, hist_min, has_max, hist_max,
          exp_scale, exp_zero_count, exp_zero_threshold, pos_offset, tls_pos_bucket_counts,
          neg_offset, tls_neg_bucket_counts, exemplars_writer, has_exemplars);
    } else if (k == "aggregated_summary" || k == "summary") {
      has_recognized_field = true;
      metric_type = "summary";
      ParseJsonSummary(fv, hist_count, has_sum, hist_sum, tls_summary_quantiles,
                       tls_explicit_bounds, exemplars_writer, has_exemplars);
    } else if (k == "exemplars" || k == "Exemplars") {
      has_recognized_field = true;
      exemplars_writer.Reset();
      core::SerializeExemplarsJson(fv, exemplars_writer);
      has_exemplars = true;
    } else if (k == "tags") {
      has_recognized_field = true;
      if (!has_seen_tags) {
        tls_deferred_tags.clear();
        tls_tag_arena.clear();
        has_seen_tags = true;
      }
      auto tags_obj_res = fv.get_object();
      if (tags_obj_res.error() == simdjson::SUCCESS) {
        for (auto tf_res : tags_obj_res.value()) {
          if (tf_res.error() != simdjson::SUCCESS) {
            ThrowIfSimdjsonSyntaxError(tf_res.error());
            continue;
          }
          auto tk_res = tf_res.unescaped_key();
          if (tk_res.error() != simdjson::SUCCESS) {
            ThrowIfSimdjsonSyntaxError(tk_res.error());
            continue;
          }
          std::string_view tk = tk_res.value();
          auto tv_res = tf_res.value();
          if (tv_res.error() != simdjson::SUCCESS) {
            ThrowIfSimdjsonSyntaxError(tv_res.error());
            continue;
          }
          auto tv = tv_res.value();

          JsonTagValue tag_val;
          auto tv_type_res = tv.type();
          if (tv_type_res.error() != simdjson::SUCCESS) {
            ThrowIfSimdjsonSyntaxError(tv_type_res.error());
            continue;
          }
          if (tv_type_res.value() == simdjson::ondemand::json_type::array) {
            auto arr_res = tv.get_array();
            if (arr_res.error() == simdjson::SUCCESS) {
              for (auto elem_res : arr_res.value()) {
                if (elem_res.error() != simdjson::SUCCESS) {
                  ThrowIfSimdjsonSyntaxError(elem_res.error());
                  continue;
                }
                auto elem_v = elem_res.value();
                ReadSingleJsonTagElement(elem_v, payload_writer, tag_val);
              }
            } else {
              ThrowIfSimdjsonSyntaxError(arr_res.error());
            }
          } else {
            ReadSingleJsonTagElement(tv, payload_writer, tag_val);
          }

          if (tag_val.kind == JsonTagValue::Kind::kNone) {
            continue;
          }

          if (tk.size() > 9 && tk.substr(0, 9) == "resource.") {
            std::string_view sub_key = tk.substr(9);
            if ((sub_key == "service.name" || sub_key == "service_name") &&
                tag_val.kind == JsonTagValue::Kind::kString && !tag_val.str_val.empty()) {
              append_to_arena(tag_val.str_val, tag_res_svc_off, tag_res_svc_len);
              has_tag_res_svc = true;
            }
            push_deferred_tag(DeferredJsonTag::Target::kResource, sub_key, tag_val);
          } else if (tk.size() > 6 && tk.substr(0, 6) == "scope.") {
            std::string_view sub_key = tk.substr(6);
            if (sub_key == "name") {
              if (tag_val.kind == JsonTagValue::Kind::kString) {
                append_to_arena(tag_val.str_val, tag_scope_name_off, tag_scope_name_len);
                has_tag_scope_name = true;
              }
            } else if (sub_key == "version") {
              if (tag_val.kind == JsonTagValue::Kind::kString) {
                append_to_arena(tag_val.str_val, tag_scope_ver_off, tag_scope_ver_len);
                has_tag_scope_ver = true;
              }
            } else {
              push_deferred_tag(DeferredJsonTag::Target::kScope, sub_key, tag_val);
            }
          } else if (tk == "otel.unit" || tk == "metric.unit") {
            if (tag_val.kind == JsonTagValue::Kind::kString) {
              append_to_arena(tag_val.str_val, tag_unit_off, tag_unit_len);
              has_tag_unit = true;
            }
          } else if (tk == "otel.description" || tk == "metric.description") {
            if (tag_val.kind == JsonTagValue::Kind::kString) {
              append_to_arena(tag_val.str_val, tag_desc_off, tag_desc_len);
              has_tag_desc = true;
            }
          } else if (tk == "otel.scope.name") {
            if (tag_val.kind == JsonTagValue::Kind::kString) {
              append_to_arena(tag_val.str_val, tag_otel_scope_name_off, tag_otel_scope_name_len);
              has_tag_otel_scope_name = true;
            }
          } else if (tk == "otel.scope.version") {
            if (tag_val.kind == JsonTagValue::Kind::kString) {
              append_to_arena(tag_val.str_val, tag_otel_scope_ver_off, tag_otel_scope_ver_len);
              has_tag_otel_scope_ver = true;
            }
          } else {
            push_deferred_tag(DeferredJsonTag::Target::kDatapoint, tk, tag_val);
            if ((tk == "service.name" || tk == "service_name") &&
                tag_val.kind == JsonTagValue::Kind::kString && !tag_val.str_val.empty()) {
              append_to_arena(tag_val.str_val, tag_dp_svc_off, tag_dp_svc_len);
              has_tag_dp_svc = true;
            }
          }
        }
      } else {
        ThrowIfSimdjsonSyntaxError(tags_obj_res.error());
      }
    } else if (k == "resources" || k == "resource_attributes" || k == "resourceAttributes" ||
               k == "resource") {
      has_recognized_field = true;
      res_attrs_writer.Reset();
      res_attrs_writer.StartObject();
      has_res_attrs = true;
      service_name = "unknown_service";
      std::string_view extracted_svc = "unknown_service";
      core::WriteAttributesEntriesJson(res_attrs_writer, fv, &extracted_svc);
      if (extracted_svc != "unknown_service" && !extracted_svc.empty()) {
        tls_service_scratch.assign(extracted_svc);
        service_name = std::string_view(tls_service_scratch);
      }
    } else if (k == "scope") {
      has_recognized_field = true;
      auto s_obj_res = fv.get_object();
      if (s_obj_res.error() == simdjson::SUCCESS) {
        for (auto sf_res : s_obj_res.value()) {
          if (sf_res.error() != simdjson::SUCCESS) {
            ThrowIfSimdjsonSyntaxError(sf_res.error());
            continue;
          }
          auto sk_res = sf_res.unescaped_key();
          if (sk_res.error() != simdjson::SUCCESS) {
            ThrowIfSimdjsonSyntaxError(sk_res.error());
            continue;
          }
          std::string_view sk = sk_res.value();
          auto sfv_res = sf_res.value();
          if (sfv_res.error() != simdjson::SUCCESS) {
            ThrowIfSimdjsonSyntaxError(sfv_res.error());
            continue;
          }
          auto sfv = sfv_res.value();
          if (sk == "name") {
            auto s_res = sfv.get_string();
            if (s_res.error() == simdjson::SUCCESS) {
              tls_scope_name_scratch.assign(s_res.value());
              scope_name = std::string_view(tls_scope_name_scratch);
            } else {
              ThrowIfSimdjsonSyntaxError(s_res.error());
            }
          } else if (sk == "version") {
            auto s_res = sfv.get_string();
            if (s_res.error() == simdjson::SUCCESS) {
              tls_scope_ver_scratch.assign(s_res.value());
              scope_version = std::string_view(tls_scope_ver_scratch);
            } else {
              ThrowIfSimdjsonSyntaxError(s_res.error());
            }
          } else if (sk == "attributes") {
            scope_attrs_writer.Reset();
            scope_attrs_writer.StartObject();
            has_scope_attrs = true;
            core::WriteAttributesEntriesJson(scope_attrs_writer, sfv);
          }
        }
      } else {
        ThrowIfSimdjsonSyntaxError(s_obj_res.error());
      }
    } else if (k == "attributes") {
      has_recognized_field = true;
      dp_attrs_writer.Reset();
      dp_attrs_writer.StartObject();
      has_dp_attrs = true;
      core::WriteAttributesEntriesJson(dp_attrs_writer, fv);
    } else if (k == "service_name" || k == "service") {
      has_recognized_field = true;
      auto s_res = fv.get_string();
      if (s_res.error() == simdjson::SUCCESS) {
        top_service_name = s_res.value();
      } else {
        ThrowIfSimdjsonSyntaxError(s_res.error());
      }
    }
  }

  if (!has_any_field) {
    return true;
  }
  if (metric_type.empty()) {
    if (depth == 0) {
      dropped_summary.other++;
    }
    return true;
  }
  if (out_matched != nullptr) {
    *out_matched = true;
  }

  if (has_seen_tags) {
    for (const auto& dt : tls_deferred_tags) {
      std::string_view key(tls_tag_arena.data() + dt.key_offset, dt.key_len);
      JsonTagValue tv{};
      tv.kind = dt.kind;
      if (dt.kind == JsonTagValue::Kind::kString || dt.kind == JsonTagValue::Kind::kRawJson) {
        tv.str_val = std::string_view(tls_tag_arena.data() + dt.str_offset, dt.str_len);
      } else if (dt.kind == JsonTagValue::Kind::kInt64) {
        tv.int_val = dt.int_val;
      } else if (dt.kind == JsonTagValue::Kind::kUint64) {
        tv.uint_val = dt.uint_val;
      } else if (dt.kind == JsonTagValue::Kind::kDouble) {
        tv.dbl_val = dt.dbl_val;
      } else if (dt.kind == JsonTagValue::Kind::kBool) {
        tv.bool_val = dt.bool_val;
      }
      if (dt.target == DeferredJsonTag::Target::kResource) {
        ensure_res_attrs_open();
        WriteJsonTagValueToWriter(res_attrs_writer, key, tv);
      } else if (dt.target == DeferredJsonTag::Target::kScope) {
        ensure_scope_attrs_open();
        WriteJsonTagValueToWriter(scope_attrs_writer, key, tv);
      } else {
        ensure_dp_attrs_open();
        WriteJsonTagValueToWriter(dp_attrs_writer, key, tv);
      }
    }
    if (has_tag_res_svc) {
      tls_service_scratch.assign(tls_tag_arena.data() + tag_res_svc_off, tag_res_svc_len);
      service_name = std::string_view(tls_service_scratch);
    } else if ((service_name.empty() || service_name == "unknown_service") && has_tag_dp_svc) {
      tls_service_scratch.assign(tls_tag_arena.data() + tag_dp_svc_off, tag_dp_svc_len);
      service_name = std::string_view(tls_service_scratch);
    }
    if (has_tag_scope_name) {
      tls_scope_name_scratch.assign(tls_tag_arena.data() + tag_scope_name_off, tag_scope_name_len);
      scope_name = std::string_view(tls_scope_name_scratch);
    } else if (scope_name.empty() && has_tag_otel_scope_name) {
      tls_scope_name_scratch.assign(tls_tag_arena.data() + tag_otel_scope_name_off,
                                    tag_otel_scope_name_len);
      scope_name = std::string_view(tls_scope_name_scratch);
    }
    if (has_tag_scope_ver) {
      tls_scope_ver_scratch.assign(tls_tag_arena.data() + tag_scope_ver_off, tag_scope_ver_len);
      scope_version = std::string_view(tls_scope_ver_scratch);
    } else if (scope_version.empty() && has_tag_otel_scope_ver) {
      tls_scope_ver_scratch.assign(tls_tag_arena.data() + tag_otel_scope_ver_off,
                                   tag_otel_scope_ver_len);
      scope_version = std::string_view(tls_scope_ver_scratch);
    }
    if (metric_unit.empty() && has_tag_unit) {
      tls_metric_unit_scratch.assign(tls_tag_arena.data() + tag_unit_off, tag_unit_len);
      metric_unit = std::string_view(tls_metric_unit_scratch);
    }
    if (metric_desc.empty() && has_tag_desc) {
      tls_metric_desc_scratch.assign(tls_tag_arena.data() + tag_desc_off, tag_desc_len);
      metric_desc = std::string_view(tls_metric_desc_scratch);
    }
  }

  if (has_res_attrs) {
    res_attrs_writer.EndObject();
  }
  if (has_scope_attrs) {
    scope_attrs_writer.EndObject();
  }
  if (has_dp_attrs) {
    dp_attrs_writer.EndObject();
  }
  if (!has_exemplars) {
    exemplars_writer.Reset();
    exemplars_writer.StartArray();
    exemplars_writer.EndArray();
  }

  std::string_view metric_name = raw_name;
  if (!metric_ns.empty() && !raw_name.empty()) {
    tls_metric_name_scratch.clear();
    tls_metric_name_scratch.append(metric_ns);
    tls_metric_name_scratch.push_back('.');
    tls_metric_name_scratch.append(raw_name);
    metric_name = std::string_view(tls_metric_name_scratch);
  } else if (!metric_ns.empty()) {
    metric_name = metric_ns;
  }

  if (service_name.empty() || service_name == "unknown_service") {
    if (!top_service_name.empty()) {
      service_name = top_service_name;
    } else {
      service_name = "unknown_service";
    }
  }

  std::array<char, 32> time_ts_buf;
  std::string_view time_unix_sv = processed_at;
  if (has_ts) {
    core::FormatIso8601Nano(t_ns, time_ts_buf.data());
    time_unix_sv = std::string_view(time_ts_buf.data(), 30);
  }

  std::array<char, 32> start_ts_buf;
  std::string_view start_time_sv;
  if (has_start_ts) {
    core::FormatIso8601Nano(start_ns, start_ts_buf.data());
    start_time_sv = std::string_view(start_ts_buf.data(), 30);
  } else if (has_ts && has_interval_ms && interval_ms > 0) {
    const uint64_t interval_ns = interval_ms * 1000000ULL;
    const uint64_t computed_start = (t_ns >= interval_ns) ? (t_ns - interval_ns) : 0ULL;
    core::FormatIso8601Nano(computed_start, start_ts_buf.data());
    start_time_sv = std::string_view(start_ts_buf.data(), 30);
  }

  const bool is_sum = (metric_type == "sum");
  const bool is_histogram = (metric_type == "histogram");
  const bool is_exp_histogram = (metric_type == "exponential_histogram");
  const bool is_summary = (metric_type == "summary");

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

  emitter.SetString("ResourceAttributes", has_res_attrs ? res_attrs_writer.Str() : "{}");
  emitter.SetString("ScopeName", scope_name);
  emitter.SetString("ScopeVersion", scope_version);
  emitter.SetString("ScopeAttributes", has_scope_attrs ? scope_attrs_writer.Str() : "{}");
  emitter.SetString("Attributes", has_dp_attrs ? dp_attrs_writer.Str() : "{}");

  emitter.SetString("StartTimeUnix", start_time_sv);
  emitter.SetString("TimeUnix", time_unix_sv);

  if (!is_histogram && !is_exp_histogram && !is_summary) {
    if (val_kind == MetricValueKind::kDouble) {
      emitter.SetDouble("Value", double_val);
    } else if (val_kind == MetricValueKind::kInt64) {
      emitter.SetInt64("Value", int_val);
    }
  }

  if (is_histogram) {
    SerializeHistogramJson(payload_writer, agg_temporality, hist_count, has_sum, hist_sum, has_min,
                           hist_min, has_max, hist_max, tls_bucket_counts, tls_explicit_bounds);
    emitter.SetString("Histogram", payload_writer.Str());
  }

  if (is_exp_histogram) {
    SerializeExponentialHistogramJson(payload_writer, agg_temporality, hist_count, has_sum,
                                      hist_sum, has_min, hist_min, has_max, hist_max, exp_scale,
                                      exp_zero_count, exp_zero_threshold, pos_offset,
                                      tls_pos_bucket_counts, neg_offset, tls_neg_bucket_counts);
    emitter.SetString("ExponentialHistogram", payload_writer.Str());
  }

  if (is_summary) {
    SerializeSummaryJson(payload_writer, hist_count, has_sum, hist_sum, tls_summary_quantiles);
    emitter.SetString("Summary", payload_writer.Str());
  }

  emitter.SetInt64("Flags", flags);
  emitter.SetString("Exemplars", exemplars_writer.Str());
  emitter.SetString("processed_at", processed_at);

  emitter.EndRecord();
  if (emitter.HasError()) {
    err_out = std::string(emitter.ErrorMessage());
    return false;
  }
  return true;
}

bool HasNativeMetricFields(lua_State* L, int table_idx) {
  int abs_idx = lua_absindex(L, table_idx);
  if (!lua_istable(L, abs_idx) || core::IsLuaTableEmpty(L, abs_idx)) {
    return false;
  }
  static constexpr core::ReadKey kKeys[] = {
      "name",
      "namespace",
      "counter",
      "sum",
      "gauge",
      "aggregated_histogram",
      "histogram",
      "exponential_histogram",
      "exponentialHistogram",
      "aggregated_exponential_histogram",
      "aggregated_summary",
      "summary",
      "distribution",
      "set",
      "sketch",
      "timestamp",
      "timeUnixNano",
      "time_unix_nano",
      "tags",
      "kind",
  };
  for (const core::ReadKey& k : kKeys) {
    if (core::GetTableFieldRaw(L, abs_idx, k)) {
      lua_pop(L, 1);
      return true;
    }
  }
  return false;
}

// Text of a scalar (string or number) tag value without converting the Lua value in place.
std::string_view ReadLuaScalarStringView(lua_State* L, int val_idx, std::string& scratch) {
  char buffer[core::kScalarTextBuffer];
  std::string_view text;
  if (!core::ScalarText(L, val_idx, buffer, text)) {
    return "";
  }
  scratch.assign(text);
  return std::string_view(scratch);
}

void ReadLuaBucketCountsArray(lua_State* L, int arr_idx, std::vector<uint64_t>& out) {
  out.clear();
  int abs_idx = lua_absindex(L, arr_idx);
  if (!lua_istable(L, abs_idx)) {
    return;
  }
  const size_t c_len = lua_rawlen(L, abs_idx);
  for (size_t i = 1; i <= c_len; ++i) {
    lua_rawgeti(L, abs_idx, static_cast<lua_Integer>(i));
    uint64_t bc = 0;
    if (!core::TryParseTableUint64At(L, -1, bc)) {
      bc = 0;
    }
    out.push_back(bc);
    lua_pop(L, 1);
  }
}

void ReadLuaMetricValue(lua_State* L, int type_obj_idx, MetricValueKind& val_kind,
                        double& double_val, int64_t& int_val, int64_t& agg_temporality,
                        core::FastJsonWriter& exemplars_writer, bool& has_exemplars) {
  int abs_idx = lua_absindex(L, type_obj_idx);
  auto parse_val_at = [&](int v_idx) {
    int abs_v = lua_absindex(L, v_idx);
    int vt = lua_type(L, abs_v);
    if (vt == LUA_TNUMBER) {
      if (lua_isinteger(L, abs_v)) {
        int_val = lua_tointeger(L, abs_v);
        val_kind = MetricValueKind::kInt64;
      } else {
        double d = lua_tonumber(L, abs_v);
        if (std::isfinite(d)) {
          double_val = d;
          val_kind = MetricValueKind::kDouble;
        }
      }
    } else if (vt == LUA_TSTRING) {
      size_t len = 0;
      const char* s = lua_tolstring(L, abs_v, &len);
      if (s != nullptr) {
        std::string_view sv = core::TrimJsonWhitespace(std::string_view(s, len));
        if (!sv.empty()) {
          int64_t iv = 0;
          auto [iptr, iec] = std::from_chars(sv.data(), sv.data() + sv.size(), iv);
          if (iec == std::errc() && iptr == sv.data() + sv.size()) {
            int_val = iv;
            val_kind = MetricValueKind::kInt64;
            return;
          }
          double dv = 0.0;
          auto [dptr, dec] = std::from_chars(sv.data(), sv.data() + sv.size(), dv);
          if (dec == std::errc() && dptr == sv.data() + sv.size() && std::isfinite(dv)) {
            double_val = dv;
            val_kind = MetricValueKind::kDouble;
          }
        }
      }
    }
  };

  if (lua_istable(L, abs_idx)) {
    ReadLuaAggregationTemporality(L, abs_idx, agg_temporality);
    if (core::GetTableFieldRaw(L, abs_idx, "value", "asDouble", "as_double")) {
      parse_val_at(-1);
      lua_pop(L, 1);
    } else if (core::GetTableFieldRaw(L, abs_idx, "asInt", "as_int")) {
      parse_val_at(-1);
      lua_pop(L, 1);
    }
    if (core::GetTableFieldRaw(L, abs_idx, "exemplars", "Exemplars")) {
      exemplars_writer.Reset();
      core::SerializeTableExemplarsToJson(L, -1, exemplars_writer);
      has_exemplars = true;
      lua_pop(L, 1);
    }
  } else {
    parse_val_at(abs_idx);
  }
}

void ReadLuaHistogram(lua_State* L, int hist_idx, int64_t& agg_temporality, uint64_t& hist_count,
                      bool& has_sum, double& hist_sum, bool& has_min, double& hist_min,
                      bool& has_max, double& hist_max, std::vector<uint64_t>& bucket_counts,
                      std::vector<double>& explicit_bounds, core::FastJsonWriter& exemplars_writer,
                      bool& has_exemplars) {
  int abs_idx = lua_absindex(L, hist_idx);
  bucket_counts.clear();
  explicit_bounds.clear();
  if (!lua_istable(L, abs_idx)) {
    return;
  }

  ReadLuaAggregationTemporality(L, abs_idx, agg_temporality);
  hist_count = core::ReadTableUint64(L, abs_idx, "count", "Count", 0);
  has_sum = core::TryReadTableDouble(L, abs_idx, "sum", "Sum", hist_sum);

  if (core::GetTableFieldRaw(L, abs_idx, "min", "Min")) {
    double d = 0.0;
    if (TryParseLuaFiniteBound(L, -1, d)) {
      hist_min = d;
      has_min = true;
    }
    lua_pop(L, 1);
  }
  if (core::GetTableFieldRaw(L, abs_idx, "max", "Max")) {
    double d = 0.0;
    if (TryParseLuaFiniteBound(L, -1, d)) {
      hist_max = d;
      has_max = true;
    }
    lua_pop(L, 1);
  }

  bool is_obj_buckets = false;
  if (core::GetTableFieldRaw(L, abs_idx, "buckets")) {
    int b_arr_idx = lua_absindex(L, -1);
    if (lua_istable(L, b_arr_idx)) {
      const size_t b_len = lua_rawlen(L, b_arr_idx);
      if (b_len > 0) {
        lua_rawgeti(L, b_arr_idx, 1);
        is_obj_buckets = lua_istable(L, -1);
        lua_pop(L, 1);

        if (is_obj_buckets) {
          for (size_t i = 1; i <= b_len; ++i) {
            lua_rawgeti(L, b_arr_idx, static_cast<lua_Integer>(i));
            if (lua_istable(L, -1)) {
              int b_obj_idx = lua_absindex(L, -1);
              uint64_t bc = core::ReadTableUint64(L, b_obj_idx, "count", "Count", 0);
              bucket_counts.push_back(bc);
              if (core::GetTableFieldRaw(L, b_obj_idx, "upper_limit", "upperLimit")) {
                double bound = 0.0;
                if (TryParseLuaFiniteBound(L, -1, bound)) {
                  explicit_bounds.push_back(bound);
                }
                lua_pop(L, 1);
              }
            }
            lua_pop(L, 1);
          }
        } else {
          for (size_t i = 1; i <= b_len; ++i) {
            lua_rawgeti(L, b_arr_idx, static_cast<lua_Integer>(i));
            double bound = 0.0;
            if (TryParseLuaFiniteBound(L, -1, bound)) {
              explicit_bounds.push_back(bound);
            }
            lua_pop(L, 1);
          }
        }
      }
    }
    lua_pop(L, 1);
  } else if (core::GetTableFieldRaw(L, abs_idx, "explicitBounds", "explicit_bounds",
                                    "ExplicitBounds")) {
    int eb_arr_idx = lua_absindex(L, -1);
    if (lua_istable(L, eb_arr_idx)) {
      const size_t eb_len = lua_rawlen(L, eb_arr_idx);
      for (size_t i = 1; i <= eb_len; ++i) {
        lua_rawgeti(L, eb_arr_idx, static_cast<lua_Integer>(i));
        double bound = 0.0;
        if (TryParseLuaFiniteBound(L, -1, bound)) {
          explicit_bounds.push_back(bound);
        }
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }

  if (!is_obj_buckets) {
    bool has_counts = core::GetTableFieldRaw(L, abs_idx, "counts", "bucketCounts", "bucket_counts");
    if (!has_counts) {
      has_counts = core::GetTableFieldRaw(L, abs_idx, "BucketCounts");
    }
    if (has_counts) {
      ReadLuaBucketCountsArray(L, -1, bucket_counts);
      lua_pop(L, 1);
    }
  }

  if (core::GetTableFieldRaw(L, abs_idx, "exemplars", "Exemplars")) {
    exemplars_writer.Reset();
    core::SerializeTableExemplarsToJson(L, -1, exemplars_writer);
    has_exemplars = true;
    lua_pop(L, 1);
  }
}

void ReadLuaExpBucketsSubTable(lua_State* L, int parent_idx, core::ReadKey k1, core::ReadKey k2,
                               int64_t& out_offset, std::vector<uint64_t>& out_buckets) {
  out_offset = 0;
  out_buckets.clear();
  if (!core::GetTableFieldRaw(L, parent_idx, k1, k2)) {
    return;
  }
  if (lua_istable(L, -1)) {
    int b_obj_idx = lua_absindex(L, -1);
    out_offset = core::ReadTableInt64(L, b_obj_idx, "offset", "Offset", 0);
    bool has_bc = core::GetTableFieldRaw(L, b_obj_idx, "counts", "bucketCounts", "bucket_counts");
    if (!has_bc) {
      has_bc = core::GetTableFieldRaw(L, b_obj_idx, "BucketCounts");
    }
    if (has_bc) {
      ReadLuaBucketCountsArray(L, -1, out_buckets);
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);
}

void ReadLuaExponentialHistogram(lua_State* L, int eh_idx, int64_t& agg_temporality,
                                 uint64_t& exp_count, bool& has_sum, double& exp_sum, bool& has_min,
                                 double& exp_min, bool& has_max, double& exp_max,
                                 int64_t& exp_scale, uint64_t& exp_zero_count,
                                 double& exp_zero_threshold, int64_t& pos_offset,
                                 std::vector<uint64_t>& pos_buckets, int64_t& neg_offset,
                                 std::vector<uint64_t>& neg_buckets,
                                 core::FastJsonWriter& exemplars_writer, bool& has_exemplars) {
  int abs_idx = lua_absindex(L, eh_idx);
  pos_offset = 0;
  neg_offset = 0;
  pos_buckets.clear();
  neg_buckets.clear();
  if (!lua_istable(L, abs_idx)) {
    return;
  }

  ReadLuaAggregationTemporality(L, abs_idx, agg_temporality);
  exp_count = core::ReadTableUint64(L, abs_idx, "count", "Count", 0);
  has_sum = core::TryReadTableDouble(L, abs_idx, "sum", "Sum", exp_sum);

  if (core::GetTableFieldRaw(L, abs_idx, "min", "Min")) {
    double d = 0.0;
    if (TryParseLuaFiniteBound(L, -1, d)) {
      exp_min = d;
      has_min = true;
    }
    lua_pop(L, 1);
  }
  if (core::GetTableFieldRaw(L, abs_idx, "max", "Max")) {
    double d = 0.0;
    if (TryParseLuaFiniteBound(L, -1, d)) {
      exp_max = d;
      has_max = true;
    }
    lua_pop(L, 1);
  }

  exp_scale = core::ReadTableInt64(L, abs_idx, "scale", "Scale", 0);
  exp_zero_count = core::ReadTableUint64(L, abs_idx, "zeroCount", "zero_count", 0, "ZeroCount");
  exp_zero_threshold =
      core::ReadTableDouble(L, abs_idx, "zeroThreshold", "zero_threshold", 0.0, "ZeroThreshold");

  ReadLuaExpBucketsSubTable(L, abs_idx, "positive", "Positive", pos_offset, pos_buckets);
  ReadLuaExpBucketsSubTable(L, abs_idx, "negative", "Negative", neg_offset, neg_buckets);

  if (core::GetTableFieldRaw(L, abs_idx, "exemplars", "Exemplars")) {
    exemplars_writer.Reset();
    core::SerializeTableExemplarsToJson(L, -1, exemplars_writer);
    has_exemplars = true;
    lua_pop(L, 1);
  }
}

void ReadLuaSummary(lua_State* L, int sum_idx, uint64_t& sum_count, bool& has_sum,
                    double& sum_total, std::vector<SummaryQuantile>& quantiles,
                    core::FastJsonWriter& exemplars_writer, bool& has_exemplars) {
  int abs_idx = lua_absindex(L, sum_idx);
  quantiles.clear();
  if (!lua_istable(L, abs_idx)) {
    return;
  }

  sum_count = core::ReadTableUint64(L, abs_idx, "count", "Count", 0);
  has_sum = core::TryReadTableDouble(L, abs_idx, "sum", "Sum", sum_total);

  bool is_obj_quantiles = false;
  bool has_q = core::GetTableFieldRaw(L, abs_idx, "quantiles", "quantileValues");
  if (!has_q) {
    has_q = core::GetTableFieldRaw(L, abs_idx, "quantile_values", "QuantileValues");
  }
  if (has_q) {
    int q_arr_idx = lua_absindex(L, -1);
    if (lua_istable(L, q_arr_idx)) {
      const size_t q_len = lua_rawlen(L, q_arr_idx);
      if (q_len > 0) {
        lua_rawgeti(L, q_arr_idx, 1);
        is_obj_quantiles = lua_istable(L, -1);
        lua_pop(L, 1);

        if (is_obj_quantiles) {
          for (size_t i = 1; i <= q_len; ++i) {
            lua_rawgeti(L, q_arr_idx, static_cast<lua_Integer>(i));
            if (lua_istable(L, -1)) {
              int q_obj_idx = lua_absindex(L, -1);
              SummaryQuantile sq{};
              sq.quantile = core::ReadTableDouble(L, q_obj_idx, "quantile", "Quantile", 0.0);
              sq.value = core::ReadTableDouble(L, q_obj_idx, "value", "Value", 0.0);
              quantiles.push_back(sq);
            }
            lua_pop(L, 1);
          }
        } else {
          for (size_t i = 1; i <= q_len; ++i) {
            lua_rawgeti(L, q_arr_idx, static_cast<lua_Integer>(i));
            SummaryQuantile sq{};
            (void)core::TryParseTableDoubleAt(L, -1, sq.quantile);
            quantiles.push_back(sq);
            lua_pop(L, 1);
          }
        }
      }
    }
    lua_pop(L, 1);
  }

  if (!is_obj_quantiles && !quantiles.empty()) {
    if (core::GetTableFieldRaw(L, abs_idx, "values", "Values")) {
      int v_arr_idx = lua_absindex(L, -1);
      if (lua_istable(L, v_arr_idx)) {
        const size_t v_len = lua_rawlen(L, v_arr_idx);
        const size_t n = std::min(quantiles.size(), v_len);
        quantiles.resize(n);
        for (size_t i = 1; i <= n; ++i) {
          lua_rawgeti(L, v_arr_idx, static_cast<lua_Integer>(i));
          double v_val = 0.0;
          if (core::TryParseTableDoubleAt(L, -1, v_val)) {
            quantiles[i - 1].value = v_val;
          }
          lua_pop(L, 1);
        }
      }
      lua_pop(L, 1);
    }
  }

  if (core::GetTableFieldRaw(L, abs_idx, "exemplars", "Exemplars")) {
    exemplars_writer.Reset();
    core::SerializeTableExemplarsToJson(L, -1, exemplars_writer);
    has_exemplars = true;
    lua_pop(L, 1);
  }
}

bool ExtractSingleNativeMetricTable(
    lua_State* L, int rec_idx, std::string_view processed_at,
    core::FastJsonWriter& res_attrs_writer, core::FastJsonWriter& scope_attrs_writer,
    core::FastJsonWriter& dp_attrs_writer, core::FastJsonWriter& exemplars_writer,
    core::FastJsonWriter& payload_writer, std::string& tls_metric_name_scratch,
    std::string& tls_service_scratch, std::string& tls_scope_name_scratch,
    std::string& tls_scope_ver_scratch, std::string& tls_metric_desc_scratch,
    std::string& tls_metric_unit_scratch, std::vector<uint64_t>& tls_bucket_counts,
    std::vector<double>& tls_explicit_bounds, std::vector<uint64_t>& tls_pos_bucket_counts,
    std::vector<uint64_t>& tls_neg_bucket_counts,
    std::vector<SummaryQuantile>& tls_summary_quantiles, core::RowEmitter& emitter,
    std::string& err_out, DroppedMetricsSummary& dropped_summary) {
  if (!lua_checkstack(L, 16)) {
    err_out = "Lua stack overflow during native metric extraction";
    return false;
  }
  int abs_rec_idx = lua_absindex(L, rec_idx);

  // 1. Check for unsupported non-OTLP metric variants first
  if (core::GetTableFieldRaw(L, abs_rec_idx, "distribution")) {
    lua_pop(L, 1);
    dropped_summary.distribution++;
    return true;
  }
  if (core::GetTableFieldRaw(L, abs_rec_idx, "set")) {
    lua_pop(L, 1);
    dropped_summary.set++;
    return true;
  }
  if (core::GetTableFieldRaw(L, abs_rec_idx, "sketch")) {
    lua_pop(L, 1);
    dropped_summary.sketch++;
    return true;
  }

  // 2. Top-level temporality & supported OTLP metric variant
  int64_t agg_temporality = 0;
  ReadLuaAggregationTemporality(L, abs_rec_idx, agg_temporality);

  std::string_view metric_type;
  MetricValueKind val_kind = MetricValueKind::kNone;
  double double_val = 0.0;
  int64_t int_val = 0;
  uint64_t hist_count = 0;
  bool has_sum = false;
  double hist_sum = 0.0;
  bool has_min = false;
  double hist_min = 0.0;
  bool has_max = false;
  double hist_max = 0.0;
  int64_t exp_scale = 0;
  uint64_t exp_zero_count = 0;
  double exp_zero_threshold = 0.0;
  int64_t pos_offset = 0;
  int64_t neg_offset = 0;
  bool has_exemplars = false;

  if (core::GetTableFieldRaw(L, abs_rec_idx, "counter", "sum")) {
    metric_type = "sum";
    ReadLuaMetricValue(L, -1, val_kind, double_val, int_val, agg_temporality, exemplars_writer,
                       has_exemplars);
    lua_pop(L, 1);
  } else if (core::GetTableFieldRaw(L, abs_rec_idx, "gauge")) {
    metric_type = "gauge";
    ReadLuaMetricValue(L, -1, val_kind, double_val, int_val, agg_temporality, exemplars_writer,
                       has_exemplars);
    lua_pop(L, 1);
  } else if (core::GetTableFieldRaw(L, abs_rec_idx, "aggregated_histogram", "histogram")) {
    metric_type = "histogram";
    ReadLuaHistogram(L, -1, agg_temporality, hist_count, has_sum, hist_sum, has_min, hist_min,
                     has_max, hist_max, tls_bucket_counts, tls_explicit_bounds, exemplars_writer,
                     has_exemplars);
    lua_pop(L, 1);
  } else if (core::GetTableFieldRaw(L, abs_rec_idx, "exponential_histogram", "exponentialHistogram",
                                    "aggregated_exponential_histogram")) {
    metric_type = "exponential_histogram";
    ReadLuaExponentialHistogram(L, -1, agg_temporality, hist_count, has_sum, hist_sum, has_min,
                                hist_min, has_max, hist_max, exp_scale, exp_zero_count,
                                exp_zero_threshold, pos_offset, tls_pos_bucket_counts, neg_offset,
                                tls_neg_bucket_counts, exemplars_writer, has_exemplars);
    lua_pop(L, 1);
  } else if (core::GetTableFieldRaw(L, abs_rec_idx, "aggregated_summary", "summary")) {
    metric_type = "summary";
    ReadLuaSummary(L, -1, hist_count, has_sum, hist_sum, tls_summary_quantiles, exemplars_writer,
                   has_exemplars);
    lua_pop(L, 1);
  }

  if (metric_type.empty()) {
    dropped_summary.other++;
    return true;
  }

  if (core::GetTableFieldRaw(L, abs_rec_idx, "exemplars", "Exemplars")) {
    exemplars_writer.Reset();
    core::SerializeTableExemplarsToJson(L, -1, exemplars_writer);
    has_exemplars = true;
    lua_pop(L, 1);
  }
  if (!has_exemplars) {
    exemplars_writer.Reset();
    exemplars_writer.StartArray();
    exemplars_writer.EndArray();
  }

  bool has_is_monotonic = false;
  bool is_monotonic = false;
  if (core::GetTableFieldRaw(L, abs_rec_idx, "isMonotonic", "is_monotonic")) {
    if (lua_isboolean(L, -1)) {
      is_monotonic = lua_toboolean(L, -1) != 0;
      has_is_monotonic = true;
    }
    lua_pop(L, 1);
  }

  // 3. MetricName (namespace .. "." .. name), MetricDescription, MetricUnit
  std::string_view raw_name =
      core::ReadTableStringView(L, abs_rec_idx, "name", nullptr, tls_service_scratch);
  std::string_view metric_ns =
      core::ReadTableStringView(L, abs_rec_idx, "namespace", nullptr, tls_metric_name_scratch);
  std::string_view metric_name = raw_name;
  if (!metric_ns.empty() && !raw_name.empty()) {
    if (metric_ns.data() != tls_metric_name_scratch.data()) {
      tls_metric_name_scratch.clear();
      tls_metric_name_scratch.append(metric_ns);
    }
    tls_metric_name_scratch.push_back('.');
    tls_metric_name_scratch.append(raw_name);
    metric_name = std::string_view(tls_metric_name_scratch);
  } else if (!metric_ns.empty()) {
    metric_name = metric_ns;
  } else if (raw_name.data() == tls_service_scratch.data() && !raw_name.empty()) {
    tls_metric_name_scratch.assign(raw_name);
    metric_name = std::string_view(tls_metric_name_scratch);
  }

  std::string_view metric_desc =
      core::ReadTableStringView(L, abs_rec_idx, "description", nullptr, tls_metric_desc_scratch);
  std::string_view metric_unit =
      core::ReadTableStringView(L, abs_rec_idx, "unit", nullptr, tls_metric_unit_scratch);

  // 4. Timestamps: TimeUnix and StartTimeUnix
  uint64_t t_ns = 0;
  bool has_ts = core::ReadTableTimestampField(L, abs_rec_idx, "timestamp", "timeUnixNano", t_ns);
  if (!has_ts) {
    has_ts = core::ReadTableTimestampField(L, abs_rec_idx, "time_unix_nano", nullptr, t_ns);
  }

  uint64_t start_ns = 0;
  bool has_start_ts = core::ReadTableTimestampField(L, abs_rec_idx, "start_time_unix_nano",
                                                    "startTimeUnixNano", start_ns);
  if (!has_start_ts) {
    has_start_ts =
        core::ReadTableTimestampField(L, abs_rec_idx, "start_timestamp", nullptr, start_ns);
  }

  uint64_t interval_ms = core::ReadTableUint64(L, abs_rec_idx, "interval_ms", nullptr, 0);

  std::array<char, 32> time_ts_buf;
  std::string_view time_unix_sv = processed_at;
  if (has_ts) {
    core::FormatIso8601Nano(t_ns, time_ts_buf.data());
    time_unix_sv = std::string_view(time_ts_buf.data(), 30);
  }

  std::array<char, 32> start_ts_buf;
  std::string_view start_time_sv;
  if (has_start_ts) {
    core::FormatIso8601Nano(start_ns, start_ts_buf.data());
    start_time_sv = std::string_view(start_ts_buf.data(), 30);
  } else if (has_ts && interval_ms > 0) {
    const uint64_t interval_ns = interval_ms * 1000000ULL;
    const uint64_t computed_start = (t_ns >= interval_ns) ? (t_ns - interval_ns) : 0ULL;
    core::FormatIso8601Nano(computed_start, start_ts_buf.data());
    start_time_sv = std::string_view(start_ts_buf.data(), 30);
  }

  // 5. Optional resources / scope / attributes tables + single-pass tags merging
  std::string_view service_name;
  std::string_view scope_name =
      core::ReadTableStringView(L, abs_rec_idx, "scope_name", "scopeName", tls_scope_name_scratch);
  std::string_view scope_version = core::ReadTableStringView(L, abs_rec_idx, "scope_version",
                                                             "scopeVersion", tls_scope_ver_scratch);

  res_attrs_writer.Reset();
  res_attrs_writer.StartObject();
  scope_attrs_writer.Reset();
  scope_attrs_writer.StartObject();
  dp_attrs_writer.Reset();
  dp_attrs_writer.StartObject();

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
      core::WriteTableAttributesEntriesJson(L, attrs_src_idx, res_attrs_writer, &service_name);
      if (!service_name.empty()) {
        tls_service_scratch.assign(service_name);
        service_name = std::string_view(tls_service_scratch);
      }
      if (has_nested_attrs) {
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }

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
      if (core::GetTableFieldRaw(L, s_idx, "attributes")) {
        core::WriteTableAttributesEntriesJson(L, -1, scope_attrs_writer);
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }

  if (core::GetTableFieldRaw(L, abs_rec_idx, "attributes")) {
    if (lua_istable(L, -1)) {
      core::WriteTableAttributesEntriesJson(L, -1, dp_attrs_writer);
    }
    lua_pop(L, 1);
  }

  if (core::GetTableFieldRaw(L, abs_rec_idx, "tags")) {
    int tags_idx = lua_absindex(L, -1);
    if (lua_istable(L, tags_idx) && !core::IsLuaTableEmpty(L, tags_idx)) {
      lua_pushnil(L);
      while (lua_next(L, tags_idx) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) {
          lua_pop(L, 1);
          continue;
        }
        size_t klen = 0;
        const char* ks = lua_tolstring(L, -2, &klen);
        std::string_view k(ks, klen);

        int val_idx = lua_absindex(L, -1);
        bool popped_unwrapped = false;
        if (lua_type(L, val_idx) == LUA_TTABLE) {
          const size_t arr_len = lua_rawlen(L, val_idx);
          if (arr_len > 0) {
            lua_rawgeti(L, val_idx, static_cast<lua_Integer>(arr_len));
            val_idx = lua_absindex(L, -1);
            popped_unwrapped = true;
          }
        }

        if (!lua_isnil(L, val_idx)) {
          if (k.size() > 9 && k.substr(0, 9) == "resource.") {
            std::string_view sub_key = k.substr(9);
            if (sub_key == "service.name" || sub_key == "service_name") {
              std::string_view sv = ReadLuaScalarStringView(L, val_idx, tls_service_scratch);
              if (!sv.empty()) {
                service_name = sv;
              }
            }
            res_attrs_writer.Key(sub_key);
            core::SerializeLuaValueToJson(L, val_idx, res_attrs_writer);
          } else if (k.size() > 6 && k.substr(0, 6) == "scope.") {
            std::string_view sub_key = k.substr(6);
            if (sub_key == "name") {
              std::string_view sv = ReadLuaScalarStringView(L, val_idx, tls_scope_name_scratch);
              if (!sv.empty()) {
                scope_name = sv;
              }
            } else if (sub_key == "version") {
              std::string_view sv = ReadLuaScalarStringView(L, val_idx, tls_scope_ver_scratch);
              if (!sv.empty()) {
                scope_version = sv;
              }
            } else {
              scope_attrs_writer.Key(sub_key);
              core::SerializeLuaValueToJson(L, val_idx, scope_attrs_writer);
            }
          } else if (k == "otel.unit" || k == "metric.unit") {
            if (metric_unit.empty()) {
              metric_unit = ReadLuaScalarStringView(L, val_idx, tls_metric_unit_scratch);
            }
          } else if (k == "otel.description" || k == "metric.description") {
            if (metric_desc.empty()) {
              metric_desc = ReadLuaScalarStringView(L, val_idx, tls_metric_desc_scratch);
            }
          } else if (k == "otel.scope.name") {
            if (scope_name.empty()) {
              scope_name = ReadLuaScalarStringView(L, val_idx, tls_scope_name_scratch);
            }
          } else if (k == "otel.scope.version") {
            if (scope_version.empty()) {
              scope_version = ReadLuaScalarStringView(L, val_idx, tls_scope_ver_scratch);
            }
          } else {
            dp_attrs_writer.Key(k);
            core::SerializeLuaValueToJson(L, val_idx, dp_attrs_writer);
            if ((k == "service.name" || k == "service_name") &&
                (service_name.empty() || service_name == "unknown_service")) {
              std::string_view sv = ReadLuaScalarStringView(L, val_idx, tls_service_scratch);
              if (!sv.empty()) {
                service_name = sv;
              }
            }
          }
        }

        if (popped_unwrapped) {
          lua_pop(L, 1);
        }
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }

  res_attrs_writer.EndObject();
  scope_attrs_writer.EndObject();
  dp_attrs_writer.EndObject();

  if (service_name.empty() || service_name == "unknown_service") {
    std::string_view top_svc =
        core::ReadTableStringView(L, abs_rec_idx, "service_name", "service", tls_service_scratch);
    if (!top_svc.empty()) {
      service_name = top_svc;
    } else {
      service_name = "unknown_service";
    }
  }

  int64_t flags = core::ReadTableInt64(L, abs_rec_idx, "flags", nullptr, 0);
  const bool is_sum = (metric_type == "sum");
  const bool is_histogram = (metric_type == "histogram");
  const bool is_exp_histogram = (metric_type == "exponential_histogram");
  const bool is_summary = (metric_type == "summary");

  // 6. Emit all columns of metrics.schema
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

  emitter.SetString("StartTimeUnix", start_time_sv);
  emitter.SetString("TimeUnix", time_unix_sv);

  if (!is_histogram && !is_exp_histogram && !is_summary) {
    if (val_kind == MetricValueKind::kDouble) {
      emitter.SetDouble("Value", double_val);
    } else if (val_kind == MetricValueKind::kInt64) {
      emitter.SetInt64("Value", int_val);
    }
  }

  if (is_histogram) {
    SerializeHistogramJson(payload_writer, agg_temporality, hist_count, has_sum, hist_sum, has_min,
                           hist_min, has_max, hist_max, tls_bucket_counts, tls_explicit_bounds);
    emitter.SetString("Histogram", payload_writer.Str());
  }

  if (is_exp_histogram) {
    SerializeExponentialHistogramJson(payload_writer, agg_temporality, hist_count, has_sum,
                                      hist_sum, has_min, hist_min, has_max, hist_max, exp_scale,
                                      exp_zero_count, exp_zero_threshold, pos_offset,
                                      tls_pos_bucket_counts, neg_offset, tls_neg_bucket_counts);
    emitter.SetString("ExponentialHistogram", payload_writer.Str());
  }

  if (is_summary) {
    SerializeSummaryJson(payload_writer, hist_count, has_sum, hist_sum, tls_summary_quantiles);
    emitter.SetString("Summary", payload_writer.Str());
  }

  emitter.SetInt64("Flags", flags);
  emitter.SetString("Exemplars", exemplars_writer.Str());
  emitter.SetString("processed_at", processed_at);

  emitter.EndRecord();
  if (emitter.HasError()) {
    err_out = std::string(emitter.ErrorMessage());
    return false;
  }
  return true;
}

// The row staged for emission (one record at a time).
core::StagedRow& RowBuffer() {
  static thread_local core::StagedRow row;
  return row;
}

struct ScratchGuard {
  ScratchGuard() = default;
  ScratchGuard(const ScratchGuard&) = delete;
  ScratchGuard& operator=(const ScratchGuard&) = delete;
  ~ScratchGuard() noexcept { TrimAllScratch(); }
};

// Records a failed extraction on the context (unless the emitter already did).
bool FailExtraction(OperationContext& ctx, const core::RowEmitter& emitter, ErrorCode code,
                    std::string_view message) {
  if (!emitter.HasError()) {
    ctx.error.Set(code, message);
  }
  return false;
}

bool FailMetric(OperationContext& ctx, const core::RowEmitter& emitter,
                const std::string& message) {
  const bool unsupported = message.rfind("Unsupported non-OTLP metric variant", 0) == 0;
  return FailExtraction(ctx, emitter, unsupported ? ErrorCode::Schema : ErrorCode::Json, message);
}

bool FailJson(OperationContext& ctx, simdjson::error_code code) {
  ctx.error.Set(ErrorCode::Json, simdjson::error_message(code));
  return false;
}

}  // namespace

bool ExtractNativeMetricsJson(OperationContext& ctx, core::OtelParser& parser,
                              simdjson::padded_string_view admitted) {
  NativeMetricsScratch& s = ScratchLevel(0);
  ResetScratch(s);
  ScratchGuard guard;
  core::RowEmitter emitter(ctx, RowBuffer());

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
  std::string err_out;
  DroppedMetricsSummary dropped_summary;

  auto extract_object = [&](simdjson::ondemand::object& obj) -> bool {
    return ExtractNativeMetricObjectJson(
        ctx.L, obj, processed_at, s.res_attrs_writer, s.scope_attrs_writer, s.dp_attrs_writer,
        s.exemplars_writer, s.payload_writer, s.metric_name, s.service, s.scope_name, s.scope_ver,
        s.metric_desc, s.metric_unit, s.bucket_counts, s.explicit_bounds, s.pos_bucket_counts,
        s.neg_bucket_counts, s.summary_quantiles, emitter, err_out, dropped_summary);
  };

  const auto root_type = type_res.value();
  if (root_type == simdjson::ondemand::json_type::object) {
    auto doc_obj_res = doc.get_object();
    if (doc_obj_res.error() != simdjson::SUCCESS) {
      return FailJson(ctx, doc_obj_res.error());
    }
    auto doc_obj = doc_obj_res.value();
    if (!extract_object(doc_obj)) {
      return FailMetric(ctx, emitter, err_out);
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
      if (!extract_object(elem_obj)) {
        return FailMetric(ctx, emitter, err_out);
      }
    }
  } else {
    ctx.error.Set(ErrorCode::Json, "root JSON value must be an object or array");
    return false;
  }

  dropped_summary.LogSummary(ResolveModuleName());

  emitter.Flush();
  return !emitter.HasError();
}

bool ExtractNativeMetricsTable(OperationContext& ctx) {
  NativeMetricsScratch& s = ScratchLevel(0);
  ResetScratch(s);
  struct Guard {
    Guard(lua_State* l, int top) : L(l), entry_top(top) {}
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    ~Guard() noexcept {
      lua_settop(L, entry_top);
      TrimAllScratch();
    }
    lua_State* L;
    int entry_top;
  } guard{ctx.L, lua_gettop(ctx.L)};

  lua_State* L = ctx.L;
  core::RowEmitter emitter(ctx, RowBuffer());
  std::array<char, 32> proc_buf;
  core::FormatIso8601Nano(core::CurrentTimeUnixNano(), proc_buf.data());
  std::string_view processed_at(proc_buf.data(), 30);
  std::string err_out;
  DroppedMetricsSummary dropped_summary;

  const int abs_idx = lua_absindex(L, ctx.input_index);

  auto extract_one = [&](int idx) -> bool {
    return ExtractSingleNativeMetricTable(
        L, idx, processed_at, s.res_attrs_writer, s.scope_attrs_writer, s.dp_attrs_writer,
        s.exemplars_writer, s.payload_writer, s.metric_name, s.service, s.scope_name, s.scope_ver,
        s.metric_desc, s.metric_unit, s.bucket_counts, s.explicit_bounds, s.pos_bucket_counts,
        s.neg_bucket_counts, s.summary_quantiles, emitter, err_out, dropped_summary);
  };

  auto extract_array = [&](int arr_idx, size_t len) -> bool {
    for (size_t i = 1; i <= len; ++i) {
      lua_rawgeti(L, arr_idx, static_cast<lua_Integer>(i));
      if (lua_istable(L, -1)) {
        int item_idx = lua_absindex(L, -1);
        if (HasNativeMetricFields(L, item_idx)) {
          if (!extract_one(item_idx)) {
            lua_pop(L, 1);
            return false;
          }
        } else if (!core::IsLuaTableEmpty(L, item_idx)) {
          dropped_summary.other++;
        }
      }
      lua_pop(L, 1);
    }
    return true;
  };

  const size_t raw_len = lua_rawlen(L, abs_idx);
  if (raw_len > 0) {
    if (!extract_array(abs_idx, raw_len)) {
      return FailMetric(ctx, emitter, err_out);
    }
  } else {
    bool handled_wrapper = false;
    if (core::GetTableFieldRaw(L, abs_idx, "metric", "log")) {
      int inner_idx = lua_absindex(L, -1);
      if (lua_istable(L, inner_idx)) {
        const size_t inner_len = lua_rawlen(L, inner_idx);
        if (inner_len > 0) {
          handled_wrapper = true;
          if (!extract_array(inner_idx, inner_len)) {
            lua_pop(L, 1);
            return FailMetric(ctx, emitter, err_out);
          }
        } else if (HasNativeMetricFields(L, inner_idx)) {
          handled_wrapper = true;
          if (!extract_one(inner_idx)) {
            lua_pop(L, 1);
            return FailMetric(ctx, emitter, err_out);
          }
        } else if (!core::IsLuaTableEmpty(L, inner_idx)) {
          handled_wrapper = true;
          dropped_summary.other++;
        }
      }
      lua_pop(L, 1);
    }
    if (!handled_wrapper) {
      if (HasNativeMetricFields(L, abs_idx)) {
        if (!extract_one(abs_idx)) {
          return FailMetric(ctx, emitter, err_out);
        }
      } else if (!core::IsLuaTableEmpty(L, abs_idx)) {
        dropped_summary.other++;
      }
    }
  }

  dropped_summary.LogSummary(ResolveModuleName());

  emitter.Flush();
  return !emitter.HasError();
}

namespace {
constexpr core::SignalExtractors kNativeMetricsExtractors{ExtractNativeMetricsJson,
                                                          ExtractNativeMetricsTable};
}

ExtractionResult ProcessNativeMetrics(lua_State* L, std::string_view raw_json, int emit_idx) {
  return core::RunCompatJson(L, core::ModuleKind::Native, core::Signal::Metrics,
                             kNativeMetricsExtractors, raw_json, emit_idx);
}

ExtractionResult ProcessNativeMetricsTable(lua_State* L, int table_idx, int emit_idx) {
  return core::RunCompatTable(L, core::ModuleKind::Native, core::Signal::Metrics,
                              kNativeMetricsExtractors, table_idx, emit_idx);
}

const core::SignalExtractors& NativeMetricsExtractors() { return kNativeMetricsExtractors; }

}  // namespace vector_otel::native_extractors
