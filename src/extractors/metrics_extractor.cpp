#include "vector_otel/extractors/metrics_extractor.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <limits>
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
using core::Key;
using core::OperationContext;
using core::StagedRow;

bool TryGetTableSubtable(lua_State* L, int obj_idx, core::ReadKey k1, core::ReadKey k2 = nullptr) {
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

// Reusable per-thread staging buffers: capacity is retained between invocations, so the steady
// state performs no C++ allocation per data point. The array tapes are root-array rows that are
// materialised into their anchor slots (bucket counts, bounds, quantiles) right before a data
// point is emitted.
struct MetricsScratch {
  core::FastJsonWriter attr_writer;
  StagedRow row;            // the data point being built
  StagedRow resource_tape;  // shared ResourceAttributes, once per resource
  StagedRow scope_tape;     // shared ScopeAttributes, once per scope
  StagedRow bucket_tape;    // histogram bucketCounts
  StagedRow bounds_tape;    // histogram explicitBounds
  StagedRow pos_tape;       // exponential histogram positive bucketCounts
  StagedRow neg_tape;       // exponential histogram negative bucketCounts
  StagedRow quantile_tape;  // summary quantileValues
};

MetricsScratch& Scratch() {
  static thread_local MetricsScratch scratch;
  return scratch;
}

struct TableScratch {
  std::string scope_name;
  std::string scope_version;
  std::string metric_name;
  std::string metric_desc;
  std::string metric_unit;
};

TableScratch& TScratch() {
  static thread_local TableScratch scratch;
  return scratch;
}

bool FailJson(OperationContext& ctx, simdjson::error_code code) {
  ctx.error.Set(ErrorCode::Json, simdjson::error_message(code));
  return false;
}

// ---- dual-keyed fields (camelCase + PascalCase) ----------------------------------------------

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

// Everything a data point needs, collected before the row is staged.
struct DataPoint {
  bool is_sum = false;
  bool is_histogram = false;
  bool is_exp_histogram = false;
  bool is_summary = false;
  std::int64_t agg_temporality = 0;
  std::uint64_t count = 0;
  double sum = 0.0;
  bool has_sum = false;
  double min = 0.0;
  bool has_min = false;
  double max = 0.0;
  bool has_max = false;
  std::int64_t scale = 0;
  std::uint64_t zero_count = 0;
  double zero_threshold = 0.0;
  std::int64_t pos_offset = 0;
  std::int64_t neg_offset = 0;
};

// Stages the Histogram / ExponentialHistogram / Summary map (their array members are anchors).
void StageHistogram(StagedRow& r, const DataPoint& d) {
  r.BeginMap("Histogram");
  DualInt(r, "aggregationTemporality", "AggregationTemporality", d.agg_temporality);
  DualUint(r, "count", "Count", d.count);
  DualDbl(r, "sum", "Sum", d.has_sum ? d.sum : 0.0);
  if (d.has_min) DualDbl(r, "min", "Min", d.min);
  if (d.has_max) DualDbl(r, "max", "Max", d.max);
  DualAnchor(r, "bucketCounts", "BucketCounts", AnchorSlot::kBucketCounts);
  DualAnchor(r, "explicitBounds", "ExplicitBounds", AnchorSlot::kExplicitBounds);
  r.End();
}

void StageExponentialBuckets(StagedRow& r, Key key, std::int64_t offset, AnchorSlot slot) {
  r.BeginMap(key);
  DualInt(r, "offset", "Offset", offset);
  DualAnchor(r, "bucketCounts", "BucketCounts", slot);
  r.End();
}

void StageExponentialHistogram(StagedRow& r, const DataPoint& d) {
  r.BeginMap("ExponentialHistogram");
  DualInt(r, "aggregationTemporality", "AggregationTemporality", d.agg_temporality);
  DualUint(r, "count", "Count", d.count);
  if (d.has_sum) DualDbl(r, "sum", "Sum", d.sum);
  if (d.has_min) DualDbl(r, "min", "Min", d.min);
  if (d.has_max) DualDbl(r, "max", "Max", d.max);
  DualInt(r, "scale", "Scale", d.scale);
  DualUint(r, "zeroCount", "ZeroCount", d.zero_count);
  DualDbl(r, "zeroThreshold", "ZeroThreshold", d.zero_threshold);
  StageExponentialBuckets(r, "positive", d.pos_offset, AnchorSlot::kPositiveBucketCounts);
  StageExponentialBuckets(r, "Positive", d.pos_offset, AnchorSlot::kPositiveBucketCounts);
  StageExponentialBuckets(r, "negative", d.neg_offset, AnchorSlot::kNegativeBucketCounts);
  StageExponentialBuckets(r, "Negative", d.neg_offset, AnchorSlot::kNegativeBucketCounts);
  r.End();
}

void StageSummary(StagedRow& r, const DataPoint& d) {
  r.BeginMap("Summary");
  DualUint(r, "count", "Count", d.count);
  DualDbl(r, "sum", "Sum", d.has_sum ? d.sum : 0.0);
  DualAnchor(r, "quantileValues", "QuantileValues", AnchorSlot::kQuantiles);
  r.End();
}

// Fields common to the JSON and table rows that do not depend on the data point body.
void StageMetricHeader(StagedRow& r, std::string_view name, std::string_view desc,
                       std::string_view unit, std::string_view type, std::string_view service,
                       const DataPoint& d, bool has_is_monotonic, bool is_monotonic) {
  r.Str("MetricName", name);
  r.Str("MetricDescription", desc);
  r.Str("MetricUnit", unit);
  r.Str("MetricType", type);
  r.Str("ServiceName", service);
  if (d.is_sum || d.is_histogram || d.is_exp_histogram) {
    r.Int("AggregationTemporality", d.agg_temporality);
  }
  if (d.is_sum && has_is_monotonic) {
    r.Bool("IsMonotonic", is_monotonic);
  }
}

// The array members a data point's type needs must be anchored before the row is delivered.
bool MaterializeDataPointArrays(OperationContext& ctx, MetricsScratch& s, const DataPoint& d) {
  if (d.is_histogram) {
    return MaterializeTape(ctx, s.bucket_tape, AnchorSlot::kBucketCounts) &&
           MaterializeTape(ctx, s.bounds_tape, AnchorSlot::kExplicitBounds);
  }
  if (d.is_exp_histogram) {
    return MaterializeTape(ctx, s.pos_tape, AnchorSlot::kPositiveBucketCounts) &&
           MaterializeTape(ctx, s.neg_tape, AnchorSlot::kNegativeBucketCounts);
  }
  if (d.is_summary) {
    return MaterializeTape(ctx, s.quantile_tape, AnchorSlot::kQuantiles);
  }
  return true;
}

void StageDataPointBody(StagedRow& r, const DataPoint& d) {
  if (d.is_histogram) {
    StageHistogram(r, d);
  } else if (d.is_exp_histogram) {
    StageExponentialHistogram(r, d);
  } else if (d.is_summary) {
    StageSummary(r, d);
  }
}

// ---- JSON input: array / exemplar staging ----------------------------------------------------

void StageJsonBucketCounts(StagedRow& tape, simdjson::ondemand::value& fv) {
  tape.Reset(/*root_array=*/true);
  auto arr_res = fv.get_array();
  if (arr_res.error() != simdjson::SUCCESS) return;
  for (auto b_elem : arr_res.value()) {
    if (b_elem.error() != simdjson::SUCCESS) continue;
    auto b_val = b_elem.value();
    tape.Uint(Key(), core::ParseUint64(b_val));
  }
}

void StageJsonExplicitBounds(StagedRow& tape, simdjson::ondemand::value& fv) {
  tape.Reset(/*root_array=*/true);
  auto arr_res = fv.get_array();
  if (arr_res.error() != simdjson::SUCCESS) return;
  for (auto eb_elem : arr_res.value()) {
    if (eb_elem.error() != simdjson::SUCCESS) continue;
    auto eb_val = eb_elem.value();
    tape.Dbl(Key(), core::ParseDouble(eb_val));
  }
}

void StageJsonQuantiles(StagedRow& tape, simdjson::ondemand::value& fv) {
  tape.Reset(/*root_array=*/true);
  auto q_arr_res = fv.get_array();
  if (q_arr_res.error() != simdjson::SUCCESS) return;
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
    tape.BeginMap(Key());
    DualDbl(tape, "quantile", "Quantile", q_quantile);
    DualDbl(tape, "value", "Value", q_value);
    tape.End();
  }
}

