#include "vector_otel/extractors/metrics_extractor.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
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

namespace vector_otel::extractors {
namespace {

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

// Reusable per-thread scratch: capacity is retained between invocations, so the steady state
// performs no C++ allocation per data point. Structured columns (attributes, exemplars, the
// histogram/summary payloads) are serialised to compact JSON strings in these writers and handed
// to the staged row as plain string columns.
struct MetricsScratch {
  core::FastJsonWriter res_attrs_writer;    // ResourceAttributes JSON, once per resource
  core::FastJsonWriter scope_attrs_writer;  // ScopeAttributes JSON, once per scope
  core::FastJsonWriter dp_attrs_writer;     // data point Attributes JSON
  core::FastJsonWriter exemplars_writer;    // Exemplars JSON
  core::FastJsonWriter payload_writer;      // Histogram / ExponentialHistogram / Summary JSON
  std::vector<uint64_t> bucket_counts;
  std::vector<double> explicit_bounds;
  std::vector<uint64_t> pos_buckets;
  std::vector<uint64_t> neg_buckets;
  std::vector<core::SummaryQuantile> quantiles;
  StagedRow row;  // the data point being built
};

MetricsScratch& Scratch() {
  static thread_local MetricsScratch scratch;
  return scratch;
}

// Prepares every scratch buffer for a new request.
void ResetScratch(MetricsScratch& s) {
  s.res_attrs_writer.Reset();
  s.scope_attrs_writer.Reset();
  s.dp_attrs_writer.Reset();
  s.exemplars_writer.Reset();
  s.payload_writer.Reset();
  core::ReserveScratchVector(s.bucket_counts);
  core::ReserveScratchVector(s.explicit_bounds);
  core::ReserveScratchVector(s.pos_buckets);
  core::ReserveScratchVector(s.neg_buckets);
  core::ReserveScratchVector(s.quantiles);
}

// Returns every scratch buffer to a bounded size once the request is done.
void TrimScratch(MetricsScratch& s) noexcept {
  s.res_attrs_writer.Trim();
  s.scope_attrs_writer.Trim();
  s.dp_attrs_writer.Trim();
  s.exemplars_writer.Trim();
  s.payload_writer.Trim();
  core::ResetScratchVector(s.bucket_counts);
  core::ResetScratchVector(s.explicit_bounds);
  core::ResetScratchVector(s.pos_buckets);
  core::ResetScratchVector(s.neg_buckets);
  core::ResetScratchVector(s.quantiles);
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

// Serialises the type-specific payload column of a data point (Histogram, ExponentialHistogram or
// Summary) as compact JSON; gauge and sum data points have none.
void StageDataPointBody(MetricsScratch& s, const DataPoint& d) {
  if (d.is_histogram) {
    core::SerializeHistogramJson(s.payload_writer, d.agg_temporality, d.count, d.has_sum, d.sum,
                                 d.has_min, d.min, d.has_max, d.max, s.bucket_counts,
                                 s.explicit_bounds);
    s.row.Str("Histogram", s.payload_writer.Str());
  } else if (d.is_exp_histogram) {
    core::SerializeExponentialHistogramJson(s.payload_writer, d.agg_temporality, d.count, d.has_sum,
                                            d.sum, d.has_min, d.min, d.has_max, d.max, d.scale,
                                            d.zero_count, d.zero_threshold, d.pos_offset,
                                            s.pos_buckets, d.neg_offset, s.neg_buckets);
    s.row.Str("ExponentialHistogram", s.payload_writer.Str());
  } else if (d.is_summary) {
    core::SerializeSummaryJson(s.payload_writer, d.count, d.has_sum, d.sum, s.quantiles);
    s.row.Str("Summary", s.payload_writer.Str());
  }
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
                              std::vector<core::SummaryQuantile>& out) {
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
    out.push_back(core::SummaryQuantile{q_quantile, q_value});
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

void ReadTableQuantilesVector(lua_State* L, int arr_idx, std::vector<core::SummaryQuantile>& out) {
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
      out.push_back(core::SummaryQuantile{q_val, v_val});
    }
    lua_pop(L, 1);
  }
}

// Reads an exponential histogram "positive"/"negative" object: its offset and bucket counts.
void ParseJsonExpBuckets(simdjson::ondemand::value& fv, std::int64_t& offset,
                         std::vector<uint64_t>& out) {
  offset = 0;
  out.clear();
  auto obj_res = fv.get_object();
  if (obj_res.error() == simdjson::INCORRECT_TYPE) {
    return;
  }
  if (obj_res.error() != simdjson::SUCCESS) {
    throw std::runtime_error(std::string("JSON parsing error: ") +
                             simdjson::error_message(obj_res.error()));
  }
  for (auto f_res : obj_res.value()) {
    if (f_res.error() != simdjson::SUCCESS) {
      throw std::runtime_error(std::string("JSON parsing error: ") +
                               simdjson::error_message(f_res.error()));
    }
    auto k_res = f_res.unescaped_key();
    if (k_res.error() != simdjson::SUCCESS) {
      throw std::runtime_error(std::string("JSON parsing error: ") +
                               simdjson::error_message(k_res.error()));
    }
    const std::string_view key = k_res.value();
    auto v_res = f_res.value();
    if (v_res.error() != simdjson::SUCCESS) {
      throw std::runtime_error(std::string("JSON parsing error: ") +
                               simdjson::error_message(v_res.error()));
    }
    auto v = v_res.value();
    if (key == "offset") {
      offset = core::ParseInt64(v);
    } else if (key == "bucketCounts" || key == "bucket_counts") {
      ParseJsonBucketCountsVector(v, out);
    }
  }
}

// Reads an exponential histogram "positive"/"negative" table: its offset and bucket counts.
std::int64_t ReadTableExpBuckets(lua_State* L, int dp_idx, core::ReadKey k1, core::ReadKey k2,
                                 std::vector<uint64_t>& out) {
  out.clear();
  std::int64_t offset = 0;
  if (core::GetTableField(L, dp_idx, k1, k2)) {
    if (lua_istable(L, -1)) {
      const int b_obj_idx = lua_absindex(L, -1);
      offset = core::ReadTableInt64(L, b_obj_idx, "offset", "Offset", 0);
      if (core::GetTableField(L, b_obj_idx, "bucketCounts", "bucket_counts", "BucketCounts")) {
        ReadTableBucketCountsVector(L, -1, out);
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }
  return offset;
}

}  // namespace

bool ExtractMetricsJson(OperationContext& ctx, core::OtelParser& parser,
                        simdjson::padded_string_view admitted) {
  MetricsScratch& s = Scratch();
  ResetScratch(s);
  struct TrimGuard {
    explicit TrimGuard(MetricsScratch& scratch) : s(scratch) {}
    TrimGuard(const TrimGuard&) = delete;
    TrimGuard& operator=(const TrimGuard&) = delete;
    ~TrimGuard() noexcept {
      TrimScratch(s);
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

    // Resource service name and attributes (serialised once per resource).
    std::string_view service_name = "unknown_service";
    s.res_attrs_writer.Reset();
    bool has_res_attrs = false;
    auto resource_field_res = res_metric_obj.find_field_unordered("resource");
    if (resource_field_res.error() == simdjson::SUCCESS) {
      auto res_obj_res = resource_field_res.value().get_object();
      if (res_obj_res.error() == simdjson::SUCCESS) {
        auto res_obj = res_obj_res.value();
        auto attrs_res = res_obj.find_field_unordered("attributes");
        if (attrs_res.error() == simdjson::SUCCESS) {
          auto attrs_val = attrs_res.value();
          core::WriteAttributesJson(s.res_attrs_writer, attrs_val, &service_name);
          has_res_attrs = true;
        }
      }
    }
    const std::string_view res_attrs_json = has_res_attrs ? s.res_attrs_writer.Str() : "{}";

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
      s.scope_attrs_writer.Reset();
      bool has_scope_attrs = false;
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
            core::WriteAttributesJson(s.scope_attrs_writer, attrs_val);
            has_scope_attrs = true;
          }
        }
      }
      const std::string_view scope_attrs_json = has_scope_attrs ? s.scope_attrs_writer.Str() : "{}";

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
          bool has_dp_attrs = false;
          bool has_exemplars = false;
          s.dp_attrs_writer.Reset();
          s.exemplars_writer.Reset();
          s.bucket_counts.clear();
          s.explicit_bounds.clear();
          s.pos_buckets.clear();
          s.neg_buckets.clear();
          s.quantiles.clear();
          d.count = 0;
          d.sum = d.min = d.max = d.zero_threshold = 0.0;
          d.has_sum = d.has_min = d.has_max = false;
          d.scale = 0;
          d.zero_count = 0;
          d.pos_offset = d.neg_offset = 0;

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
              s.dp_attrs_writer.Reset();
              core::WriteAttributesJson(s.dp_attrs_writer, fv);
              has_dp_attrs = true;
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
              if (d.is_exp_histogram) ParseJsonExpBuckets(fv, d.pos_offset, s.pos_buckets);
            } else if (k == "negative") {
              if (d.is_exp_histogram) ParseJsonExpBuckets(fv, d.neg_offset, s.neg_buckets);
            } else if (k == "quantileValues" || k == "quantile_values") {
              if (d.is_summary) ParseJsonQuantilesVector(fv, s.quantiles);
            } else if (k == "bucketCounts" || k == "bucket_counts") {
              if (d.is_histogram) ParseJsonBucketCountsVector(fv, s.bucket_counts);
            } else if (k == "explicitBounds" || k == "explicit_bounds") {
              if (d.is_histogram) ParseJsonExplicitBoundsVector(fv, s.explicit_bounds);
            } else if (k == "exemplars") {
              s.exemplars_writer.Reset();
              core::SerializeExemplarsJson(fv, s.exemplars_writer);
              has_exemplars = true;
            }
          }

