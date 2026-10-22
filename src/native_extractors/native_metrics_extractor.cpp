#include "vector_otel/native_extractors/native_metrics_extractor.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
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

constexpr uint64_t kMaxLuaInt = static_cast<uint64_t>(std::numeric_limits<lua_Integer>::max());

std::string_view TrimJsonWhitespace(std::string_view sv) noexcept {
  while (!sv.empty() &&
         (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() &&
         (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
    sv.remove_suffix(1);
  }
  return sv;
}

bool EqualsIgnoreCaseAscii(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'a' && ca <= 'z') {
      ca = static_cast<char>(ca - 'a' + 'A');
    }
    if (cb >= 'a' && cb <= 'z') {
      cb = static_cast<char>(cb - 'a' + 'A');
    }
    if (ca != cb) {
      return false;
    }
  }
  return true;
}

bool IsInfinityString(std::string_view sv) noexcept {
  sv = TrimJsonWhitespace(sv);
  return EqualsIgnoreCaseAscii(sv, "inf") || EqualsIgnoreCaseAscii(sv, "+inf") ||
         EqualsIgnoreCaseAscii(sv, "infinity") || EqualsIgnoreCaseAscii(sv, "+infinity");
}

std::string UnsupportedMetricVariantError(std::string_view variant) {
  std::string msg = "Unsupported non-OTLP metric variant '";
  msg.append(variant);
  msg.append(
      "': vector_native only supports OpenTelemetry metric types (counter, gauge, "
      "aggregated_histogram, aggregated_summary)");
  return msg;
}

bool ParseJsonTimestamp(simdjson::ondemand::value& fv, uint64_t& out_ns) noexcept {
  auto type_res = fv.type();
  if (type_res.error() != simdjson::SUCCESS) {
    return false;
  }
  const auto type = type_res.value();
  if (type == simdjson::ondemand::json_type::string) {
    auto s_res = fv.get_string();
    if (s_res.error() != simdjson::SUCCESS) {
      return false;
    }
    std::string_view sv = s_res.value();
    if (core::ParseIso8601ToUnixNano(sv, out_ns)) {
      return true;
    }
    sv = TrimJsonWhitespace(sv);
    if (sv.empty()) {
      return false;
    }
    uint64_t uval = 0;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), uval);
    if (ec == std::errc() && ptr == sv.data() + sv.size()) {
      out_ns = uval;
      return true;
    }
    return false;
  }
  if (type == simdjson::ondemand::json_type::number) {
    auto num_res = fv.get_number();
    if (num_res.error() != simdjson::SUCCESS) {
      return false;
    }
    const auto num = num_res.value();
    if (num.is_uint64()) {
      out_ns = num.get_uint64();
      return true;
    }
    if (num.is_int64()) {
      const int64_t iv = num.get_int64();
      if (iv < 0) {
        return false;
      }
      out_ns = static_cast<uint64_t>(iv);
      return true;
    }
    if (num.is_double()) {
      const double d = num.get_double();
      if (std::isfinite(d) && d >= 0.0 && d < 18446744073709551616.0) {
        out_ns = static_cast<uint64_t>(d);
        return true;
      }
    }
    return false;
  }
  if (type == simdjson::ondemand::json_type::object) {
    auto obj_res = fv.get_object();
    if (obj_res.error() != simdjson::SUCCESS) {
      return false;
    }
    int64_t year = 0;
    int64_t month = 0;
    int64_t day = 0;
    int64_t hour = 0;
    int64_t min = 0;
    int64_t sec = 0;
    int64_t nanosec = 0;
    bool has_year = false;
    bool has_month = false;
    bool has_day = false;
    for (auto field_res : obj_res.value()) {
      if (field_res.error() != simdjson::SUCCESS) continue;
      auto k_res = field_res.unescaped_key();
      if (k_res.error() != simdjson::SUCCESS) continue;
      std::string_view k = k_res.value();
      auto v_res = field_res.value();
      if (v_res.error() != simdjson::SUCCESS) continue;
      auto v = v_res.value();
      if (k == "year") {
        year = core::ParseInt64(v);
        has_year = true;
      } else if (k == "month") {
        month = core::ParseInt64(v);
        has_month = true;
      } else if (k == "day") {
        day = core::ParseInt64(v);
        has_day = true;
      } else if (k == "hour") {
        hour = core::ParseInt64(v);
      } else if (k == "min") {
        min = core::ParseInt64(v);
      } else if (k == "sec") {
        sec = core::ParseInt64(v);
      } else if (k == "nanosec" || k == "nanosecond" || k == "nsec") {
        nanosec = core::ParseInt64(v);
      }
    }
    if (!has_year || !has_month || !has_day) {
      return false;
    }
    return core::CivilToUnixNano(year, month, day, hour, min, sec, nanosec, out_ns);
  }
  return false;
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
    std::string_view sv = TrimJsonWhitespace(s_res.value());
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
    std::string_view sv = TrimJsonWhitespace(std::string_view(s, len));
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
    std::string_view sv = TrimJsonWhitespace(s_res.value());
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

