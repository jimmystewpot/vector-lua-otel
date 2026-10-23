#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "simdjson.h"
#include "vector_otel/core/fast_json_writer.hpp"
#include "vector_otel/core/staged_row.hpp"

extern "C" {
#include <lua.h>
}

namespace vector_otel::core {

inline constexpr size_t kTraceIdBinaryLen = 16;
inline constexpr size_t kTraceIdHexLen = 32;
inline constexpr size_t kSpanIdBinaryLen = 8;
inline constexpr size_t kSpanIdHexLen = 16;
inline constexpr size_t kMaxIdHexBufSize = 64;

using IdHexBuffer = std::array<char, kMaxIdHexBufSize>;

// Normalizes an OTLP correlation ID (trace/span ID) to canonical lowercase hex.
// Strictly allocation-free and noexcept; uses `stack_buf` when encoding or lowercasing.
std::string_view NormalizeOtelId(std::string_view raw, size_t expected_binary_len,
                                 IdHexBuffer& stack_buf) noexcept;

inline std::string_view NormalizeTraceId(std::string_view raw, IdHexBuffer& stack_buf) noexcept {
  return NormalizeOtelId(raw, kTraceIdBinaryLen, stack_buf);
}

inline std::string_view NormalizeSpanId(std::string_view raw, IdHexBuffer& stack_buf) noexcept {
  return NormalizeOtelId(raw, kSpanIdBinaryLen, stack_buf);
}

std::string_view ExtractServiceName(simdjson::ondemand::object& resource_obj);

// Staged (Lua-free) attribute builders. They write into a StagedRow and may throw std::bad_alloc /
// std::length_error; they never touch Lua.
//  * StageAnyValue stages one value under `key` (nothing for unsupported values) and returns true
//    when it was a scalar string, whose text is then reported through `scalar_text`.
//  * StageAttributeFields stages the entries as fields of the currently open container (the row
//    root when none is open), so a tape can be materialised as the attributes table itself.
//  * StageAttributes wraps the fields in a map under `key` (an empty map for malformed input).
// service.name capture: the last matching key wins.
bool StageAnyValue(StagedRow& row, Key key, simdjson::ondemand::value& val,
                   FastJsonWriter& json_writer, std::string_view* scalar_text = nullptr);
void StageAttributeFields(StagedRow& row, simdjson::ondemand::value& attrs_val,
                          FastJsonWriter& json_writer,
                          std::string_view* out_service_name = nullptr);
void StageAttributes(StagedRow& row, Key key, simdjson::ondemand::value& attrs_val,
                     FastJsonWriter& json_writer, std::string_view* out_service_name = nullptr);

void WriteAnyValueJson(FastJsonWriter& writer, simdjson::ondemand::value& val, int depth = 0);
std::string_view ExtractOtelBodyView(simdjson::ondemand::value& body_val, FastJsonWriter& writer);
void WriteAttributesEntriesJson(FastJsonWriter& writer, simdjson::ondemand::value& attrs_val,
                                std::string_view* out_service_name = nullptr);
void WriteAttributesJson(FastJsonWriter& writer, simdjson::ondemand::value& attrs_val,
                         std::string_view* out_service_name = nullptr);
void SerializeExemplarsJson(simdjson::ondemand::value& val, FastJsonWriter& writer);

struct SummaryQuantile {
  double quantile = 0.0;
  double value = 0.0;
};

void SerializeHistogramJson(FastJsonWriter& w, int64_t agg_temporality, uint64_t count,
                            bool has_sum, double sum, bool has_min, double min_val, bool has_max,
                            double max_val, const std::vector<uint64_t>& bucket_counts,
                            const std::vector<double>& explicit_bounds);

void SerializeExponentialHistogramJson(FastJsonWriter& w, int64_t agg_temporality, uint64_t count,
                                       bool has_sum, double sum, bool has_min, double min_val,
                                       bool has_max, double max_val, int64_t scale,
                                       uint64_t zero_count, double zero_threshold,
                                       int64_t pos_offset, const std::vector<uint64_t>& pos_buckets,
                                       int64_t neg_offset,
                                       const std::vector<uint64_t>& neg_buckets);

void SerializeSummaryJson(FastJsonWriter& w, uint64_t count, bool has_sum, double sum,
                          const std::vector<SummaryQuantile>& quantiles);

int64_t ParseInt64(simdjson::ondemand::value& val);
bool TryParseInt64(simdjson::ondemand::value& val, int64_t& out);
int64_t ParseSeverityNumberString(std::string_view sv) noexcept;
int64_t ParseSeverityNumber(simdjson::ondemand::value& val);
uint64_t ParseUint64(simdjson::ondemand::value& val);
double ParseDouble(simdjson::ondemand::value& val);
bool TryParseDouble(simdjson::ondemand::value& val, double& out);
std::string_view ExtractStringField(simdjson::ondemand::object& obj, std::string_view key,
                                    std::string_view default_val = "");
std::string_view ExtractStringField(simdjson::ondemand::object& obj, std::string_view key1,
                                    std::string_view key2, std::string_view default_val);

// Reset the thread-local scratch buffer used by StageAttributes to store service.name.
// Must be called at the end of each batch to ensure the string_view is invalidated.
void ResetServiceNameScratch() noexcept;

std::string_view TrimJsonWhitespace(std::string_view sv) noexcept;
bool EqualsIgnoreCaseAscii(std::string_view a, std::string_view b) noexcept;
bool ParseJsonTimestamp(simdjson::ondemand::value& fv, uint64_t& out_ns);

}  // namespace vector_otel::core