// One exemplar map: FilteredAttributes, TimeUnix, Value (when present), SpanId, TraceId.
void StageJsonExemplar(StagedRow& row, simdjson::ondemand::object& ex_obj,
                       core::FastJsonWriter& writer) {
  row.BeginMap(Key());
  std::array<char, 32> time_iso_buf;
  size_t time_iso_len = 0;
  std::string_view span_id;
  std::string_view trace_id;
  core::IdHexBuffer span_id_buf;
  core::IdHexBuffer trace_id_buf;
  enum class ExValKind { kNone, kDouble, kInt64 } ex_val_kind = ExValKind::kNone;
  double ex_double_val = 0.0;
  int64_t ex_int_val = 0;
  bool filtered_staged = false;

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
      core::StageAttributes(row, "FilteredAttributes", fv, writer);
      filtered_staged = true;
    }
  }

  if (!filtered_staged) {
    row.BeginMap("FilteredAttributes");
    row.End();
  }
  row.Str("TimeUnix", std::string_view(time_iso_buf.data(), time_iso_len));
  if (ex_val_kind == ExValKind::kDouble) {
    row.Dbl("Value", ex_double_val);
  } else if (ex_val_kind == ExValKind::kInt64) {
    row.Int("Value", ex_int_val);
  }
  row.Str("SpanId", core::NormalizeSpanId(span_id, span_id_buf));
  row.Str("TraceId", core::NormalizeTraceId(trace_id, trace_id_buf));
  row.End();
}