void PushHistogramTableToStack(lua_State* L, uint64_t hist_count, double hist_sum, bool has_min,
                               double hist_min, bool has_max, double hist_max,
                               const std::vector<int64_t>& bucket_counts,
                               const std::vector<double>& explicit_bounds) {
  lua_newtable(L);
  const lua_Integer clamped_hist_count = static_cast<lua_Integer>(std::min(hist_count, kMaxLuaInt));
  lua_pushinteger(L, clamped_hist_count);
  lua_setfield(L, -2, "count");
  lua_pushinteger(L, clamped_hist_count);
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

  lua_createtable(L, static_cast<int>(bucket_counts.size()), 0);
  for (size_t bi = 0; bi < bucket_counts.size(); ++bi) {
    lua_pushinteger(L, static_cast<lua_Integer>(bucket_counts[bi]));
    lua_rawseti(L, -2, static_cast<lua_Integer>(bi + 1));
  }
  lua_pushvalue(L, -1);
  lua_setfield(L, -3, "BucketCounts");
  lua_setfield(L, -2, "bucketCounts");

  lua_createtable(L, static_cast<int>(explicit_bounds.size()), 0);
  for (size_t bi = 0; bi < explicit_bounds.size(); ++bi) {
    lua_pushnumber(L, explicit_bounds[bi]);
    lua_rawseti(L, -2, static_cast<lua_Integer>(bi + 1));
  }
  lua_pushvalue(L, -1);
  lua_setfield(L, -3, "ExplicitBounds");
  lua_setfield(L, -2, "explicitBounds");
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
      writer.RawJson(TrimJsonWhitespace(raw_res.value()));
      out.kind = JsonTagValue::Kind::kString;
      out.str_val = writer.Str();
    }
  }
}

void PushJsonTagValueToLuaTable(lua_State* L, int table_abs_idx, std::string_view key,
                                const JsonTagValue& tv) {
  if (L == nullptr || tv.kind == JsonTagValue::Kind::kNone) {
    return;
  }
  lua_pushlstring(L, key.data(), key.size());
  switch (tv.kind) {
    case JsonTagValue::Kind::kString:
      lua_pushlstring(L, tv.str_val.data(), tv.str_val.size());
      break;
    case JsonTagValue::Kind::kInt64:
      lua_pushinteger(L, static_cast<lua_Integer>(tv.int_val));
      break;
    case JsonTagValue::Kind::kUint64:
      lua_pushinteger(L, static_cast<lua_Integer>(std::min(tv.uint_val, kMaxLuaInt)));
      break;
    case JsonTagValue::Kind::kDouble:
      lua_pushnumber(L, tv.dbl_val);
      break;
    case JsonTagValue::Kind::kBool:
      lua_pushboolean(L, tv.bool_val ? 1 : 0);
      break;
    case JsonTagValue::Kind::kNone:
      lua_pop(L, 1);
      return;
  }
  lua_rawset(L, table_abs_idx);
}

void CopyLuaTableInto(lua_State* L, int src_idx, int dst_idx) {
  int abs_src = lua_absindex(L, src_idx);
  int abs_dst = lua_absindex(L, dst_idx);
  if (!lua_istable(L, abs_src) || !lua_istable(L, abs_dst)) {
    return;
  }
  lua_pushnil(L);
  while (lua_next(L, abs_src) != 0) {
    lua_pushvalue(L, -2);
    lua_pushvalue(L, -2);
    lua_rawset(L, abs_dst);
    lua_pop(L, 1);
  }
}