          if (!has_dp_attrs) {
            s.dp_attrs_writer.StartObject();
            s.dp_attrs_writer.EndObject();
          }
          if (!has_exemplars) {
            s.exemplars_writer.StartArray();
            s.exemplars_writer.EndArray();
          }

          emitter.StartRecord();
          StageMetricHeader(s.row, metric_name, metric_desc, metric_unit, metric_type, service_name,
                            d, has_is_monotonic, is_monotonic);
          s.row.Str("ResourceAttributes", res_attrs_json);
          s.row.Str("ScopeName", scope_name);
          s.row.Str("ScopeVersion", scope_version);
          s.row.Str("ScopeAttributes", scope_attrs_json);
          s.row.Str("Attributes", s.dp_attrs_writer.Str());
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
          StageDataPointBody(s, d);
          s.row.Int("Flags", static_cast<int64_t>(flags));
          s.row.Str("Exemplars", s.exemplars_writer.Str());
          s.row.Str("processed_at", processed_at);
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
    s.res_attrs_writer.Reset();
    bool has_res_attrs = false;
    if (core::GetTableField(L, rm_obj_idx, "resource")) {
      const int res_idx = lua_absindex(L, -1);
      if (core::GetTableField(L, res_idx, "attributes")) {
        core::WriteTableAttributesJson(L, -1, s.res_attrs_writer, &service_name);
        has_res_attrs = true;
        lua_pop(L, 1);
      }
      lua_pop(L, 1);
    }
    const std::string_view res_attrs_json = has_res_attrs ? s.res_attrs_writer.Str() : "{}";

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
      s.scope_attrs_writer.Reset();
      bool has_scope_attrs = false;
      if (core::GetTableField(L, sm_obj_idx, "scope")) {
        const int s_idx = lua_absindex(L, -1);
        scope_name = core::ReadTableStringView(L, s_idx, "name", nullptr, t.scope_name);
        scope_version = core::ReadTableStringView(L, s_idx, "version", nullptr, t.scope_version);
        if (core::GetTableField(L, s_idx, "attributes")) {
          core::WriteTableAttributesJson(L, -1, s.scope_attrs_writer);
          has_scope_attrs = true;
          lua_pop(L, 1);
        }
        lua_pop(L, 1);
      }
      const std::string_view scope_attrs_json = has_scope_attrs ? s.scope_attrs_writer.Str() : "{}";

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