// Stages "Exemplars" when `val` is an array; anything else leaves the field unset.
void StageJsonExemplars(StagedRow& row, simdjson::ondemand::value& val,
                        core::FastJsonWriter& writer) {
  auto arr_res = val.get_array();
  if (arr_res.error() != simdjson::SUCCESS) return;
  row.BeginArray("Exemplars");
  for (auto elem_res : arr_res.value()) {
    if (elem_res.error() != simdjson::SUCCESS) continue;
    auto elem_val = elem_res.value();
    auto ex_obj_res = elem_val.get_object();
    if (ex_obj_res.error() != simdjson::SUCCESS) continue;
    auto ex_obj = ex_obj_res.value();
    StageJsonExemplar(row, ex_obj, writer);
  }
  row.End();
}

// Reads an exponential histogram "positive"/"negative" object: its offset and bucket counts.
void ReadJsonExpBuckets(simdjson::ondemand::value& fv, StagedRow& counts_tape,
                        std::int64_t& offset) {
  auto obj_res = fv.get_object();
  if (obj_res.error() != simdjson::SUCCESS) return;
  for (auto f_res : obj_res.value()) {
    if (f_res.error() != simdjson::SUCCESS) continue;
    auto key_res = f_res.unescaped_key();
    if (key_res.error() != simdjson::SUCCESS) continue;
    std::string_view key = key_res.value();
    auto v_res = f_res.value();
    if (v_res.error() != simdjson::SUCCESS) continue;
    auto v = v_res.value();
    if (key == "offset") {
      offset = core::ParseInt64(v);
    } else if (key == "bucketCounts" || key == "bucket_counts") {
      StageJsonBucketCounts(counts_tape, v);
    }
  }
}

}  // namespace