bool ExtractNativeMetricObjectJson(
    lua_State* L, simdjson::ondemand::object& rec_obj, std::string_view processed_at,
    core::FastJsonWriter& writer, std::string& tls_metric_name_scratch,
    std::string& tls_service_scratch, std::string& tls_scope_name_scratch,
    std::string& tls_scope_ver_scratch, std::string& tls_metric_desc_scratch,
    std::string& tls_metric_unit_scratch, std::vector<int64_t>& tls_bucket_counts,
    std::vector<double>& tls_explicit_bounds, core::LuaEmitter& emitter, std::string& err_out,
    int depth = 0) {
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

  uint64_t hist_count = 0;
  double hist_sum = 0.0;
  bool has_min = false;
  double hist_min = 0.0;
  bool has_max = false;
  double hist_max = 0.0;
  tls_bucket_counts.clear();
  tls_explicit_bounds.clear();

  ScopedLuaRef res_attrs_scoped(L, LUA_NOREF);
  ScopedLuaRef scope_attrs_scoped(L, LUA_NOREF);
  ScopedLuaRef dp_attrs_scoped(L, LUA_NOREF);

  auto ensure_scoped_table = [&](ScopedLuaRef& ref) {
    if (L != nullptr && ref.get() == LUA_NOREF) {
      lua_newtable(L);
      ref = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
    }
  };

  bool has_recognized_field = false;

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

    if ((k == "metric" || k == "log") && depth == 0) {
      auto t_res = fv.type();
      if (t_res.error() == simdjson::SUCCESS) {
        if (t_res.value() == simdjson::ondemand::json_type::object) {
          auto inner_obj_res = fv.get_object();
          if (inner_obj_res.error() == simdjson::SUCCESS) {
            auto inner_obj = inner_obj_res.value();
            return ExtractNativeMetricObjectJson(
                L, inner_obj, processed_at, writer, tls_metric_name_scratch, tls_service_scratch,
                tls_scope_name_scratch, tls_scope_ver_scratch, tls_metric_desc_scratch,
                tls_metric_unit_scratch, tls_bucket_counts, tls_explicit_bounds, emitter, err_out,
                depth + 1);
          }
        } else if (t_res.value() == simdjson::ondemand::json_type::array) {
          auto inner_arr_res = fv.get_array();
          if (inner_arr_res.error() == simdjson::SUCCESS) {
            for (auto elem_res : inner_arr_res.value()) {
              if (elem_res.error() != simdjson::SUCCESS) {
                err_out = std::string(simdjson::error_message(elem_res.error()));
                return false;
              }
              auto elem_obj_res = elem_res.value().get_object();
              if (elem_obj_res.error() == simdjson::SUCCESS) {
                auto elem_obj = elem_obj_res.value();
                if (!ExtractNativeMetricObjectJson(
                        L, elem_obj, processed_at, writer, tls_metric_name_scratch,
                        tls_service_scratch, tls_scope_name_scratch, tls_scope_ver_scratch,
                        tls_metric_desc_scratch, tls_metric_unit_scratch, tls_bucket_counts,
                        tls_explicit_bounds, emitter, err_out, depth + 1)) {
                  return false;
                }
              }
            }
            return true;
          }
        }
      }
      continue;
    }

    if (k == "distribution" || k == "set" || k == "sketch") {
      auto t_res = fv.type();
      if (t_res.error() == simdjson::SUCCESS &&
          t_res.value() != simdjson::ondemand::json_type::null) {
        err_out = UnsupportedMetricVariantError(k);
        return false;
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
      if (ParseJsonTimestamp(fv, parsed)) {
        t_ns = parsed;
        has_ts = true;
      }
    } else if (k == "start_time_unix_nano" || k == "startTimeUnixNano" || k == "start_timestamp") {
      has_recognized_field = true;
      uint64_t parsed = 0;
      if (ParseJsonTimestamp(fv, parsed)) {
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
    } else if (k == "kind") {
      has_recognized_field = true;
    } else if (k == "counter" || k == "sum") {
      has_recognized_field = true;
      metric_type = "sum";
      ParseJsonCounterOrGauge(fv, val_kind, double_val, int_val);
    } else if (k == "gauge") {
      has_recognized_field = true;
      metric_type = "gauge";
      ParseJsonCounterOrGauge(fv, val_kind, double_val, int_val);
    } else if (k == "aggregated_histogram" || k == "histogram") {
      has_recognized_field = true;
      metric_type = "histogram";
      ParseJsonHistogram(fv, hist_count, hist_sum, has_min, hist_min, has_max, hist_max,
                         tls_bucket_counts, tls_explicit_bounds);
    } else if (k == "aggregated_summary" || k == "summary") {
      has_recognized_field = true;
      metric_type = "summary";
      auto obj_res = fv.get_object();
      if (obj_res.error() == simdjson::SUCCESS) {
        for (auto sf_res : obj_res.value()) {
          if (sf_res.error() != simdjson::SUCCESS) continue;
        }
      }
    } else if (k == "tags") {
      has_recognized_field = true;
      auto tags_obj_res = fv.get_object();
      if (tags_obj_res.error() == simdjson::SUCCESS) {
        int res_tbl_idx = 0;
        int scope_tbl_idx = 0;
        int dp_tbl_idx = 0;
        if (L != nullptr) {
          ensure_scoped_table(res_attrs_scoped);
          ensure_scoped_table(scope_attrs_scoped);
          ensure_scoped_table(dp_attrs_scoped);
          lua_rawgeti(L, LUA_REGISTRYINDEX, res_attrs_scoped.get());
          res_tbl_idx = lua_absindex(L, -1);
          lua_rawgeti(L, LUA_REGISTRYINDEX, scope_attrs_scoped.get());
          scope_tbl_idx = lua_absindex(L, -1);
          lua_rawgeti(L, LUA_REGISTRYINDEX, dp_attrs_scoped.get());
          dp_tbl_idx = lua_absindex(L, -1);
        }

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
                ReadSingleJsonTagElement(elem_v, writer, tag_val);
              }
            }
          } else {
            ReadSingleJsonTagElement(tv, writer, tag_val);
          }

          if (tag_val.kind == JsonTagValue::Kind::kNone) {
            continue;
          }

          if (tk.size() > 9 && tk.substr(0, 9) == "resource.") {
            std::string_view sub_key = tk.substr(9);
            if ((sub_key == "service.name" || sub_key == "service_name") &&
                tag_val.kind == JsonTagValue::Kind::kString && !tag_val.str_val.empty()) {
              tls_service_scratch.assign(tag_val.str_val);
              service_name = std::string_view(tls_service_scratch);
            }
            PushJsonTagValueToLuaTable(L, res_tbl_idx, sub_key, tag_val);
          } else if (tk.size() > 6 && tk.substr(0, 6) == "scope.") {
            std::string_view sub_key = tk.substr(6);
            if (sub_key == "name") {
              if (tag_val.kind == JsonTagValue::Kind::kString) {
                tls_scope_name_scratch.assign(tag_val.str_val);
                scope_name = std::string_view(tls_scope_name_scratch);
              }
            } else if (sub_key == "version") {
              if (tag_val.kind == JsonTagValue::Kind::kString) {
                tls_scope_ver_scratch.assign(tag_val.str_val);
                scope_version = std::string_view(tls_scope_ver_scratch);
              }
            } else {
              PushJsonTagValueToLuaTable(L, scope_tbl_idx, sub_key, tag_val);
            }
          } else if (tk == "otel.unit" || tk == "metric.unit") {
            if (tag_val.kind == JsonTagValue::Kind::kString && metric_unit.empty()) {
              tls_metric_unit_scratch.assign(tag_val.str_val);
              metric_unit = std::string_view(tls_metric_unit_scratch);
            }
          } else if (tk == "otel.description" || tk == "metric.description") {
            if (tag_val.kind == JsonTagValue::Kind::kString && metric_desc.empty()) {
              tls_metric_desc_scratch.assign(tag_val.str_val);
              metric_desc = std::string_view(tls_metric_desc_scratch);
            }
          } else if (tk == "otel.scope.name") {
            if (tag_val.kind == JsonTagValue::Kind::kString && scope_name.empty()) {
              tls_scope_name_scratch.assign(tag_val.str_val);
              scope_name = std::string_view(tls_scope_name_scratch);
            }
          } else if (tk == "otel.scope.version") {
            if (tag_val.kind == JsonTagValue::Kind::kString && scope_version.empty()) {
              tls_scope_ver_scratch.assign(tag_val.str_val);
              scope_version = std::string_view(tls_scope_ver_scratch);
            }
          } else {
            PushJsonTagValueToLuaTable(L, dp_tbl_idx, tk, tag_val);
            if ((tk == "service.name" || tk == "service_name") && service_name.empty() &&
                tag_val.kind == JsonTagValue::Kind::kString && !tag_val.str_val.empty()) {
              tls_service_scratch.assign(tag_val.str_val);
              service_name = std::string_view(tls_service_scratch);
            }
          }
        }

        if (L != nullptr) {
          lua_pop(L, 3);
        }
      }
    } else if (k == "resources" || k == "resource_attributes" || k == "resourceAttributes" ||
               k == "resource") {
      has_recognized_field = true;
      if (L != nullptr) {
        std::string_view extracted_svc = "unknown_service";
        core::PushAttributesTable(L, fv, writer, &extracted_svc);
        if (extracted_svc != "unknown_service" && !extracted_svc.empty()) {
          tls_service_scratch.assign(extracted_svc);
          service_name = std::string_view(tls_service_scratch);
        }
        if (res_attrs_scoped.get() == LUA_NOREF) {
          res_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
        } else {
          lua_rawgeti(L, LUA_REGISTRYINDEX, res_attrs_scoped.get());
          CopyLuaTableInto(L, -2, -1);
          lua_pop(L, 2);
        }
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
              tls_scope_name_scratch.assign(s_res.value());
              scope_name = std::string_view(tls_scope_name_scratch);
            }
          } else if (sk == "version") {
            auto s_res = sfv.get_string();
            if (s_res.error() == simdjson::SUCCESS) {
              tls_scope_ver_scratch.assign(s_res.value());
              scope_version = std::string_view(tls_scope_ver_scratch);
            }
          } else if (sk == "attributes" && L != nullptr) {
            core::PushAttributesTable(L, sfv, writer);
            if (scope_attrs_scoped.get() == LUA_NOREF) {
              scope_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
            } else {
              lua_rawgeti(L, LUA_REGISTRYINDEX, scope_attrs_scoped.get());
              CopyLuaTableInto(L, -2, -1);
              lua_pop(L, 2);
            }
          }
        }
      }
    } else if (k == "attributes") {
      has_recognized_field = true;
      if (L != nullptr) {
        core::PushAttributesTable(L, fv, writer);
        if (dp_attrs_scoped.get() == LUA_NOREF) {
          dp_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
        } else {
          lua_rawgeti(L, LUA_REGISTRYINDEX, dp_attrs_scoped.get());
          CopyLuaTableInto(L, -2, -1);
          lua_pop(L, 2);
        }
      }
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

  std::string_view metric_name = raw_name;
  if (!metric_ns.empty() && !raw_name.empty()) {
    tls_metric_name_scratch.clear();
    tls_metric_name_scratch.reserve(metric_ns.size() + 1 + raw_name.size());
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

  const bool is_histogram = (metric_type == "histogram");
  const bool is_summary = (metric_type == "summary");

  emitter.StartRecord();
  emitter.SetString("MetricName", metric_name);
  emitter.SetString("MetricDescription", metric_desc);
  emitter.SetString("MetricUnit", metric_unit);
  emitter.SetString("MetricType", metric_type);
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
    if (scope_attrs_scoped.get() != LUA_NOREF) {
      lua_rawgeti(L, LUA_REGISTRYINDEX, scope_attrs_scoped.get());
      scope_attrs_scoped.reset();
    } else {
      lua_newtable(L);
    }
    emitter.SetTableFromStack("ScopeAttributes");
  }

  if (L != nullptr) {
    if (dp_attrs_scoped.get() != LUA_NOREF) {
      lua_rawgeti(L, LUA_REGISTRYINDEX, dp_attrs_scoped.get());
      dp_attrs_scoped.reset();
    } else {
      lua_newtable(L);
    }
    emitter.SetTableFromStack("Attributes");
  }

  emitter.SetString("StartTimeUnix", start_time_sv);
  emitter.SetString("TimeUnix", time_unix_sv);

  if (!is_histogram && !is_summary) {
    if (val_kind == MetricValueKind::kDouble) {
      emitter.SetDouble("Value", double_val);
    } else if (val_kind == MetricValueKind::kInt64) {
      emitter.SetInt64("Value", int_val);
    }
  }

  if (is_histogram && L != nullptr) {
    PushHistogramTableToStack(L, hist_count, hist_sum, has_min, hist_min, has_max, hist_max,
                              tls_bucket_counts, tls_explicit_bounds);
    emitter.SetTableFromStack("Histogram");
  }

  emitter.SetInt64("Flags", flags);

  if (L != nullptr) {
    lua_newtable(L);
    emitter.SetTableFromStack("Exemplars");
  }

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
  if (!lua_istable(L, abs_idx)) {
    return false;
  }
  static constexpr const char* kKeys[] = {
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
  for (const char* k : kKeys) {
    if (core::GetTableFieldRaw(L, abs_idx, k)) {
      lua_pop(L, 1);
      return true;
    }
  }
  return false;
}

std::string_view ReadLuaScalarStringView(lua_State* L, int val_idx, std::string& scratch) {
  int abs_idx = lua_absindex(L, val_idx);
  int t = lua_type(L, abs_idx);
  if (t == LUA_TSTRING) {
    size_t len = 0;
    const char* s = lua_tolstring(L, abs_idx, &len);
    if (s != nullptr) {
      scratch.assign(s, len);
      return std::string_view(scratch);
    }
  } else if (t == LUA_TNUMBER) {
    lua_pushvalue(L, abs_idx);
    size_t len = 0;
    const char* s = lua_tolstring(L, -1, &len);
    if (s != nullptr) {
      scratch.assign(s, len);
    } else {
      scratch.clear();
    }
    lua_pop(L, 1);
    return std::string_view(scratch);
  }
  return "";
}

void PushLuaTagScalarOrJson(lua_State* L, int val_idx, core::FastJsonWriter& writer) {
  int abs_idx = lua_absindex(L, val_idx);
  int t = lua_type(L, abs_idx);
  if (t == LUA_TSTRING || t == LUA_TNUMBER || t == LUA_TBOOLEAN) {
    lua_pushvalue(L, abs_idx);
    return;
  }
  if (t == LUA_TTABLE) {
    writer.Reset();
    core::SerializeLuaValueToJson(L, abs_idx, writer);
    std::string_view sv = writer.Str();
    lua_pushlstring(L, sv.data(), sv.size());
    return;
  }
  lua_pushliteral(L, "");
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
        std::string_view sv = TrimJsonWhitespace(std::string_view(s, len));
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
              std::string_view sv = TrimJsonWhitespace(std::string_view(s, slen));
              std::from_chars(sv.data(), sv.data() + sv.size(), bc);
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

bool ExtractSingleNativeMetricTable(
    lua_State* L, int rec_idx, std::string_view processed_at, core::FastJsonWriter& writer,
    std::string& tls_metric_name_scratch, std::string& tls_service_scratch,
    std::string& tls_scope_name_scratch, std::string& tls_scope_ver_scratch,
    std::string& tls_metric_desc_scratch, std::string& tls_metric_unit_scratch,
    std::vector<int64_t>& tls_bucket_counts, std::vector<double>& tls_explicit_bounds,
    core::LuaEmitter& emitter, std::string& err_out) {
  int abs_rec_idx = lua_absindex(L, rec_idx);

  // 1. Check for unsupported non-OTLP metric variants first
  static constexpr const char* kUnsupportedVariants[] = {"distribution", "set", "sketch"};
  for (const char* v : kUnsupportedVariants) {
    if (core::GetTableFieldRaw(L, abs_rec_idx, v)) {
      lua_pop(L, 1);
      err_out = UnsupportedMetricVariantError(v);
      return false;
    }
  }

  // 2. Detect supported OTLP metric variant
  std::string_view metric_type;
  MetricValueKind val_kind = MetricValueKind::kNone;
  double double_val = 0.0;
  int64_t int_val = 0;
  uint64_t hist_count = 0;
  double hist_sum = 0.0;
  bool has_min = false;
  double hist_min = 0.0;
  bool has_max = false;
  double hist_max = 0.0;

  if (core::GetTableFieldRaw(L, abs_rec_idx, "counter", "sum")) {
    metric_type = "sum";
    ReadLuaMetricValue(L, -1, val_kind, double_val, int_val);
    lua_pop(L, 1);
  } else if (core::GetTableFieldRaw(L, abs_rec_idx, "gauge")) {
    metric_type = "gauge";
    ReadLuaMetricValue(L, -1, val_kind, double_val, int_val);
    lua_pop(L, 1);
  } else if (core::GetTableFieldRaw(L, abs_rec_idx, "aggregated_histogram", "histogram")) {
    metric_type = "histogram";
    ReadLuaHistogram(L, -1, hist_count, hist_sum, has_min, hist_min, has_max, hist_max,
                     tls_bucket_counts, tls_explicit_bounds);
    lua_pop(L, 1);
  } else if (core::GetTableFieldRaw(L, abs_rec_idx, "aggregated_summary", "summary")) {
    metric_type = "summary";
    lua_pop(L, 1);
  }

  if (metric_type.empty()) {
    return true;
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
      tls_metric_name_scratch.reserve(metric_ns.size() + 1 + raw_name.size());
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

  // 5. Optional resources / scope / attributes tables + single-pass tags splitting
  std::string_view service_name;
  std::string_view scope_name =
      core::ReadTableStringView(L, abs_rec_idx, "scope_name", "scopeName", tls_scope_name_scratch);
  std::string_view scope_version = core::ReadTableStringView(L, abs_rec_idx, "scope_version",
                                                             "scopeVersion", tls_scope_ver_scratch);

  ScopedLuaRef res_attrs_scoped(L, LUA_NOREF);
  ScopedLuaRef scope_attrs_scoped(L, LUA_NOREF);
  ScopedLuaRef dp_attrs_scoped(L, LUA_NOREF);

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
      core::PushTableAttributes(L, attrs_src_idx, writer);
      int pushed_attrs_idx = lua_absindex(L, -1);
      service_name = core::ReadTableStringView(L, pushed_attrs_idx, "service.name", "service_name",
                                               tls_service_scratch);
      res_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
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
        core::PushTableAttributes(L, -1, writer);
        scope_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }

  if (core::GetTableFieldRaw(L, abs_rec_idx, "attributes")) {
    if (lua_istable(L, -1)) {
      core::PushTableAttributes(L, -1, writer);
      dp_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
    }
    lua_pop(L, 1);
  }

  if (res_attrs_scoped.get() == LUA_NOREF) {
    lua_newtable(L);
    res_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
  }
  if (scope_attrs_scoped.get() == LUA_NOREF) {
    lua_newtable(L);
    scope_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
  }
  if (dp_attrs_scoped.get() == LUA_NOREF) {
    lua_newtable(L);
    dp_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
  }

  // Single-pass tags partitioning
  if (core::GetTableFieldRaw(L, abs_rec_idx, "tags")) {
    int tags_idx = lua_absindex(L, -1);
    if (lua_istable(L, tags_idx)) {
      lua_rawgeti(L, LUA_REGISTRYINDEX, res_attrs_scoped.get());
      int res_tbl_idx = lua_absindex(L, -1);
      lua_rawgeti(L, LUA_REGISTRYINDEX, scope_attrs_scoped.get());
      int scope_tbl_idx = lua_absindex(L, -1);
      lua_rawgeti(L, LUA_REGISTRYINDEX, dp_attrs_scoped.get());
      int dp_tbl_idx = lua_absindex(L, -1);

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
            lua_pushlstring(L, sub_key.data(), sub_key.size());
            PushLuaTagScalarOrJson(L, val_idx, writer);
            lua_rawset(L, res_tbl_idx);
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
              lua_pushlstring(L, sub_key.data(), sub_key.size());
              PushLuaTagScalarOrJson(L, val_idx, writer);
              lua_rawset(L, scope_tbl_idx);
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
            lua_pushlstring(L, k.data(), k.size());
            PushLuaTagScalarOrJson(L, val_idx, writer);
            lua_rawset(L, dp_tbl_idx);
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

      lua_pop(L, 3);
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

  int64_t flags = core::ReadTableInt64(L, abs_rec_idx, "flags", nullptr, 0);
  const bool is_histogram = (metric_type == "histogram");
  const bool is_summary = (metric_type == "summary");

  // 6. Emit all columns of metrics.schema
  emitter.StartRecord();
  emitter.SetString("MetricName", metric_name);
  emitter.SetString("MetricDescription", metric_desc);
  emitter.SetString("MetricUnit", metric_unit);
  emitter.SetString("MetricType", metric_type);
  emitter.SetString("ServiceName", service_name);

  if (res_attrs_scoped.get() != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, res_attrs_scoped.get());
    res_attrs_scoped.reset();
    emitter.SetTableFromStack("ResourceAttributes");
  }

  emitter.SetString("ScopeName", scope_name);
  emitter.SetString("ScopeVersion", scope_version);

  if (scope_attrs_scoped.get() != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, scope_attrs_scoped.get());
    scope_attrs_scoped.reset();
    emitter.SetTableFromStack("ScopeAttributes");
  }

  if (dp_attrs_scoped.get() != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, dp_attrs_scoped.get());
    dp_attrs_scoped.reset();
    emitter.SetTableFromStack("Attributes");
  }

  emitter.SetString("StartTimeUnix", start_time_sv);
  emitter.SetString("TimeUnix", time_unix_sv);

  if (!is_histogram && !is_summary) {
    if (val_kind == MetricValueKind::kDouble) {
      emitter.SetDouble("Value", double_val);
    } else if (val_kind == MetricValueKind::kInt64) {
      emitter.SetInt64("Value", int_val);
    }
  }

  if (is_histogram) {
    PushHistogramTableToStack(L, hist_count, hist_sum, has_min, hist_min, has_max, hist_max,
                              tls_bucket_counts, tls_explicit_bounds);
    emitter.SetTableFromStack("Histogram");
  }

  emitter.SetInt64("Flags", flags);

  if (core::GetTableFieldRaw(L, abs_rec_idx, "exemplars")) {
    if (lua_istable(L, -1)) {
      emitter.SetTableFromStack("Exemplars");
    } else {
      lua_pop(L, 1);
      lua_newtable(L);
      emitter.SetTableFromStack("Exemplars");
    }
  } else {
    lua_newtable(L);
    emitter.SetTableFromStack("Exemplars");
  }

  emitter.SetString("processed_at", processed_at);

  emitter.EndRecord();
  if (emitter.HasError()) {
    err_out = std::string(emitter.ErrorMessage());
    return false;
  }
  return true;
}

}  // namespace