              // Per-type body. Shared scalar columns are read once; arrays go to scratch vectors.
              d.count = 0;
              d.sum = d.min = d.max = d.zero_threshold = 0.0;
              d.has_sum = d.has_min = d.has_max = false;
              d.scale = 0;
              d.zero_count = 0;
              d.pos_offset = d.neg_offset = 0;
              s.bucket_counts.clear();
              s.explicit_bounds.clear();
              s.pos_buckets.clear();
              s.neg_buckets.clear();
              s.quantiles.clear();
              if (d.is_histogram || d.is_exp_histogram || d.is_summary) {
                d.count = core::ReadTableUint64(L, dp_obj_idx, "count", "Count");
                d.has_sum = core::TryReadTableDouble(L, dp_obj_idx, "sum", "Sum", d.sum);
              }
              if (d.is_histogram || d.is_exp_histogram) {
                d.has_min = core::TryReadTableDouble(L, dp_obj_idx, "min", "Min", d.min);
                d.has_max = core::TryReadTableDouble(L, dp_obj_idx, "max", "Max", d.max);
              }
              if (d.is_histogram) {
                if (core::GetTableField(L, dp_obj_idx, "bucketCounts", "bucket_counts",
                                        "BucketCounts")) {
                  ReadTableBucketCountsVector(L, -1, s.bucket_counts);
                  lua_pop(L, 1);
                }
                if (core::GetTableField(L, dp_obj_idx, "explicitBounds", "explicit_bounds",
                                        "ExplicitBounds")) {
                  ReadTableExplicitBoundsVector(L, -1, s.explicit_bounds);
                  lua_pop(L, 1);
                }
              } else if (d.is_exp_histogram) {
                d.scale = core::ReadTableInt64(L, dp_obj_idx, "scale", "Scale", 0);
                d.zero_count =
                    core::ReadTableUint64(L, dp_obj_idx, "zeroCount", "zero_count", 0, "ZeroCount");
                d.zero_threshold = core::ReadTableDouble(L, dp_obj_idx, "zeroThreshold",
                                                         "zero_threshold", 0.0, "ZeroThreshold");
                d.pos_offset =
                    ReadTableExpBuckets(L, dp_obj_idx, "positive", "Positive", s.pos_buckets);
                d.neg_offset =
                    ReadTableExpBuckets(L, dp_obj_idx, "negative", "Negative", s.neg_buckets);
              } else if (d.is_summary) {
                if (core::GetTableField(L, dp_obj_idx, "quantileValues", "quantile_values",
                                        "QuantileValues")) {
                  ReadTableQuantilesVector(L, -1, s.quantiles);
                  lua_pop(L, 1);
                }
              }