bool ExtractMetricsJson(OperationContext& ctx, core::OtelParser& parser,
                        simdjson::padded_string_view admitted) {
  MetricsScratch& s = Scratch();
  s.attr_writer.Reset();
  struct TrimGuard {
    explicit TrimGuard(MetricsScratch& scratch) : s(scratch) {}
    TrimGuard(const TrimGuard&) = delete;
    TrimGuard& operator=(const TrimGuard&) = delete;
    ~TrimGuard() noexcept {
      s.attr_writer.Trim();
      core::ResetServiceNameScratch();
    }
    MetricsScratch& s;
  } trim_guard{s};

  core::RowEmitter emitter(ctx, s.row);

  auto doc_res = parser.IteratePrepared(admitted);
  if (doc_res.error() != simdjson::SUCCESS) {
    return FailJson(ctx, doc_res.error());
  }
  auto doc = std::move(doc_res.value());
  auto root_obj_res = doc.get_object();
  if (root_obj_res.error() != simdjson::SUCCESS) {
    return FailJson(ctx, root_obj_res.error());
  }
  auto root_obj = root_obj_res.value();

  auto res_metrics_res = root_obj.find_field_unordered("resourceMetrics");
  if (res_metrics_res.error() == simdjson::NO_SUCH_FIELD) {
    res_metrics_res = root_obj.find_field_unordered("resource_metrics");
  }
  if (res_metrics_res.error() == simdjson::NO_SUCH_FIELD) {
    return true;
  }
  if (res_metrics_res.error() != simdjson::SUCCESS) {
    return FailJson(ctx, res_metrics_res.error());
  }
  auto res_metrics_arr_res = res_metrics_res.value().get_array();
  if (res_metrics_arr_res.error() != simdjson::SUCCESS) {
    return true;
  }

  std::array<char, 32> processed_at_buf;
  core::FormatIso8601Nano(core::CurrentTimeUnixNano(), processed_at_buf.data());
  const std::string_view processed_at(processed_at_buf.data(), 30);

  for (auto res_metric_elem : res_metrics_arr_res.value()) {
    if (res_metric_elem.error() != simdjson::SUCCESS) {
      return FailJson(ctx, res_metric_elem.error());
    }
    auto res_metric_obj_res = res_metric_elem.value().get_object();
    if (res_metric_obj_res.error() != simdjson::SUCCESS) {
      continue;
    }
    auto res_metric_obj = res_metric_obj_res.value();

    // Resource service name and attributes (staged once, materialised lazily).
    std::string_view service_name = "unknown_service";
    s.resource_tape.Reset();
    bool resource_ready = false;
    auto resource_field_res = res_metric_obj.find_field_unordered("resource");
    if (resource_field_res.error() == simdjson::SUCCESS) {
      auto res_obj_res = resource_field_res.value().get_object();
      if (res_obj_res.error() == simdjson::SUCCESS) {
        auto res_obj = res_obj_res.value();
        auto attrs_res = res_obj.find_field_unordered("attributes");
        if (attrs_res.error() == simdjson::SUCCESS) {
          auto attrs_val = attrs_res.value();
          core::StageAttributeFields(s.resource_tape, attrs_val, s.attr_writer, &service_name);
        }
      }
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
        return FailJson(ctx, scope_metric_elem.error());
      }
      auto scope_metric_obj_res = scope_metric_elem.value().get_object();
      if (scope_metric_obj_res.error() != simdjson::SUCCESS) {
        continue;
      }
      auto scope_metric_obj = scope_metric_obj_res.value();

      std::string_view scope_name;
      std::string_view scope_version;
      s.scope_tape.Reset();
      bool scope_ready = false;
      auto scope_field_res = scope_metric_obj.find_field_unordered("scope");
      if (scope_field_res.error() == simdjson::SUCCESS) {
        auto scope_obj_res = scope_field_res.value().get_object();
        if (scope_obj_res.error() == simdjson::SUCCESS) {
          auto scope_obj = scope_obj_res.value();
          scope_name = core::ExtractStringField(scope_obj, "name");
          scope_version = core::ExtractStringField(scope_obj, "version");
          auto attrs_res = scope_obj.find_field_unordered("attributes");
          if (attrs_res.error() == simdjson::SUCCESS) {
            auto attrs_val = attrs_res.value();
            core::StageAttributeFields(s.scope_tape, attrs_val, s.attr_writer);
          }
        }
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
          return FailJson(ctx, metric_elem.error());
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
            return FailJson(ctx, m_field_res.error());
          }
          auto mk_res = m_field_res.unescaped_key();
          if (mk_res.error() != simdjson::SUCCESS) {
            return FailJson(ctx, mk_res.error());
          }
          std::string_view mk = mk_res.value();
          auto mv_res = m_field_res.value();
          if (mv_res.error() != simdjson::SUCCESS) {
            return FailJson(ctx, mv_res.error());
          }
          auto mfv = mv_res.value();

          if (mk == "name") {
            auto s_res = mfv.get_string();
            if (s_res.error() == simdjson::SUCCESS) metric_name = s_res.value();
          } else if (mk == "description") {
            auto s_res = mfv.get_string();
            if (s_res.error() == simdjson::SUCCESS) metric_desc = s_res.value();
          } else if (mk == "unit") {
            auto s_res = mfv.get_string();
            if (s_res.error() == simdjson::SUCCESS) metric_unit = s_res.value();
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

        DataPoint d;
        d.is_sum = (metric_type == "sum");
        d.is_histogram = (metric_type == "histogram");
        d.is_exp_histogram = (metric_type == "exponential_histogram");
        d.is_summary = (metric_type == "summary");
        if (d.is_sum || d.is_histogram || d.is_exp_histogram) {
          d.agg_temporality = ExtractJsonAggregationTemporality(type_obj);
        }

        bool has_is_monotonic = false;
        bool is_monotonic = false;
        if (d.is_sum) {
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
            return FailJson(ctx, dp_elem.error());
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
          bool attrs_staged = false;
          d.count = 0;
          d.sum = d.min = d.max = d.zero_threshold = 0.0;
          d.has_sum = d.has_min = d.has_max = false;
          d.scale = 0;
          d.zero_count = 0;
          d.pos_offset = d.neg_offset = 0;
          s.bucket_tape.Reset(/*root_array=*/true);
          s.bounds_tape.Reset(/*root_array=*/true);
          s.pos_tape.Reset(/*root_array=*/true);
          s.neg_tape.Reset(/*root_array=*/true);
          s.quantile_tape.Reset(/*root_array=*/true);

          // The row is started first: attributes and exemplars are staged straight into it when
          // met; everything that depends on the whole data point follows the field loop.
          emitter.StartRecord();
          StageMetricHeader(s.row, metric_name, metric_desc, metric_unit, metric_type, service_name,
                            d, has_is_monotonic, is_monotonic);
          s.row.Anchor("ResourceAttributes", AnchorSlot::kResourceAttrs);
          s.row.Str("ScopeName", scope_name);
          s.row.Str("ScopeVersion", scope_version);
          s.row.Anchor("ScopeAttributes", AnchorSlot::kScopeAttrs);

          for (auto field_res : dp_obj) {
            if (field_res.error() != simdjson::SUCCESS) {
              return FailJson(ctx, field_res.error());
            }
            auto k_res = field_res.unescaped_key();
            if (k_res.error() != simdjson::SUCCESS) {
              return FailJson(ctx, k_res.error());
            }
            std::string_view k = k_res.value();
            auto val_field_res = field_res.value();
            if (val_field_res.error() != simdjson::SUCCESS) {
              return FailJson(ctx, val_field_res.error());
            }
            auto fv = val_field_res.value();
            auto fv_type_res = fv.type();
            if (fv_type_res.error() != simdjson::SUCCESS) {
              return FailJson(ctx, fv_type_res.error());
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
              if (core::TryParseDouble(fv, double_val)) val_kind = ValueKind::kDouble;
            } else if (k == "asInt" || k == "as_int") {
              if (core::TryParseInt64(fv, int_val)) val_kind = ValueKind::kInt64;
            } else if (k == "flags") {
              flags = static_cast<uint32_t>(core::ParseUint64(fv));
            } else if (k == "attributes") {
              core::StageAttributes(s.row, "Attributes", fv, s.attr_writer);
              attrs_staged = true;
            } else if (k == "count") {
              d.count = core::ParseUint64(fv);
            } else if (k == "sum") {
              if (core::TryParseDouble(fv, d.sum)) d.has_sum = true;
            } else if (k == "min") {
              if (core::TryParseDouble(fv, d.min)) d.has_min = true;
            } else if (k == "max") {
              if (core::TryParseDouble(fv, d.max)) d.has_max = true;
            } else if (k == "scale") {
              d.scale = core::ParseInt64(fv);
            } else if (k == "zeroCount" || k == "zero_count") {
              d.zero_count = core::ParseUint64(fv);
            } else if (k == "zeroThreshold" || k == "zero_threshold") {
              (void)core::TryParseDouble(fv, d.zero_threshold);
            } else if (k == "positive") {
              if (d.is_exp_histogram) ReadJsonExpBuckets(fv, s.pos_tape, d.pos_offset);
            } else if (k == "negative") {
              if (d.is_exp_histogram) ReadJsonExpBuckets(fv, s.neg_tape, d.neg_offset);
            } else if (k == "quantileValues" || k == "quantile_values") {
              if (d.is_summary) StageJsonQuantiles(s.quantile_tape, fv);
            } else if (k == "bucketCounts" || k == "bucket_counts") {
              if (d.is_histogram) StageJsonBucketCounts(s.bucket_tape, fv);
            } else if (k == "explicitBounds" || k == "explicit_bounds") {
              if (d.is_histogram) StageJsonExplicitBounds(s.bounds_tape, fv);
            } else if (k == "exemplars") {
              StageJsonExemplars(s.row, fv, s.attr_writer);
            }
          }

          if (!attrs_staged) {
            s.row.BeginMap("Attributes");
            s.row.End();
          }
          s.row.Str("StartTimeUnix", std::string_view(start_ts_buf.data(), start_ts_len));
          s.row.Str("TimeUnix", std::string_view(time_ts_buf.data(), time_ts_len));
          // Value: set for gauge/sum, omitted for histogram/exponential_histogram/summary.
          if (!d.is_histogram && !d.is_exp_histogram && !d.is_summary) {
            if (val_kind == ValueKind::kDouble) {
              s.row.Dbl("Value", double_val);
            } else if (val_kind == ValueKind::kInt64) {
              s.row.Int("Value", int_val);
            }
          }
          StageDataPointBody(s.row, d);
          s.row.Int("Flags", static_cast<int64_t>(flags));
          s.row.Str("processed_at", processed_at);

          if (!core::EnsureSharedAnchor(ctx, s.resource_tape, AnchorSlot::kResourceAttrs,
                                        resource_ready) ||
              !core::EnsureSharedAnchor(ctx, s.scope_tape, AnchorSlot::kScopeAttrs, scope_ready) ||
              !MaterializeDataPointArrays(ctx, s, d)) {
            return false;
          }
          emitter.EndRecord();
          if (emitter.HasError()) {
            return false;
          }
        }
      }
    }
  }

  emitter.Flush();
  return !emitter.HasError();
}

