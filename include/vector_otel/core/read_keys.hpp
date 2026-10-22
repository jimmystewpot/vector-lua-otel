#pragma once

// Names that the table-mode readers look up in caller-supplied Lua tables.
//
// Table-mode extraction reads Lua tables in the dispatcher's outer frame, where a Lua error would
// skip C++ destructors. The only reader operation that can allocate (and therefore raise) is
// pushing a key string that is not yet interned. Every name in this list is interned at module
// bootstrap and rooted in the module anchor, so lua_pushstring() on it finds the existing string
// and can never allocate.
//
// ReadKey makes that a compile-time guarantee: it can only be built from a string literal that is
// listed here (and short enough to be interned by Lua), so a reader call with an unlisted name
// does not compile. Add new names to kReadKeyList.

#include <cstddef>
#include <string_view>

namespace vector_otel::core {

// Lua interns strings of at most 40 bytes (LUAI_MAXSHORTLEN); longer ones are allocated per push.
inline constexpr std::size_t kMaxReadKeyLength = 40;

inline constexpr std::string_view kReadKeyList[] = {
    "AggregationTemporality",
    "Attributes",
    "BucketCounts",
    "Count",
    "EventName",
    "Exemplars",
    "ExplicitBounds",
    "FilteredAttributes",
    "Flags",
    "Max",
    "Min",
    "Name",
    "Negative",
    "Offset",
    "ParentSpanId",
    "Positive",
    "Quantile",
    "QuantileValues",
    "Scale",
    "SpanId",
    "SpanName",
    "Sum",
    "TimeUnix",
    "Timestamp",
    "TraceFlags",
    "TraceId",
    "TraceState",
    "Value",
    "Values",
    "ZeroCount",
    "ZeroThreshold",
    "aggregated_histogram",
    "aggregated_summary",
    "aggregationTemporality",
    "aggregation_temporality",
    "arrayValue",
    "array_value",
    "asDouble",
    "asInt",
    "as_double",
    "as_double_value",
    "as_int",
    "as_int_value",
    "attributes",
    "body",
    "boolValue",
    "bool_value",
    "bucketCounts",
    "bucket_counts",
    "buckets",
    "bytesValue",
    "bytes_value",
    "code",
    "count",
    "counter",
    "counts",
    "dataPoints",
    "data_points",
    "day",
    "description",
    "distribution",
    "doubleValue",
    "double_value",
    "endTimeUnixNano",
    "end_time_unix_nano",
    "end_timestamp",
    "eventName",
    "event_name",
    "events",
    "exemplars",
    "explicitBounds",
    "explicit_bounds",
    "exponentialHistogram",
    "exponential_histogram",
    "filteredAttributes",
    "filtered_attributes",
    "flags",
    "gauge",
    "histogram",
    "hour",
    "ingestTimestamp",
    "ingest_timestamp",
    "intValue",
    "int_value",
    "interval_ms",
    "isMonotonic",
    "is_monotonic",
    "key",
    "kind",
    "kvlistValue",
    "kvlist_value",
    "links",
    "log",
    "logAttributes",
    "logRecords",
    "log_attributes",
    "log_records",
    "max",
    "message",
    "metric",
    "metrics",
    "min",
    "month",
    "name",
    "namespace",
    "nanosec",
    "negative",
    "observedTimeUnixNano",
    "observed_time_unix_nano",
    "observed_timestamp",
    "offset",
    "parentSpanId",
    "parent_span_id",
    "positive",
    "quantile",
    "quantileValues",
    "quantile_values",
    "quantiles",
    "resource",
    "resourceAttributes",
    "resourceLogs",
    "resourceMetrics",
    "resourceSchemaUrl",
    "resourceSpans",
    "resource_attributes",
    "resource_logs",
    "resource_metrics",
    "resource_schema_url",
    "resource_spans",
    "resources",
    "scale",
    "schemaUrl",
    "schema_url",
    "scope",
    "scopeAttributes",
    "scopeLogs",
    "scopeMetrics",
    "scopeName",
    "scopeSchemaUrl",
    "scopeSpans",
    "scopeVersion",
    "scope_attributes",
    "scope_logs",
    "scope_metrics",
    "scope_name",
    "scope_schema_url",
    "scope_spans",
    "scope_version",
    "sec",
    "service",
    "service.name",
    "service_name",
    "set",
    "severityNumber",
    "severityText",
    "severity_number",
    "severity_text",
    "sketch",
    "spanAttributes",
    "spanId",
    "spanKind",
    "spanName",
    "span_attributes",
    "span_id",
    "span_kind",
    "span_name",
    "spans",
    "startTimeUnixNano",
    "start_time_unix_nano",
    "start_timestamp",
    "status",
    "statusCode",
    "statusMessage",
    "status_code",
    "status_message",
    "stringValue",
    "string_value",
    "sum",
    "summary",
    "tags",
    "timeUnix",
    "timeUnixNano",
    "time_unix",
    "time_unix_nano",
    "timestamp",
    "trace",
    "traceFlags",
    "traceId",
    "traceState",
    "trace_flags",
    "trace_id",
    "trace_state",
    "uintValue",
    "uint_value",
    "unit",
    "upperLimit",
    "upper_limit",
    "value",
    "values",
    "version",
    "year",
    "zeroCount",
    "zeroThreshold",
    "zero_count",
    "zero_threshold",
};

inline constexpr std::size_t kReadKeyCount = sizeof(kReadKeyList) / sizeof(kReadKeyList[0]);

constexpr bool IsListedReadKey(std::string_view name) noexcept {
  for (const std::string_view listed : kReadKeyList) {
    if (listed == name) {
      return true;
    }
  }
  return false;
}

// A table field name. Default-constructed / nullptr means "no name" (optional alternative
// spellings).
class ReadKey {
 public:
  constexpr ReadKey() noexcept = default;
  constexpr ReadKey(std::nullptr_t) noexcept {}  // NOLINT(google-explicit-constructor)

  template <std::size_t N>
  consteval ReadKey(const char (&literal)[N])  // NOLINT(google-explicit-constructor)
      : name_(literal) {
    static_assert(N - 1 <= kMaxReadKeyLength, "read key too long to be interned by Lua");
    if (!IsListedReadKey(std::string_view(literal, N - 1))) {
      throw "read key is not listed in kReadKeyList (include/vector_otel/core/read_keys.hpp)";
    }
  }

#ifdef VECTOR_OTEL_UNCHECKED_READ_KEYS
  // Test targets only: lets unit tests probe the readers with arbitrary keys. The library and the
  // Lua modules are never built with this macro, so production code keeps the compile-time check.
  constexpr ReadKey(const char* unchecked) noexcept : name_(unchecked) {}  // NOLINT
#endif

  constexpr bool present() const noexcept { return name_ != nullptr; }
  constexpr const char* c_str() const noexcept { return name_; }

 private:
  const char* name_ = nullptr;
};

}  // namespace vector_otel::core