              s.dp_attrs_writer.Reset();
              if (core::GetTableField(L, dp_obj_idx, "attributes")) {
                core::WriteTableAttributesJson(L, -1, s.dp_attrs_writer);
                lua_pop(L, 1);
              } else {
                s.dp_attrs_writer.StartObject();
                s.dp_attrs_writer.EndObject();
              }
              s.exemplars_writer.Reset();
              if (core::GetTableField(L, dp_obj_idx, "exemplars", "Exemplars")) {
                core::SerializeTableExemplarsToJson(L, -1, s.exemplars_writer);
                lua_pop(L, 1);
              } else {
                s.exemplars_writer.StartArray();
                s.exemplars_writer.EndArray();
              }

              emitter.StartRecord();
              StageMetricHeader(s.row, metric_name, metric_desc, metric_unit, metric_type,
                                service_name, d, has_is_monotonic, is_monotonic);
              s.row.Str("ResourceAttributes", res_attrs_json);
              s.row.Str("ScopeName", scope_name);
              s.row.Str("ScopeVersion", scope_version);
              s.row.Str("ScopeAttributes", scope_attrs_json);
              s.row.Str("Attributes", s.dp_attrs_writer.Str());
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
              StageDataPointBody(s, d);
              s.row.Int("Flags", static_cast<int64_t>(static_cast<uint32_t>(
                                     core::ReadTableUint64(L, dp_obj_idx, "flags", "Flags"))));
              s.row.Str("Exemplars", s.exemplars_writer.Str());
              s.row.Str("processed_at", processed_at);
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
  ResetScratch(s);
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
      TrimScratch(s);
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