namespace {

// ---- table input: array staging --------------------------------------------------------------

// Normalised bucket counts: every entry becomes an integer (unparseable entries become 0).
void StageTableBucketCounts(StagedRow& tape, lua_State* L, int arr_idx) {
  tape.Reset(/*root_array=*/true);
  const int abs_idx = lua_absindex(L, arr_idx);
  if (!lua_istable(L, abs_idx)) return;
  const int top = lua_gettop(L);
  const lua_Integer len = static_cast<lua_Integer>(lua_rawlen(L, abs_idx));
  for (lua_Integer i = 1; i <= len; ++i) {
    lua_rawgeti(L, abs_idx, i);
    uint64_t u_val = 0;
    if (core::TryParseTableUint64At(L, -1, u_val)) {
      tape.Uint(Key(), u_val);
    } else {
      tape.Int(Key(), 0);
    }
    lua_settop(L, top);
  }
}

void StageTableExplicitBounds(StagedRow& tape, lua_State* L, int arr_idx) {
  tape.Reset(/*root_array=*/true);
  const int abs_idx = lua_absindex(L, arr_idx);
  if (!lua_istable(L, abs_idx)) return;
  const int top = lua_gettop(L);
  const lua_Integer len = static_cast<lua_Integer>(lua_rawlen(L, abs_idx));
  for (lua_Integer i = 1; i <= len; ++i) {
    lua_rawgeti(L, abs_idx, i);
    double d_val = 0.0;
    (void)core::TryParseTableDoubleAt(L, -1, d_val);
    tape.Dbl(Key(), d_val);
    lua_settop(L, top);
  }
}

void StageTableQuantiles(StagedRow& tape, lua_State* L, int arr_idx) {
  tape.Reset(/*root_array=*/true);
  const int abs_idx = lua_absindex(L, arr_idx);
  if (!lua_istable(L, abs_idx)) return;
  const int top = lua_gettop(L);
  const lua_Integer len = static_cast<lua_Integer>(lua_rawlen(L, abs_idx));
  for (lua_Integer i = 1; i <= len; ++i) {
    lua_rawgeti(L, abs_idx, i);
    if (lua_istable(L, -1)) {
      const int item_idx = lua_absindex(L, -1);
      const double q = core::ReadTableDouble(L, item_idx, "quantile", "Quantile", 0.0);
      const double v = core::ReadTableDouble(L, item_idx, "value", "Value", 0.0);
      tape.BeginMap(Key());
      DualDbl(tape, "quantile", "Quantile", q);
      DualDbl(tape, "value", "Value", v);
      tape.End();
    }
    lua_settop(L, top);
  }
}

// Reads an exponential histogram "positive"/"negative" table: its offset and bucket counts.
std::int64_t StageTableExpBuckets(StagedRow& counts_tape, lua_State* L, int dp_idx,
                                  core::ReadKey k1, core::ReadKey k2) {
  counts_tape.Reset(/*root_array=*/true);
  std::int64_t offset = 0;
  if (core::GetTableField(L, dp_idx, k1, k2)) {
    if (lua_istable(L, -1)) {
      const int b_obj_idx = lua_absindex(L, -1);
      offset = core::ReadTableInt64(L, b_obj_idx, "offset", "Offset", 0);
      if (core::GetTableField(L, b_obj_idx, "bucketCounts", "bucket_counts", "BucketCounts")) {
        StageTableBucketCounts(counts_tape, L, -1);
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }
  return offset;
}

// Table-mode extraction. Every Lua access here is a raw, allocation-free read: this frame is the
// dispatcher's outer frame and must never see a Lua error. Rows are staged and delivered by the
// protected emission leaves.
bool ExtractMetricsTableImpl(OperationContext& ctx, MetricsScratch& s, TableScratch& t,
                             std::string_view processed_at) {
  lua_State* L = ctx.L;
  core::RowEmitter emitter(ctx, s.row);
  const int abs_idx = lua_absindex(L, ctx.input_index);
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
    return true;
  }

  const lua_Integer rm_len = static_cast<lua_Integer>(lua_rawlen(L, rm_arr_idx));
  for (lua_Integer i = 1; i <= rm_len; ++i) {
    lua_rawgeti(L, rm_arr_idx, i);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      continue;
    }
    const int rm_obj_idx = lua_absindex(L, -1);

    std::string_view service_name = "unknown_service";
    s.resource_tape.Reset();
    bool resource_ready = false;
    if (core::GetTableField(L, rm_obj_idx, "resource")) {
      const int res_idx = lua_absindex(L, -1);
      if (core::GetTableField(L, res_idx, "attributes")) {
        core::StageTableAttributeFields(s.resource_tape, L, -1, s.attr_writer, &service_name);
        lua_pop(L, 1);
      }
      lua_pop(L, 1);
    }

    if (!core::GetTableField(L, rm_obj_idx, "scopeMetrics", "scope_metrics")) {
      lua_pop(L, 1);
      continue;
    }
    const int sm_arr_idx = lua_absindex(L, -1);
    if (!lua_istable(L, sm_arr_idx)) {
      lua_pop(L, 2);
      continue;
    }

    const lua_Integer sm_len = static_cast<lua_Integer>(lua_rawlen(L, sm_arr_idx));
    for (lua_Integer j = 1; j <= sm_len; ++j) {
      lua_rawgeti(L, sm_arr_idx, j);
      if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        continue;
      }
      const int sm_obj_idx = lua_absindex(L, -1);

      std::string_view scope_name;
      std::string_view scope_version;
      s.scope_tape.Reset();
      bool scope_ready = false;
      if (core::GetTableField(L, sm_obj_idx, "scope")) {
        const int s_idx = lua_absindex(L, -1);
        scope_name = core::ReadTableStringView(L, s_idx, "name", nullptr, t.scope_name);
        scope_version = core::ReadTableStringView(L, s_idx, "version", nullptr, t.scope_version);
        if (core::GetTableField(L, s_idx, "attributes")) {
          core::StageTableAttributeFields(s.scope_tape, L, -1, s.attr_writer);
          lua_pop(L, 1);
        }
        lua_pop(L, 1);
      }

      if (!core::GetTableField(L, sm_obj_idx, "metrics")) {
        lua_pop(L, 1);
        continue;
      }
      const int metrics_arr_idx = lua_absindex(L, -1);
      if (!lua_istable(L, metrics_arr_idx)) {
        lua_pop(L, 2);
        continue;
      }

      const lua_Integer m_len = static_cast<lua_Integer>(lua_rawlen(L, metrics_arr_idx));
      for (lua_Integer k = 1; k <= m_len; ++k) {
        lua_rawgeti(L, metrics_arr_idx, k);
        if (!lua_istable(L, -1)) {
          lua_pop(L, 1);
          continue;
        }
        const int metric_obj_idx = lua_absindex(L, -1);

        std::string_view metric_type;
        int type_obj_idx = 0;
        if (TryGetTableSubtable(L, metric_obj_idx, "gauge")) {
          metric_type = "gauge";
        } else if (TryGetTableSubtable(L, metric_obj_idx, "sum")) {
          metric_type = "sum";
        } else if (TryGetTableSubtable(L, metric_obj_idx, "histogram")) {
          metric_type = "histogram";
        } else if (TryGetTableSubtable(L, metric_obj_idx, "exponentialHistogram",
                                       "exponential_histogram")) {
          metric_type = "exponential_histogram";
        } else if (TryGetTableSubtable(L, metric_obj_idx, "summary")) {
          metric_type = "summary";
        }
        if (metric_type.empty()) {
          lua_pop(L, 1);  // pop metric_obj
          continue;
        }
        type_obj_idx = lua_absindex(L, -1);

        DataPoint d;
        d.is_sum = (metric_type == "sum");
        d.is_histogram = (metric_type == "histogram");
        d.is_exp_histogram = (metric_type == "exponential_histogram");
        d.is_summary = (metric_type == "summary");
        if (d.is_sum || d.is_histogram || d.is_exp_histogram) {
          d.agg_temporality = ReadTableAggregationTemporality(L, type_obj_idx);
        }

        bool has_is_monotonic = false;
        bool is_monotonic = false;
        if (d.is_sum && core::GetTableField(L, type_obj_idx, "isMonotonic", "is_monotonic")) {
          if (lua_isboolean(L, -1)) {
            is_monotonic = (lua_toboolean(L, -1) != 0);
            has_is_monotonic = true;
          }
          lua_pop(L, 1);
        }

        const std::string_view metric_name =
            core::ReadTableStringView(L, metric_obj_idx, "name", nullptr, t.metric_name);
        const std::string_view metric_desc =
            core::ReadTableStringView(L, metric_obj_idx, "description", nullptr, t.metric_desc);
        const std::string_view metric_unit =
            core::ReadTableStringView(L, metric_obj_idx, "unit", nullptr, t.metric_unit);

        if (core::GetTableField(L, type_obj_idx, "dataPoints", "data_points")) {
          const int dps_arr_idx = lua_absindex(L, -1);
          if (lua_istable(L, dps_arr_idx)) {
            const lua_Integer dps_len = static_cast<lua_Integer>(lua_rawlen(L, dps_arr_idx));
            for (lua_Integer dp_i = 1; dp_i <= dps_len; ++dp_i) {
              lua_rawgeti(L, dps_arr_idx, dp_i);
              if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                continue;
              }
              const int dp_obj_idx = lua_absindex(L, -1);

              const uint64_t start_nanos =
                  core::ReadTableUint64(L, dp_obj_idx, "startTimeUnixNano", "start_time_unix_nano");
              std::array<char, 32> start_ts_buf;
              size_t start_ts_len = 0;
              if (start_nanos > 0) {
                core::FormatIso8601Nano(start_nanos, start_ts_buf.data());
                start_ts_len = 30;
              }
              const uint64_t time_nanos =
                  core::ReadTableUint64(L, dp_obj_idx, "timeUnixNano", "time_unix_nano");
              std::array<char, 32> time_ts_buf;
              size_t time_ts_len = 0;
              if (time_nanos > 0) {
                core::FormatIso8601Nano(time_nanos, time_ts_buf.data());
                time_ts_len = 30;
              }

              emitter.StartRecord();
              StageMetricHeader(s.row, metric_name, metric_desc, metric_unit, metric_type,
                                service_name, d, has_is_monotonic, is_monotonic);
              s.row.Anchor("ResourceAttributes", AnchorSlot::kResourceAttrs);
              s.row.Str("ScopeName", scope_name);
              s.row.Str("ScopeVersion", scope_version);
              s.row.Anchor("ScopeAttributes", AnchorSlot::kScopeAttrs);

              if (core::GetTableField(L, dp_obj_idx, "attributes")) {
                core::StageTableAttributes(s.row, "Attributes", L, -1, s.attr_writer);
                lua_pop(L, 1);
              } else {
                s.row.BeginMap("Attributes");
                s.row.End();
              }
              s.row.Str("StartTimeUnix", std::string_view(start_ts_buf.data(), start_ts_len));
              s.row.Str("TimeUnix", std::string_view(time_ts_buf.data(), time_ts_len));

              if (!d.is_histogram && !d.is_exp_histogram && !d.is_summary) {
                bool is_int = false;
                int64_t i_val = 0;
                double d_val = 0.0;
                if (ReadTableGenericMetricValue(L, dp_obj_idx, is_int, i_val, d_val)) {
                  if (is_int) {
                    s.row.Int("Value", i_val);
                  } else {
                    s.row.Dbl("Value", d_val);
                  }
                }
              }

              // Per-type body. Shared scalar columns are read once; the arrays go to their tapes.
              d.count = 0;
              d.sum = d.min = d.max = d.zero_threshold = 0.0;
              d.has_sum = d.has_min = d.has_max = false;
              d.scale = 0;
              d.zero_count = 0;
              d.pos_offset = d.neg_offset = 0;
              if (d.is_histogram || d.is_exp_histogram || d.is_summary) {
                d.count = core::ReadTableUint64(L, dp_obj_idx, "count", "Count");
              }
              if (d.is_histogram || d.is_summary) {
                d.sum = core::ReadTableDouble(L, dp_obj_idx, "sum", "Sum", 0.0);
                d.has_sum = true;
              }
              if (d.is_histogram || d.is_exp_histogram) {
                if (d.is_exp_histogram) {
                  d.has_sum = core::TryReadTableDouble(L, dp_obj_idx, "sum", "Sum", d.sum);
                }
                d.has_min = core::TryReadTableDouble(L, dp_obj_idx, "min", "Min", d.min);
                d.has_max = core::TryReadTableDouble(L, dp_obj_idx, "max", "Max", d.max);
              }
              if (d.is_histogram) {
                if (core::GetTableField(L, dp_obj_idx, "bucketCounts", "bucket_counts",
                                        "BucketCounts")) {
                  StageTableBucketCounts(s.bucket_tape, L, -1);
                  lua_pop(L, 1);
                } else {
                  s.bucket_tape.Reset(/*root_array=*/true);
                }
                if (core::GetTableField(L, dp_obj_idx, "explicitBounds", "explicit_bounds",
                                        "ExplicitBounds")) {
                  StageTableExplicitBounds(s.bounds_tape, L, -1);
                  lua_pop(L, 1);
                } else {
                  s.bounds_tape.Reset(/*root_array=*/true);
                }
              } else if (d.is_exp_histogram) {
                d.scale = core::ReadTableInt64(L, dp_obj_idx, "scale", "Scale", 0);
                d.zero_count =
                    core::ReadTableUint64(L, dp_obj_idx, "zeroCount", "zero_count", 0, "ZeroCount");
                d.zero_threshold = core::ReadTableDouble(L, dp_obj_idx, "zeroThreshold",
                                                         "zero_threshold", 0.0, "ZeroThreshold");
                d.pos_offset =
                    StageTableExpBuckets(s.pos_tape, L, dp_obj_idx, "positive", "Positive");
                d.neg_offset =
                    StageTableExpBuckets(s.neg_tape, L, dp_obj_idx, "negative", "Negative");
              } else if (d.is_summary) {
                if (core::GetTableField(L, dp_obj_idx, "quantileValues", "quantile_values",
                                        "QuantileValues")) {
                  StageTableQuantiles(s.quantile_tape, L, -1);
                  lua_pop(L, 1);
                } else {
                  s.quantile_tape.Reset(/*root_array=*/true);
                }
              }
              StageDataPointBody(s.row, d);

              s.row.Int("Flags", static_cast<int64_t>(static_cast<uint32_t>(
                                     core::ReadTableUint64(L, dp_obj_idx, "flags", "Flags"))));

              if (core::GetTableField(L, dp_obj_idx, "exemplars", "Exemplars")) {
                core::StageTableExemplars(s.row, "Exemplars", L, -1, s.attr_writer);
                lua_pop(L, 1);
              } else {
                s.row.BeginArray("Exemplars");
                s.row.End();
              }
              s.row.Str("processed_at", processed_at);

              if (!core::EnsureSharedAnchor(ctx, s.resource_tape, AnchorSlot::kResourceAttrs,
                                            resource_ready) ||
                  !core::EnsureSharedAnchor(ctx, s.scope_tape, AnchorSlot::kScopeAttrs,
                                            scope_ready) ||
                  !MaterializeDataPointArrays(ctx, s, d)) {
                return false;
              }
              emitter.EndRecord();
              lua_pop(L, 1);  // pop dp_obj
              if (emitter.HasError()) {
                return false;
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
  return !emitter.HasError();
}

}  // namespace

bool ExtractMetricsTable(OperationContext& ctx) {
  MetricsScratch& s = Scratch();
  TableScratch& t = TScratch();
  s.attr_writer.Reset();
  core::ReserveScratchString(t.scope_name, 128);
  core::ReserveScratchString(t.scope_version, 64);
  core::ReserveScratchString(t.metric_name, 128);
  core::ReserveScratchString(t.metric_desc, 256);
  core::ReserveScratchString(t.metric_unit, 64);
  struct Guard {
    Guard(lua_State* l, int top, MetricsScratch& scratch, TableScratch& table)
        : L(l), entry_top(top), s(scratch), t(table) {}
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    ~Guard() noexcept {
      lua_settop(L, entry_top);
      s.attr_writer.Trim();
      core::ResetScratchString(t.scope_name);
      core::ResetScratchString(t.scope_version);
      core::ResetScratchString(t.metric_name);
      core::ResetScratchString(t.metric_desc);
      core::ResetScratchString(t.metric_unit);
    }
    lua_State* L;
    int entry_top;
    MetricsScratch& s;
    TableScratch& t;
  } guard{ctx.L, lua_gettop(ctx.L), s, t};

  std::array<char, 32> proc_buf;
  core::FormatIso8601Nano(core::CurrentTimeUnixNano(), proc_buf.data());
  return ExtractMetricsTableImpl(ctx, s, t, std::string_view(proc_buf.data(), 30));
}

// ---- compatibility entry points ---------------------------------------------------------------

namespace {
constexpr core::SignalExtractors kMetricsExtractors{ExtractMetricsJson, ExtractMetricsTable};
}

const core::SignalExtractors& MetricsExtractors() { return kMetricsExtractors; }

ExtractionResult ProcessMetrics(lua_State* L, std::string_view raw_json, int emit_idx) {
  return core::RunCompatJson(L, core::ModuleKind::Otel, core::Signal::Metrics, kMetricsExtractors,
                             raw_json, emit_idx);
}

ExtractionResult ProcessMetricsTable(lua_State* L, int table_idx, int emit_idx) {
  return core::RunCompatTable(L, core::ModuleKind::Otel, core::Signal::Metrics, kMetricsExtractors,
                              table_idx, emit_idx);
}

}  // namespace vector_otel::extractors
