#include "vector_otel/native_extractors/native_metrics_extractor.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

using core::AnchorSlot;
using core::ErrorCode;
using core::Key;
using core::OperationContext;
using core::StagedRow;
using extractors::ExtractionResult;

constexpr uint64_t kMaxLuaInt = static_cast<uint64_t>(std::numeric_limits<lua_Integer>::max());

bool IsInfinityString(std::string_view sv) noexcept {
  sv = core::TrimJsonWhitespace(sv);
  return core::EqualsIgnoreCaseAscii(sv, "inf") || core::EqualsIgnoreCaseAscii(sv, "+inf") ||
         core::EqualsIgnoreCaseAscii(sv, "infinity") ||
         core::EqualsIgnoreCaseAscii(sv, "+infinity");
}

bool TryParseJsonFiniteBound(simdjson::ondemand::value& fv, double& out_bound) noexcept {
  auto t_res = fv.type();
  if (t_res.error() != simdjson::SUCCESS) {
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

enum class MetricValueKind { kNone, kDouble, kInt64 };

void ParseJsonMetricScalarValue(simdjson::ondemand::value& fv, MetricValueKind& val_kind,
                                double& double_val, int64_t& int_val) noexcept {
  auto t_res = fv.type();
  if (t_res.error() != simdjson::SUCCESS) {
    return;
  }
  const auto t = t_res.value();
  if (t == simdjson::ondemand::json_type::number) {
    auto num_res = fv.get_number();
    if (num_res.error() != simdjson::SUCCESS) {
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
                             double& double_val, int64_t& int_val) noexcept {
  auto t_res = fv.type();
  if (t_res.error() != simdjson::SUCCESS) {
    return;
  }
  if (t_res.value() == simdjson::ondemand::json_type::object) {
    auto obj_res = fv.get_object();
    if (obj_res.error() != simdjson::SUCCESS) {
      return;
    }
    for (auto field_res : obj_res.value()) {
      if (field_res.error() != simdjson::SUCCESS) continue;
      auto k_res = field_res.unescaped_key();
      if (k_res.error() != simdjson::SUCCESS) continue;
      std::string_view k = k_res.value();
      auto v_res = field_res.value();
      if (v_res.error() != simdjson::SUCCESS) continue;
      auto v = v_res.value();
      if (k == "value" || k == "asDouble" || k == "as_double") {
        ParseJsonMetricScalarValue(v, val_kind, double_val, int_val);
      } else if (k == "asInt" || k == "as_int") {
        int_val = core::ParseInt64(v);
        val_kind = MetricValueKind::kInt64;
      }
    }
  } else {
    ParseJsonMetricScalarValue(fv, val_kind, double_val, int_val);
  }
}

void ParseJsonHistogram(simdjson::ondemand::value& fv, uint64_t& hist_count, double& hist_sum,
                        bool& has_min, double& hist_min, bool& has_max, double& hist_max,
                        std::vector<int64_t>& bucket_counts, std::vector<double>& explicit_bounds) {
  bucket_counts.clear();
  explicit_bounds.clear();
  auto obj_res = fv.get_object();
  if (obj_res.error() != simdjson::SUCCESS) {
    return;
  }
  for (auto field_res : obj_res.value()) {
    if (field_res.error() != simdjson::SUCCESS) continue;
    auto k_res = field_res.unescaped_key();
    if (k_res.error() != simdjson::SUCCESS) continue;
    std::string_view k = k_res.value();
    auto v_res = field_res.value();
    if (v_res.error() != simdjson::SUCCESS) continue;
    auto v = v_res.value();

    if (k == "count") {
      hist_count = core::ParseUint64(v);
    } else if (k == "sum") {
      hist_sum = core::ParseDouble(v);
    } else if (k == "min") {
      if (core::TryParseDouble(v, hist_min)) {
        has_min = true;
      }
    } else if (k == "max") {
      if (core::TryParseDouble(v, hist_max)) {
        has_max = true;
      }
    } else if (k == "buckets") {
      auto arr_res = v.get_array();
      if (arr_res.error() == simdjson::SUCCESS) {
        for (auto elem_res : arr_res.value()) {
          if (elem_res.error() != simdjson::SUCCESS) continue;
          auto elem_val = elem_res.value();
          auto et_res = elem_val.type();
          if (et_res.error() != simdjson::SUCCESS) continue;
          if (et_res.value() == simdjson::ondemand::json_type::object) {
            auto b_obj_res = elem_val.get_object();
            if (b_obj_res.error() != simdjson::SUCCESS) continue;
            uint64_t bc = 0;
            for (auto bf_res : b_obj_res.value()) {
              if (bf_res.error() != simdjson::SUCCESS) continue;
              auto bk_res = bf_res.unescaped_key();
              if (bk_res.error() != simdjson::SUCCESS) continue;
              std::string_view bk = bk_res.value();
              auto bfv_res = bf_res.value();
              if (bfv_res.error() != simdjson::SUCCESS) continue;
              auto bfv = bfv_res.value();
              if (bk == "count") {
                bc = core::ParseUint64(bfv);
              } else if (bk == "upper_limit" || bk == "upperLimit") {
                double bound = 0.0;
                if (TryParseJsonFiniteBound(bfv, bound)) {
                  explicit_bounds.push_back(bound);
                }
              }
            }
            bucket_counts.push_back(static_cast<int64_t>(std::min(bc, kMaxLuaInt)));
          } else {
            double bound = 0.0;
            if (TryParseJsonFiniteBound(elem_val, bound)) {
              explicit_bounds.push_back(bound);
            }
          }
        }
      }
    } else if (k == "counts" || k == "bucketCounts" || k == "bucket_counts") {
      auto arr_res = v.get_array();
      if (arr_res.error() == simdjson::SUCCESS) {
        for (auto elem_res : arr_res.value()) {
          if (elem_res.error() != simdjson::SUCCESS) continue;
          auto elem_val = elem_res.value();
          uint64_t bc = core::ParseUint64(elem_val);
          bucket_counts.push_back(static_cast<int64_t>(std::min(bc, kMaxLuaInt)));
        }
      }
    } else if (k == "explicitBounds" || k == "explicit_bounds") {
      auto arr_res = v.get_array();
      if (arr_res.error() == simdjson::SUCCESS) {
        for (auto elem_res : arr_res.value()) {
          if (elem_res.error() != simdjson::SUCCESS) continue;
          auto elem_val = elem_res.value();
          double bound = 0.0;
          if (TryParseJsonFiniteBound(elem_val, bound)) {
            explicit_bounds.push_back(bound);
          }
        }
      }
    }
  }
}

struct SummaryQuantile {
  double quantile = 0.0;
  double value = 0.0;
};

int64_t ParseKindToTemporality(std::string_view kind_sv) noexcept {
  kind_sv = core::TrimJsonWhitespace(kind_sv);
  if (core::EqualsIgnoreCaseAscii(kind_sv, "incremental") ||
      core::EqualsIgnoreCaseAscii(kind_sv, "delta") ||
      core::EqualsIgnoreCaseAscii(kind_sv, "AGGREGATION_TEMPORALITY_DELTA")) {
    return 1;
  }
  if (core::EqualsIgnoreCaseAscii(kind_sv, "absolute") ||
      core::EqualsIgnoreCaseAscii(kind_sv, "cumulative") ||
      core::EqualsIgnoreCaseAscii(kind_sv, "AGGREGATION_TEMPORALITY_CUMULATIVE")) {
    return 2;
  }
  return 0;
}

void ParseJsonSummary(simdjson::ondemand::value& fv, uint64_t& sum_count, double& sum_total,
                      std::vector<SummaryQuantile>& quantiles,
                      std::vector<double>& scratch_bounds) {
  quantiles.clear();
  scratch_bounds.clear();
  auto obj_res = fv.get_object();
  if (obj_res.error() != simdjson::SUCCESS) {
    return;
  }
  bool has_parallel_quantiles = false;
  for (auto field_res : obj_res.value()) {
    if (field_res.error() != simdjson::SUCCESS) continue;
    auto k_res = field_res.unescaped_key();
    if (k_res.error() != simdjson::SUCCESS) continue;
    std::string_view k = k_res.value();
    auto v_res = field_res.value();
    if (v_res.error() != simdjson::SUCCESS) continue;
    auto v = v_res.value();

    if (k == "count") {
      sum_count = core::ParseUint64(v);
    } else if (k == "sum") {
      sum_total = core::ParseDouble(v);
    } else if (k == "quantiles" || k == "quantileValues" || k == "quantile_values") {
      auto arr_res = v.get_array();
      if (arr_res.error() == simdjson::SUCCESS) {
        for (auto elem_res : arr_res.value()) {
          if (elem_res.error() != simdjson::SUCCESS) continue;
          auto elem_val = elem_res.value();
          auto et_res = elem_val.type();
          if (et_res.error() != simdjson::SUCCESS) continue;
          if (et_res.value() == simdjson::ondemand::json_type::object) {
            auto q_obj_res = elem_val.get_object();
            if (q_obj_res.error() != simdjson::SUCCESS) continue;
            SummaryQuantile sq{};
            for (auto qf_res : q_obj_res.value()) {
              if (qf_res.error() != simdjson::SUCCESS) continue;
              auto qk_res = qf_res.unescaped_key();
              if (qk_res.error() != simdjson::SUCCESS) continue;
              std::string_view qk = qk_res.value();
              auto qfv_res = qf_res.value();
              if (qfv_res.error() != simdjson::SUCCESS) continue;
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
    } else if (k == "values") {
      auto arr_res = v.get_array();
      if (arr_res.error() == simdjson::SUCCESS) {
        for (auto elem_res : arr_res.value()) {
          if (elem_res.error() != simdjson::SUCCESS) continue;
          auto elem_val = elem_res.value();
          scratch_bounds.push_back(core::ParseDouble(elem_val));
        }
      }
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
  enum class Kind { kNone, kString, kInt64, kUint64, kDouble, kBool } kind = Kind::kNone;
  std::string_view str_val;
  int64_t int_val = 0;
  uint64_t uint_val = 0;
  double dbl_val = 0.0;
  bool bool_val = false;
};

void ReadSingleJsonTagElement(simdjson::ondemand::value& v, core::FastJsonWriter& writer,
                              JsonTagValue& out) {
  auto t_res = v.type();
  if (t_res.error() != simdjson::SUCCESS) {
    return;
  }
  const auto t = t_res.value();
  if (t == simdjson::ondemand::json_type::string) {
    auto s_res = v.get_string();
    if (s_res.error() == simdjson::SUCCESS) {
      out.kind = JsonTagValue::Kind::kString;
      out.str_val = s_res.value();
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
    }
  } else if (t == simdjson::ondemand::json_type::boolean) {
    auto b_res = v.get_bool();
    if (b_res.error() == simdjson::SUCCESS) {
      out.kind = JsonTagValue::Kind::kBool;
      out.bool_val = b_res.value();
    }
  } else if (t == simdjson::ondemand::json_type::object) {
    auto raw_res = v.raw_json();
    if (raw_res.error() == simdjson::SUCCESS) {
      writer.Reset();
      writer.RawJson(core::TrimJsonWhitespace(raw_res.value()));
      out.kind = JsonTagValue::Kind::kString;
      out.str_val = writer.Str();
    }
  }
}

// Exact text of the unsupported-variant diagnostic, built without allocating.
bool FailUnsupportedVariant(OperationContext& ctx, std::string_view variant) {
  static constexpr std::string_view kPrefix = "Unsupported non-OTLP metric variant '";
  static constexpr std::string_view kSuffix =
      "': vector_native only supports OpenTelemetry metric types (counter, gauge, "
      "aggregated_histogram, aggregated_summary)";
  char buf[256];
  std::size_t n = 0;
  auto append = [&](std::string_view text) {
    const std::size_t take = std::min(text.size(), sizeof(buf) - n);
    std::memcpy(buf + n, text.data(), take);
    n += take;
  };
  append(kPrefix);
  append(variant.substr(0, 16));
  append(kSuffix);
  ctx.error.Set(ErrorCode::Schema, std::string_view(buf, n));
  return false;
}

bool FailJson(OperationContext& ctx, simdjson::error_code code) {
  ctx.error.Set(ErrorCode::Json, simdjson::error_message(code));
  return false;
}

// Per-nesting-level staging state. A wrapper ({"metric": {...}}) nests at most one level, so two
// levels cover an enclosing object and its record; the table path uses level 0. Everything is
// retained between invocations, so the steady state performs no C++ allocation per record.
struct Level {
  StagedRow row;            // the record being built
  StagedRow res_tape;       // ResourceAttributes (tags "resource.*" plus resources / resource)
  StagedRow scope_tape;     // ScopeAttributes (tags "scope.*" plus scope.attributes)
  StagedRow dp_tape;        // Attributes (remaining tags plus attributes)
  StagedRow bucket_tape;    // histogram bucketCounts
  StagedRow bounds_tape;    // histogram explicitBounds
  StagedRow quantile_tape;  // summary quantileValues
  std::string metric_name;
  std::string service;
  std::string scope_name;
  std::string scope_ver;
  std::string metric_desc;
  std::string metric_unit;
  std::vector<int64_t> bucket_counts;
  std::vector<double> explicit_bounds;
  std::vector<SummaryQuantile> summary_quantiles;
};

struct NativeMetricsScratch {
  core::FastJsonWriter writer;  // tag / attribute serialisation
  Level levels[2];
};

NativeMetricsScratch& Scratch() {
  static thread_local NativeMetricsScratch scratch;
  return scratch;
}

void ReserveLevel(Level& lv) {
  core::ReserveScratchString(lv.metric_name);
  core::ReserveScratchString(lv.service);
  core::ReserveScratchString(lv.scope_name);
  core::ReserveScratchString(lv.scope_ver);
  core::ReserveScratchString(lv.metric_desc);
  core::ReserveScratchString(lv.metric_unit);
  core::ReserveScratchVector(lv.bucket_counts);
  core::ReserveScratchVector(lv.explicit_bounds);
  core::ReserveScratchVector(lv.summary_quantiles);
}

void ResetLevel(Level& lv) noexcept {
  core::TrimScratchString(lv.metric_name);
  core::TrimScratchString(lv.service);
  core::TrimScratchString(lv.scope_name);
  core::TrimScratchString(lv.scope_ver);
  core::TrimScratchString(lv.metric_desc);
  core::TrimScratchString(lv.metric_unit);
  core::ResetScratchVector(lv.bucket_counts);
  core::ResetScratchVector(lv.explicit_bounds);
  core::ResetScratchVector(lv.summary_quantiles);
}

// ---- row staging -------------------------------------------------------------------------------

void DualInt(StagedRow& r, Key camel, Key pascal, std::int64_t v) {
  r.Int(camel, v);
  r.Int(pascal, v);
}
void DualUint(StagedRow& r, Key camel, Key pascal, std::uint64_t v) {
  r.Uint(camel, v);
  r.Uint(pascal, v);
}
void DualDbl(StagedRow& r, Key camel, Key pascal, double v) {
  r.Dbl(camel, v);
  r.Dbl(pascal, v);
}
void DualAnchor(StagedRow& r, Key camel, Key pascal, AnchorSlot slot) {
  r.Anchor(camel, slot);
  r.Anchor(pascal, slot);
}

// Dry runs never touch Lua; otherwise the tape becomes a table in its anchor slot.
bool MaterializeTape(OperationContext& ctx, const StagedRow& tape, AnchorSlot slot) {
  return ctx.dry_run || core::MaterializeToAnchor(ctx, tape, slot);
}

// Everything a record needs, collected before the row is staged.
struct MetricRecord {
  std::string_view name;
  std::string_view description;
  std::string_view unit;
  std::string_view type;
  std::string_view service;
  std::string_view scope_name;
  std::string_view scope_version;
  std::string_view start_time;
  std::string_view time_unix;
  bool is_sum = false;
  bool is_histogram = false;
  bool is_summary = false;
  int64_t agg_temporality = 0;
  bool has_is_monotonic = false;
  bool is_monotonic = false;
  MetricValueKind val_kind = MetricValueKind::kNone;
  double double_val = 0.0;
  int64_t int_val = 0;
  uint64_t count = 0;
  double sum = 0.0;
  bool has_min = false;
  double min = 0.0;
  bool has_max = false;
  double max = 0.0;
  int64_t flags = 0;
};

// Stages every column except Exemplars and processed_at (the table path stages its own exemplars).
// The attribute maps and the histogram / summary arrays are anchors, materialised by
// MaterializeRecordTapes() right before the row is delivered.
void StageRecordColumns(StagedRow& r, const MetricRecord& m) {
  r.Str("MetricName", m.name);
  r.Str("MetricDescription", m.description);
  r.Str("MetricUnit", m.unit);
  r.Str("MetricType", m.type);
  r.Str("ServiceName", m.service);
  if (m.is_sum || m.is_histogram) {
    r.Int("AggregationTemporality", m.agg_temporality);
  }
  if (m.is_sum && m.has_is_monotonic) {
    r.Bool("IsMonotonic", m.is_monotonic);
  }
  r.Anchor("ResourceAttributes", AnchorSlot::kResourceAttrs);
  r.Str("ScopeName", m.scope_name);
  r.Str("ScopeVersion", m.scope_version);
  r.Anchor("ScopeAttributes", AnchorSlot::kScopeAttrs);
  r.Anchor("Attributes", AnchorSlot::kRecordAttrs);
  r.Str("StartTimeUnix", m.start_time);
  r.Str("TimeUnix", m.time_unix);
  if (!m.is_histogram && !m.is_summary) {
    if (m.val_kind == MetricValueKind::kDouble) {
      r.Dbl("Value", m.double_val);
    } else if (m.val_kind == MetricValueKind::kInt64) {
      r.Int("Value", m.int_val);
    }
  }
  if (m.is_histogram) {
    r.BeginMap("Histogram");
    DualInt(r, "aggregationTemporality", "AggregationTemporality", m.agg_temporality);
    DualUint(r, "count", "Count", m.count);
    DualDbl(r, "sum", "Sum", m.sum);
    if (m.has_min) DualDbl(r, "min", "Min", m.min);
    if (m.has_max) DualDbl(r, "max", "Max", m.max);
    DualAnchor(r, "bucketCounts", "BucketCounts", AnchorSlot::kBucketCounts);
    DualAnchor(r, "explicitBounds", "ExplicitBounds", AnchorSlot::kExplicitBounds);
    r.End();
  }
  if (m.is_summary) {
    r.BeginMap("Summary");
    DualUint(r, "count", "Count", m.count);
    DualDbl(r, "sum", "Sum", m.sum);
    DualAnchor(r, "quantileValues", "QuantileValues", AnchorSlot::kQuantiles);
    r.End();
  }
  r.Int("Flags", m.flags);
}

// Builds the array tapes from the parsed vectors and anchors every table the row references.
bool MaterializeRecordTapes(OperationContext& ctx, Level& lv, const MetricRecord& m) {
  if (!MaterializeTape(ctx, lv.res_tape, AnchorSlot::kResourceAttrs) ||
      !MaterializeTape(ctx, lv.scope_tape, AnchorSlot::kScopeAttrs) ||
      !MaterializeTape(ctx, lv.dp_tape, AnchorSlot::kRecordAttrs)) {
    return false;
  }
  if (m.is_histogram) {
    lv.bucket_tape.Reset(/*root_array=*/true);
    for (const int64_t c : lv.bucket_counts) lv.bucket_tape.Int(Key(), c);
    lv.bounds_tape.Reset(/*root_array=*/true);
    for (const double b : lv.explicit_bounds) lv.bounds_tape.Dbl(Key(), b);
    return MaterializeTape(ctx, lv.bucket_tape, AnchorSlot::kBucketCounts) &&
           MaterializeTape(ctx, lv.bounds_tape, AnchorSlot::kExplicitBounds);
  }
  if (m.is_summary) {
    lv.quantile_tape.Reset(/*root_array=*/true);
    for (const SummaryQuantile& q : lv.summary_quantiles) {
      lv.quantile_tape.BeginMap(Key());
      DualDbl(lv.quantile_tape, "quantile", "Quantile", q.quantile);
      DualDbl(lv.quantile_tape, "value", "Value", q.value);
      lv.quantile_tape.End();
    }
    return MaterializeTape(ctx, lv.quantile_tape, AnchorSlot::kQuantiles);
  }
  return true;
}

// ---- tag staging
// ---------------------------------------------------------------------------------

void StageJsonTagValue(StagedRow& tape, std::string_view key, const JsonTagValue& tv) {
  switch (tv.kind) {
    case JsonTagValue::Kind::kString:
      tape.Str(Key::Dynamic(key), tv.str_val);
      break;
    case JsonTagValue::Kind::kInt64:
      tape.Int(Key::Dynamic(key), tv.int_val);
      break;
    case JsonTagValue::Kind::kUint64:
      tape.Uint(Key::Dynamic(key), tv.uint_val);
      break;
    case JsonTagValue::Kind::kDouble:
      tape.Dbl(Key::Dynamic(key), tv.dbl_val);
      break;
    case JsonTagValue::Kind::kBool:
      tape.Bool(Key::Dynamic(key), tv.bool_val);
      break;
    case JsonTagValue::Kind::kNone:
      break;
  }
}

// Scalars keep their type; a table is serialised to a JSON string; anything else becomes "".
void StageLuaTag(StagedRow& tape, std::string_view key, lua_State* L, int val_idx,
                 core::FastJsonWriter& writer) {
  const int abs_idx = lua_absindex(L, val_idx);
  switch (lua_type(L, abs_idx)) {
    case LUA_TSTRING: {
      size_t len = 0;
      const char* s = lua_tolstring(L, abs_idx, &len);
      tape.Str(Key::Dynamic(key), std::string_view(s, len));
      break;
    }
    case LUA_TNUMBER:
      if (lua_isinteger(L, abs_idx)) {
        tape.Int(Key::Dynamic(key), static_cast<int64_t>(lua_tointeger(L, abs_idx)));
      } else {
        tape.Dbl(Key::Dynamic(key), static_cast<double>(lua_tonumber(L, abs_idx)));
      }
      break;
    case LUA_TBOOLEAN:
      tape.Bool(Key::Dynamic(key), lua_toboolean(L, abs_idx) != 0);
      break;
    case LUA_TTABLE:
      writer.Reset();
      core::SerializeLuaValueToJson(L, abs_idx, writer);
      tape.Str(Key::Dynamic(key), writer.Str());
      break;
    default:
      tape.Str(Key::Dynamic(key), "");
      break;
  }
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

void ReadLuaMetricValue(lua_State* L, int type_obj_idx, MetricValueKind& val_kind,
                        double& double_val, int64_t& int_val) {
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
    if (core::GetTableFieldRaw(L, abs_idx, "value", "asDouble")) {
      parse_val_at(-1);
      lua_pop(L, 1);
    } else if (core::GetTableFieldRaw(L, abs_idx, "as_double")) {
      parse_val_at(-1);
      lua_pop(L, 1);
    } else if (core::GetTableFieldRaw(L, abs_idx, "asInt", "as_int")) {
      parse_val_at(-1);
      lua_pop(L, 1);
    }
  } else {
    parse_val_at(abs_idx);
  }
}

void ReadLuaHistogram(lua_State* L, int hist_idx, uint64_t& hist_count, double& hist_sum,
                      bool& has_min, double& hist_min, bool& has_max, double& hist_max,
                      std::vector<int64_t>& bucket_counts, std::vector<double>& explicit_bounds) {
  int abs_idx = lua_absindex(L, hist_idx);
  bucket_counts.clear();
  explicit_bounds.clear();
  if (!lua_istable(L, abs_idx)) {
    return;
  }

  hist_count = core::ReadTableUint64(L, abs_idx, "count", nullptr, 0);
  hist_sum = core::ReadTableDouble(L, abs_idx, "sum", nullptr, 0.0);

  if (core::GetTableFieldRaw(L, abs_idx, "min")) {
    double d = 0.0;
    if (TryParseLuaFiniteBound(L, -1, d)) {
      hist_min = d;
      has_min = true;
    }
    lua_pop(L, 1);
  }
  if (core::GetTableFieldRaw(L, abs_idx, "max")) {
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
              uint64_t bc = core::ReadTableUint64(L, b_obj_idx, "count", nullptr, 0);
              bucket_counts.push_back(static_cast<int64_t>(std::min(bc, kMaxLuaInt)));
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
  } else if (core::GetTableFieldRaw(L, abs_idx, "explicitBounds", "explicit_bounds")) {
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
    bool has_counts = core::GetTableFieldRaw(L, abs_idx, "counts", "bucketCounts");
    if (!has_counts) {
      has_counts = core::GetTableFieldRaw(L, abs_idx, "bucket_counts");
    }
    if (has_counts) {
      int c_arr_idx = lua_absindex(L, -1);
      if (lua_istable(L, c_arr_idx)) {
        const size_t c_len = lua_rawlen(L, c_arr_idx);
        for (size_t i = 1; i <= c_len; ++i) {
          lua_rawgeti(L, c_arr_idx, static_cast<lua_Integer>(i));
          uint64_t bc = 0;
          if (lua_isinteger(L, -1)) {
            lua_Integer iv = lua_tointeger(L, -1);
            if (iv >= 0) {
              bc = static_cast<uint64_t>(iv);
            }
          } else if (lua_type(L, -1) == LUA_TNUMBER) {
            double d = lua_tonumber(L, -1);
            if (std::isfinite(d) && d >= 0.0 && d < 18446744073709551616.0) {
              bc = static_cast<uint64_t>(d);
            }
          } else if (lua_type(L, -1) == LUA_TSTRING) {
            size_t slen = 0;
            const char* s = lua_tolstring(L, -1, &slen);
            if (s != nullptr) {
              std::string_view sv = core::TrimJsonWhitespace(std::string_view(s, slen));
              uint64_t parsed_bc = 0;
              auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), parsed_bc);
              if (ec == std::errc() && ptr == sv.data() + sv.size()) {
                bc = parsed_bc;
              }
            }
          }
          bucket_counts.push_back(static_cast<int64_t>(std::min(bc, kMaxLuaInt)));
          lua_pop(L, 1);
        }
      }
      lua_pop(L, 1);
    }
  }
}

void ReadLuaSummary(lua_State* L, int sum_idx, uint64_t& sum_count, double& sum_total,
                    std::vector<SummaryQuantile>& quantiles) {
  int abs_idx = lua_absindex(L, sum_idx);
  quantiles.clear();
  if (!lua_istable(L, abs_idx)) {
    return;
  }

  sum_count = core::ReadTableUint64(L, abs_idx, "count", "Count", 0);
  sum_total = core::ReadTableDouble(L, abs_idx, "sum", "Sum", 0.0);

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
}

// ---- JSON input --------------------------------------------------------------------------------

bool ExtractNativeMetricObjectJson(OperationContext& ctx, NativeMetricsScratch& s,
                                   simdjson::ondemand::object& rec_obj,
                                   std::string_view processed_at, int depth,
                                   bool* out_matched = nullptr) {
  Level& lv = s.levels[depth];
  core::RowEmitter emitter(ctx, lv.row);

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

  MetricRecord m;
  uint64_t hist_count = 0;
  double hist_sum = 0.0;
  bool has_min = false;
  double hist_min = 0.0;
  bool has_max = false;
  double hist_max = 0.0;
  lv.bucket_counts.clear();
  lv.explicit_bounds.clear();
  lv.summary_quantiles.clear();
  lv.res_tape.Reset();
  lv.scope_tape.Reset();
  lv.dp_tape.Reset();

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

    if ((k == "metric" || k == "log") && depth == 0) {
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
        if (!ExtractNativeMetricObjectJson(ctx, s, inner_obj, processed_at, depth + 1,
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
          if (!ExtractNativeMetricObjectJson(ctx, s, elem_obj, processed_at, depth + 1,
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

    if (k == "distribution" || k == "set" || k == "sketch") {
      auto t_res = fv.type();
      if (t_res.error() == simdjson::SUCCESS &&
          t_res.value() != simdjson::ondemand::json_type::null) {
        return FailUnsupportedVariant(ctx, k);
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
      m.flags = core::ParseInt64(fv);
    } else if (k == "kind" || k == "aggregationTemporality" || k == "aggregation_temporality") {
      has_recognized_field = true;
      auto t_res = fv.type();
      if (t_res.error() == simdjson::SUCCESS) {
        if (t_res.value() == simdjson::ondemand::json_type::string) {
          auto s_res = fv.get_string();
          if (s_res.error() == simdjson::SUCCESS) {
            m.agg_temporality = ParseKindToTemporality(s_res.value());
          }
        } else if (t_res.value() == simdjson::ondemand::json_type::number) {
          m.agg_temporality = core::ParseInt64(fv);
        }
      }
    } else if (k == "isMonotonic" || k == "is_monotonic") {
      has_recognized_field = true;
      auto b_res = fv.get_bool();
      if (b_res.error() == simdjson::SUCCESS) {
        m.is_monotonic = b_res.value();
        m.has_is_monotonic = true;
      }
    } else if (k == "counter" || k == "sum") {
      has_recognized_field = true;
      metric_type = "sum";
      ParseJsonCounterOrGauge(fv, m.val_kind, m.double_val, m.int_val);
    } else if (k == "gauge") {
      has_recognized_field = true;
      metric_type = "gauge";
      ParseJsonCounterOrGauge(fv, m.val_kind, m.double_val, m.int_val);
    } else if (k == "aggregated_histogram" || k == "histogram") {
      has_recognized_field = true;
      metric_type = "histogram";
      ParseJsonHistogram(fv, hist_count, hist_sum, has_min, hist_min, has_max, hist_max,
                         lv.bucket_counts, lv.explicit_bounds);
    } else if (k == "aggregated_summary" || k == "summary") {
      has_recognized_field = true;
      metric_type = "summary";
      ParseJsonSummary(fv, hist_count, hist_sum, lv.summary_quantiles, lv.explicit_bounds);
    } else if (k == "tags") {
      has_recognized_field = true;
      auto tags_obj_res = fv.get_object();
      if (tags_obj_res.error() == simdjson::SUCCESS) {
        for (auto tf_res : tags_obj_res.value()) {
          if (tf_res.error() != simdjson::SUCCESS) continue;
          auto tk_res = tf_res.unescaped_key();
          if (tk_res.error() != simdjson::SUCCESS) continue;
          std::string_view tk = tk_res.value();
          auto tv_res = tf_res.value();
          if (tv_res.error() != simdjson::SUCCESS) continue;
          auto tv = tv_res.value();

          JsonTagValue tag_val;
          auto tv_type_res = tv.type();
          if (tv_type_res.error() != simdjson::SUCCESS) continue;
          if (tv_type_res.value() == simdjson::ondemand::json_type::array) {
            auto arr_res = tv.get_array();
            if (arr_res.error() == simdjson::SUCCESS) {
              for (auto elem_res : arr_res.value()) {
                if (elem_res.error() != simdjson::SUCCESS) continue;
                auto elem_v = elem_res.value();
                ReadSingleJsonTagElement(elem_v, s.writer, tag_val);
              }
            }
          } else {
            ReadSingleJsonTagElement(tv, s.writer, tag_val);
          }

          if (tag_val.kind == JsonTagValue::Kind::kNone) {
            continue;
          }

          if (tk.size() > 9 && tk.substr(0, 9) == "resource.") {
            std::string_view sub_key = tk.substr(9);
            if ((sub_key == "service.name" || sub_key == "service_name") &&
                tag_val.kind == JsonTagValue::Kind::kString && !tag_val.str_val.empty()) {
              lv.service.assign(tag_val.str_val);
              service_name = std::string_view(lv.service);
            }
            StageJsonTagValue(lv.res_tape, sub_key, tag_val);
          } else if (tk.size() > 6 && tk.substr(0, 6) == "scope.") {
            std::string_view sub_key = tk.substr(6);
            if (sub_key == "name") {
              if (tag_val.kind == JsonTagValue::Kind::kString) {
                lv.scope_name.assign(tag_val.str_val);
                scope_name = std::string_view(lv.scope_name);
              }
            } else if (sub_key == "version") {
              if (tag_val.kind == JsonTagValue::Kind::kString) {
                lv.scope_ver.assign(tag_val.str_val);
                scope_version = std::string_view(lv.scope_ver);
              }
            } else {
              StageJsonTagValue(lv.scope_tape, sub_key, tag_val);
            }
          } else if (tk == "otel.unit" || tk == "metric.unit") {
            if (tag_val.kind == JsonTagValue::Kind::kString && metric_unit.empty()) {
              lv.metric_unit.assign(tag_val.str_val);
              metric_unit = std::string_view(lv.metric_unit);
            }
          } else if (tk == "otel.description" || tk == "metric.description") {
            if (tag_val.kind == JsonTagValue::Kind::kString && metric_desc.empty()) {
              lv.metric_desc.assign(tag_val.str_val);
              metric_desc = std::string_view(lv.metric_desc);
            }
          } else if (tk == "otel.scope.name") {
            if (tag_val.kind == JsonTagValue::Kind::kString && scope_name.empty()) {
              lv.scope_name.assign(tag_val.str_val);
              scope_name = std::string_view(lv.scope_name);
            }
          } else if (tk == "otel.scope.version") {
            if (tag_val.kind == JsonTagValue::Kind::kString && scope_version.empty()) {
              lv.scope_ver.assign(tag_val.str_val);
              scope_version = std::string_view(lv.scope_ver);
            }
          } else {
            StageJsonTagValue(lv.dp_tape, tk, tag_val);
            if ((tk == "service.name" || tk == "service_name") && service_name.empty() &&
                tag_val.kind == JsonTagValue::Kind::kString && !tag_val.str_val.empty()) {
              lv.service.assign(tag_val.str_val);
              service_name = std::string_view(lv.service);
            }
          }
        }
      }
    } else if (k == "resources" || k == "resource_attributes" || k == "resourceAttributes" ||
               k == "resource") {
      has_recognized_field = true;
      std::string_view extracted_svc = "unknown_service";
      core::StageAttributeFields(lv.res_tape, fv, s.writer, &extracted_svc);
      if (extracted_svc != "unknown_service" && !extracted_svc.empty()) {
        lv.service.assign(extracted_svc);
        service_name = std::string_view(lv.service);
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
              lv.scope_name.assign(s_res.value());
              scope_name = std::string_view(lv.scope_name);
            }
          } else if (sk == "version") {
            auto s_res = sfv.get_string();
            if (s_res.error() == simdjson::SUCCESS) {
              lv.scope_ver.assign(s_res.value());
              scope_version = std::string_view(lv.scope_ver);
            }
          } else if (sk == "attributes") {
            core::StageAttributeFields(lv.scope_tape, sfv, s.writer);
          }
        }
      }
    } else if (k == "attributes") {
      has_recognized_field = true;
      core::StageAttributeFields(lv.dp_tape, fv, s.writer);
    } else if (k == "service_name" || k == "service") {
      has_recognized_field = true;
      auto s_res = fv.get_string();
      if (s_res.error() == simdjson::SUCCESS) {
        top_service_name = s_res.value();
      }
    }
  }

  if (!has_recognized_field || metric_type.empty()) {
    return true;
  }
  if (out_matched != nullptr) {
    *out_matched = true;
  }

  std::string_view metric_name = raw_name;
  if (!metric_ns.empty() && !raw_name.empty()) {
    lv.metric_name.clear();
    lv.metric_name.append(metric_ns);
    lv.metric_name.push_back('.');
    lv.metric_name.append(raw_name);
    metric_name = std::string_view(lv.metric_name);
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

  m.name = metric_name;
  m.description = metric_desc;
  m.unit = metric_unit;
  m.type = metric_type;
  m.service = service_name;
  m.scope_name = scope_name;
  m.scope_version = scope_version;
  m.start_time = start_time_sv;
  m.time_unix = time_unix_sv;
  m.is_sum = (metric_type == "sum");
  m.is_histogram = (metric_type == "histogram");
  m.is_summary = (metric_type == "summary");
  m.count = hist_count;
  m.sum = hist_sum;
  m.has_min = has_min;
  m.min = hist_min;
  m.has_max = has_max;
  m.max = hist_max;

  emitter.StartRecord();
  StageRecordColumns(lv.row, m);
  lv.row.BeginArray("Exemplars");
  lv.row.End();
  lv.row.Str("processed_at", processed_at);
  if (!MaterializeRecordTapes(ctx, lv, m)) {
    return false;
  }
  emitter.EndRecord();
  return !emitter.HasError();
}

// ---- table input -------------------------------------------------------------------------------

bool ExtractSingleNativeMetricTable(OperationContext& ctx, NativeMetricsScratch& s, int rec_idx,
                                    std::string_view processed_at) {
  lua_State* L = ctx.L;
  Level& lv = s.levels[0];
  core::RowEmitter emitter(ctx, lv.row);
  const int abs_rec_idx = lua_absindex(L, rec_idx);

  // 1. Unsupported non-OTLP metric variants are rejected first.
  static constexpr core::ReadKey kUnsupportedVariants[] = {"distribution", "set", "sketch"};
  for (const core::ReadKey& v : kUnsupportedVariants) {
    if (core::GetTableFieldRaw(L, abs_rec_idx, v)) {
      lua_pop(L, 1);
      return FailUnsupportedVariant(ctx, v.c_str());
    }
  }

  // 2. Detect the supported OTLP metric variant.
  MetricRecord m;
  std::string_view metric_type;
  uint64_t hist_count = 0;
  double hist_sum = 0.0;
  bool has_min = false;
  double hist_min = 0.0;
  bool has_max = false;
  double hist_max = 0.0;
  lv.bucket_counts.clear();
  lv.explicit_bounds.clear();
  lv.summary_quantiles.clear();

  if (core::GetTableFieldRaw(L, abs_rec_idx, "counter", "sum")) {
    metric_type = "sum";
    ReadLuaMetricValue(L, -1, m.val_kind, m.double_val, m.int_val);
    lua_pop(L, 1);
  } else if (core::GetTableFieldRaw(L, abs_rec_idx, "gauge")) {
    metric_type = "gauge";
    ReadLuaMetricValue(L, -1, m.val_kind, m.double_val, m.int_val);
    lua_pop(L, 1);
  } else if (core::GetTableFieldRaw(L, abs_rec_idx, "aggregated_histogram", "histogram")) {
    metric_type = "histogram";
    ReadLuaHistogram(L, -1, hist_count, hist_sum, has_min, hist_min, has_max, hist_max,
                     lv.bucket_counts, lv.explicit_bounds);
    lua_pop(L, 1);
  } else if (core::GetTableFieldRaw(L, abs_rec_idx, "aggregated_summary", "summary")) {
    metric_type = "summary";
    ReadLuaSummary(L, -1, hist_count, hist_sum, lv.summary_quantiles);
    lua_pop(L, 1);
  }

  if (metric_type.empty()) {
    return true;
  }

  if (core::GetTableFieldRaw(L, abs_rec_idx, "kind", "aggregationTemporality")) {
    if (lua_type(L, -1) == LUA_TSTRING) {
      size_t klen = 0;
      const char* ks = lua_tolstring(L, -1, &klen);
      if (ks != nullptr) {
        m.agg_temporality = ParseKindToTemporality(std::string_view(ks, klen));
      }
    } else if (lua_type(L, -1) == LUA_TNUMBER) {
      int64_t parsed_temp = 0;
      if (core::TryParseTableInt64At(L, -1, parsed_temp)) {
        m.agg_temporality = parsed_temp;
      }
    }
    lua_pop(L, 1);
  }

  if (core::GetTableFieldRaw(L, abs_rec_idx, "isMonotonic", "is_monotonic")) {
    if (lua_isboolean(L, -1)) {
      m.is_monotonic = lua_toboolean(L, -1) != 0;
      m.has_is_monotonic = true;
    }
    lua_pop(L, 1);
  }

  // 3. MetricName (namespace .. "." .. name), MetricDescription, MetricUnit.
  std::string_view raw_name =
      core::ReadTableStringView(L, abs_rec_idx, "name", nullptr, lv.service);
  std::string_view metric_ns =
      core::ReadTableStringView(L, abs_rec_idx, "namespace", nullptr, lv.metric_name);
  std::string_view metric_name = raw_name;
  if (!metric_ns.empty() && !raw_name.empty()) {
    if (metric_ns.data() != lv.metric_name.data()) {
      lv.metric_name.clear();
      lv.metric_name.append(metric_ns);
    }
    lv.metric_name.push_back('.');
    lv.metric_name.append(raw_name);
    metric_name = std::string_view(lv.metric_name);
  } else if (!metric_ns.empty()) {
    metric_name = metric_ns;
  } else if (raw_name.data() == lv.service.data() && !raw_name.empty()) {
    lv.metric_name.assign(raw_name);
    metric_name = std::string_view(lv.metric_name);
  }

  std::string_view metric_desc =
      core::ReadTableStringView(L, abs_rec_idx, "description", nullptr, lv.metric_desc);
  std::string_view metric_unit =
      core::ReadTableStringView(L, abs_rec_idx, "unit", nullptr, lv.metric_unit);

  // 4. Timestamps: TimeUnix and StartTimeUnix.
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

  // 5. Optional resources / scope / attributes tables + single-pass tags splitting. The three
  //    attribute maps are staged into tapes; later writes to a key win, exactly as the legacy
  //    raw-set into one table did.
  lv.res_tape.Reset();
  lv.scope_tape.Reset();
  lv.dp_tape.Reset();
  std::string_view service_name;
  std::string_view scope_name =
      core::ReadTableStringView(L, abs_rec_idx, "scope_name", "scopeName", lv.scope_name);
  std::string_view scope_version =
      core::ReadTableStringView(L, abs_rec_idx, "scope_version", "scopeVersion", lv.scope_ver);

  bool has_res = core::GetTableFieldRaw(L, abs_rec_idx, "resources", "resource_attributes");
  if (!has_res) {
    has_res = core::GetTableFieldRaw(L, abs_rec_idx, "resourceAttributes", "resource");
  }
  if (has_res) {
    const int res_idx = lua_absindex(L, -1);
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
      core::StageTableAttributeFields(lv.res_tape, L, attrs_src_idx, s.writer, &service_name);
      if (!service_name.empty()) {
        lv.service.assign(service_name);
        service_name = std::string_view(lv.service);
      }
      if (has_nested_attrs) {
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }

  if (core::GetTableFieldRaw(L, abs_rec_idx, "scope")) {
    const int s_idx = lua_absindex(L, -1);
    if (lua_istable(L, s_idx)) {
      if (scope_name.empty()) {
        scope_name = core::ReadTableStringView(L, s_idx, "name", nullptr, lv.scope_name);
      }
      if (scope_version.empty()) {
        scope_version = core::ReadTableStringView(L, s_idx, "version", nullptr, lv.scope_ver);
      }
      if (core::GetTableFieldRaw(L, s_idx, "attributes")) {
        core::StageTableAttributeFields(lv.scope_tape, L, lua_absindex(L, -1), s.writer);
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }

  if (core::GetTableFieldRaw(L, abs_rec_idx, "attributes")) {
    if (lua_istable(L, -1)) {
      core::StageTableAttributeFields(lv.dp_tape, L, lua_absindex(L, -1), s.writer);
    }
    lua_pop(L, 1);
  }

  if (core::GetTableFieldRaw(L, abs_rec_idx, "tags")) {
    const int tags_idx = lua_absindex(L, -1);
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
              std::string_view sv = ReadLuaScalarStringView(L, val_idx, lv.service);
              if (!sv.empty()) {
                service_name = sv;
              }
            }
            StageLuaTag(lv.res_tape, sub_key, L, val_idx, s.writer);
          } else if (k.size() > 6 && k.substr(0, 6) == "scope.") {
            std::string_view sub_key = k.substr(6);
            if (sub_key == "name") {
              std::string_view sv = ReadLuaScalarStringView(L, val_idx, lv.scope_name);
              if (!sv.empty()) {
                scope_name = sv;
              }
            } else if (sub_key == "version") {
              std::string_view sv = ReadLuaScalarStringView(L, val_idx, lv.scope_ver);
              if (!sv.empty()) {
                scope_version = sv;
              }
            } else {
              StageLuaTag(lv.scope_tape, sub_key, L, val_idx, s.writer);
            }
          } else if (k == "otel.unit" || k == "metric.unit") {
            if (metric_unit.empty()) {
              metric_unit = ReadLuaScalarStringView(L, val_idx, lv.metric_unit);
            }
          } else if (k == "otel.description" || k == "metric.description") {
            if (metric_desc.empty()) {
              metric_desc = ReadLuaScalarStringView(L, val_idx, lv.metric_desc);
            }
          } else if (k == "otel.scope.name") {
            if (scope_name.empty()) {
              scope_name = ReadLuaScalarStringView(L, val_idx, lv.scope_name);
            }
          } else if (k == "otel.scope.version") {
            if (scope_version.empty()) {
              scope_version = ReadLuaScalarStringView(L, val_idx, lv.scope_ver);
            }
          } else {
            StageLuaTag(lv.dp_tape, k, L, val_idx, s.writer);
            if ((k == "service.name" || k == "service_name") &&
                (service_name.empty() || service_name == "unknown_service")) {
              std::string_view sv = ReadLuaScalarStringView(L, val_idx, lv.service);
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

  if (service_name.empty() || service_name == "unknown_service") {
    std::string_view top_svc =
        core::ReadTableStringView(L, abs_rec_idx, "service_name", "service", lv.service);
    if (!top_svc.empty()) {
      service_name = top_svc;
    } else {
      service_name = "unknown_service";
    }
  }

  m.flags = core::ReadTableInt64(L, abs_rec_idx, "flags", nullptr, 0);
  m.name = metric_name;
  m.description = metric_desc;
  m.unit = metric_unit;
  m.type = metric_type;
  m.service = service_name;
  m.scope_name = scope_name;
  m.scope_version = scope_version;
  m.start_time = start_time_sv;
  m.time_unix = time_unix_sv;
  m.is_sum = (metric_type == "sum");
  m.is_histogram = (metric_type == "histogram");
  m.is_summary = (metric_type == "summary");
  m.count = hist_count;
  m.sum = hist_sum;
  m.has_min = has_min;
  m.min = hist_min;
  m.has_max = has_max;
  m.max = hist_max;

  // 6. Stage all columns of metrics.schema.
  emitter.StartRecord();
  StageRecordColumns(lv.row, m);
  if (core::GetTableFieldRaw(L, abs_rec_idx, "exemplars")) {
    core::StageTableExemplars(lv.row, "Exemplars", L, lua_absindex(L, -1), s.writer);
    lua_pop(L, 1);
  } else {
    lv.row.BeginArray("Exemplars");
    lv.row.End();
  }
  lv.row.Str("processed_at", processed_at);
  if (!MaterializeRecordTapes(ctx, lv, m)) {
    return false;
  }
  emitter.EndRecord();
  return !emitter.HasError();
}

}  // namespace

bool ExtractNativeMetricsJson(OperationContext& ctx, core::OtelParser& parser,
                              simdjson::padded_string_view admitted) {
  NativeMetricsScratch& s = Scratch();
  s.writer.Reset();
  ReserveLevel(s.levels[0]);
  ReserveLevel(s.levels[1]);
  struct TrimGuard {
    explicit TrimGuard(NativeMetricsScratch& scratch) : s(scratch) {}
    TrimGuard(const TrimGuard&) = delete;
    TrimGuard& operator=(const TrimGuard&) = delete;
    ~TrimGuard() noexcept {
      s.writer.Trim();
      for (Level& lv : s.levels) ResetLevel(lv);
      core::ResetServiceNameScratch();
    }
    NativeMetricsScratch& s;
  } trim_guard{s};

  core::RowEmitter emitter(ctx, s.levels[0].row);

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
    if (!ExtractNativeMetricObjectJson(ctx, s, doc_obj, processed_at, 0)) {
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
      if (!ExtractNativeMetricObjectJson(ctx, s, elem_obj, processed_at, 0)) {
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

bool ExtractNativeMetricsTable(OperationContext& ctx) {
  NativeMetricsScratch& s = Scratch();
  s.writer.Reset();
  ReserveLevel(s.levels[0]);
  struct Guard {
    Guard(lua_State* l, int top, NativeMetricsScratch& scratch)
        : L(l), entry_top(top), s(scratch) {}
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    ~Guard() noexcept {
      lua_settop(L, entry_top);
      s.writer.Trim();
      ResetLevel(s.levels[0]);
    }
    lua_State* L;
    int entry_top;
    NativeMetricsScratch& s;
  } guard{ctx.L, lua_gettop(ctx.L), s};

  lua_State* L = ctx.L;
  core::RowEmitter emitter(ctx, s.levels[0].row);
  std::array<char, 32> proc_buf;
  core::FormatIso8601Nano(core::CurrentTimeUnixNano(), proc_buf.data());
  std::string_view processed_at(proc_buf.data(), 30);

  const int abs_idx = lua_absindex(L, ctx.input_index);

  auto extract_array = [&](int arr_idx, size_t len) -> bool {
    for (size_t i = 1; i <= len; ++i) {
      lua_rawgeti(L, arr_idx, static_cast<lua_Integer>(i));
      if (lua_istable(L, -1)) {
        const int item_idx = lua_absindex(L, -1);
        if (HasNativeMetricFields(L, item_idx)) {
          if (!ExtractSingleNativeMetricTable(ctx, s, item_idx, processed_at)) {
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
    if (core::GetTableFieldRaw(L, abs_idx, "metric", "log")) {
      const int inner_idx = lua_absindex(L, -1);
      if (lua_istable(L, inner_idx)) {
        const size_t inner_len = lua_rawlen(L, inner_idx);
        if (inner_len > 0) {
          handled_wrapper = true;
          if (!extract_array(inner_idx, inner_len)) {
            lua_pop(L, 1);
            return false;
          }
        } else if (HasNativeMetricFields(L, inner_idx)) {
          handled_wrapper = true;
          if (!ExtractSingleNativeMetricTable(ctx, s, inner_idx, processed_at)) {
            lua_pop(L, 1);
            return false;
          }
        }
      }
      lua_pop(L, 1);
    }
    if (!handled_wrapper && HasNativeMetricFields(L, abs_idx)) {
      if (!ExtractSingleNativeMetricTable(ctx, s, abs_idx, processed_at)) {
        return false;
      }
    }
  }

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