extractors::ExtractionResult ProcessNativeMetrics(lua_State* L, std::string_view raw_json,
                                                  int emit_idx) {
  if (raw_json.empty()) {
    return ExtractionResult{false, "Empty JSON input", 0};
  }

  core::LuaEmitter emitter(L, emit_idx);
  try {
    auto& parser = core::OtelParser::Instance();
    static thread_local core::FastJsonWriter writer;
    static thread_local std::string tls_metric_name_scratch;
    static thread_local std::string tls_service_scratch;
    static thread_local std::string tls_scope_name_scratch;
    static thread_local std::string tls_scope_ver_scratch;
    static thread_local std::string tls_metric_desc_scratch;
    static thread_local std::string tls_metric_unit_scratch;
    static thread_local std::vector<int64_t> tls_bucket_counts;
    static thread_local std::vector<double> tls_explicit_bounds;

    writer.Reset();
    struct WriterTrimGuard {
      explicit WriterTrimGuard(core::FastJsonWriter& w_in, std::vector<int64_t>& bc_in,
                               std::vector<double>& eb_in)
          : w(w_in), bc(bc_in), eb(eb_in) {}
      WriterTrimGuard(const WriterTrimGuard&) = delete;
      WriterTrimGuard& operator=(const WriterTrimGuard&) = delete;
      ~WriterTrimGuard() noexcept {
        w.Trim();
        core::ResetScratchVector(bc);
        core::ResetScratchVector(eb);
        auto trim_scratch = [](std::string& s) noexcept {
          if (s.capacity() > 4096) {
            std::string().swap(s);
          }
        };
        trim_scratch(tls_metric_name_scratch);
        trim_scratch(tls_service_scratch);
        trim_scratch(tls_scope_name_scratch);
        trim_scratch(tls_scope_ver_scratch);
        trim_scratch(tls_metric_desc_scratch);
        trim_scratch(tls_metric_unit_scratch);
      }
      core::FastJsonWriter& w;
      std::vector<int64_t>& bc;
      std::vector<double>& eb;
    } trim_guard{writer, tls_bucket_counts, tls_explicit_bounds};

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
      if (!ExtractNativeMetricObjectJson(
              L, doc_obj, processed_at, writer, tls_metric_name_scratch, tls_service_scratch,
              tls_scope_name_scratch, tls_scope_ver_scratch, tls_metric_desc_scratch,
              tls_metric_unit_scratch, tls_bucket_counts, tls_explicit_bounds, emitter, err_out)) {
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
          continue;
        }
        auto elem_obj = elem_obj_res.value();
        if (!ExtractNativeMetricObjectJson(
                L, elem_obj, processed_at, writer, tls_metric_name_scratch, tls_service_scratch,
                tls_scope_name_scratch, tls_scope_ver_scratch, tls_metric_desc_scratch,
                tls_metric_unit_scratch, tls_bucket_counts, tls_explicit_bounds, emitter,
                err_out)) {
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
    return ExtractionResult{false, "Unknown C++ exception during native metrics processing",
                            emitter.EmittedCount()};
  }
}

extractors::ExtractionResult ProcessNativeMetricsTable(lua_State* L, int table_idx, int emit_idx) {
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
  static thread_local std::string tls_metric_name_scratch;
  static thread_local std::string tls_service_scratch;
  static thread_local std::string tls_scope_name_scratch;
  static thread_local std::string tls_scope_ver_scratch;
  static thread_local std::string tls_metric_desc_scratch;
  static thread_local std::string tls_metric_unit_scratch;
  static thread_local std::vector<int64_t> tls_bucket_counts;
  static thread_local std::vector<double> tls_explicit_bounds;
  writer.Reset();
  struct TableScopeGuard {
    TableScopeGuard(lua_State* l, int t, core::FastJsonWriter& w, std::vector<int64_t>& bc_in,
                    std::vector<double>& eb_in)
        : L(l), top(t), writer(w), bc(bc_in), eb(eb_in) {}
    TableScopeGuard(const TableScopeGuard&) = delete;
    TableScopeGuard& operator=(const TableScopeGuard&) = delete;
    ~TableScopeGuard() noexcept {
      if (L != nullptr) {
        lua_settop(L, top);
      }
      writer.Trim();
      core::ResetScratchVector(bc);
      core::ResetScratchVector(eb);
      auto trim_scratch = [](std::string& s) noexcept {
        if (s.capacity() > 4096) {
          std::string().swap(s);
        }
      };
      trim_scratch(tls_metric_name_scratch);
      trim_scratch(tls_service_scratch);
      trim_scratch(tls_scope_name_scratch);
      trim_scratch(tls_scope_ver_scratch);
      trim_scratch(tls_metric_desc_scratch);
      trim_scratch(tls_metric_unit_scratch);
    }
    lua_State* L;
    int top;
    core::FastJsonWriter& writer;
    std::vector<int64_t>& bc;
    std::vector<double>& eb;
  } scope_guard{L, entry_top, writer, tls_bucket_counts, tls_explicit_bounds};

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
          if (HasNativeMetricFields(L, item_idx)) {
            if (!ExtractSingleNativeMetricTable(
                    L, item_idx, processed_at, writer, tls_metric_name_scratch, tls_service_scratch,
                    tls_scope_name_scratch, tls_scope_ver_scratch, tls_metric_desc_scratch,
                    tls_metric_unit_scratch, tls_bucket_counts, tls_explicit_bounds, emitter,
                    err_out)) {
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
      if (core::GetTableFieldRaw(L, abs_idx, "metric", "log")) {
        int inner_idx = lua_absindex(L, -1);
        if (lua_istable(L, inner_idx)) {
          handled_wrapper = true;
          const size_t inner_len = lua_rawlen(L, inner_idx);
          if (inner_len > 0) {
            if (!extract_array(inner_idx, inner_len)) {
              lua_pop(L, 1);
              return ExtractionResult{false, err_out, emitter.EmittedCount()};
            }
          } else if (HasNativeMetricFields(L, inner_idx)) {
            if (!ExtractSingleNativeMetricTable(
                    L, inner_idx, processed_at, writer, tls_metric_name_scratch,
                    tls_service_scratch, tls_scope_name_scratch, tls_scope_ver_scratch,
                    tls_metric_desc_scratch, tls_metric_unit_scratch, tls_bucket_counts,
                    tls_explicit_bounds, emitter, err_out)) {
              lua_pop(L, 1);
              return ExtractionResult{false, err_out, emitter.EmittedCount()};
            }
          }
        }
        lua_pop(L, 1);
      }
      if (!handled_wrapper && HasNativeMetricFields(L, abs_idx)) {
        if (!ExtractSingleNativeMetricTable(
                L, abs_idx, processed_at, writer, tls_metric_name_scratch, tls_service_scratch,
                tls_scope_name_scratch, tls_scope_ver_scratch, tls_metric_desc_scratch,
                tls_metric_unit_scratch, tls_bucket_counts, tls_explicit_bounds, emitter,
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
    return ExtractionResult{false, "Unknown C++ exception during native metrics table processing",
                            emitter.EmittedCount()};
  }
}

}  // namespace vector_otel::native_extractors
