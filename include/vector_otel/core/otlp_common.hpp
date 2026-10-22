#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "simdjson.h"
#include "vector_otel/core/fast_json_writer.hpp"

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
void PushAttributesTable(lua_State* L, simdjson::ondemand::value& attrs_val,
                         FastJsonWriter& json_writer, std::string_view* out_service_name = nullptr);
void PushAnyValue(lua_State* L, simdjson::ondemand::value& val, FastJsonWriter& json_writer);

namespace otlp_json_adl_barrier {
void WriteAnyValueJson(FastJsonWriter& writer, simdjson::ondemand::value& val, int depth = 0);
void WriteAttributesEntriesJson(FastJsonWriter& writer, simdjson::ondemand::value& attrs_val,
                                std::string_view* out_service_name = nullptr);
void WriteAttributesJson(FastJsonWriter& writer, simdjson::ondemand::value& attrs_val,
                         std::string_view* out_service_name = nullptr);
void SerializeExemplarsJson(simdjson::ondemand::value& val, FastJsonWriter& writer);
}  // namespace otlp_json_adl_barrier
using namespace otlp_json_adl_barrier;

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

// Reset the thread-local scratch buffer used by PushAttributesTable to store service.name.
// Must be called at the end of each batch to ensure the string_view is invalidated.
void ResetServiceNameScratch() noexcept;

std::string_view TrimJsonWhitespace(std::string_view sv) noexcept;
bool EqualsIgnoreCaseAscii(std::string_view a, std::string_view b) noexcept;
bool ParseJsonTimestamp(simdjson::ondemand::value& fv, uint64_t& out_ns) noexcept;

}  // namespace vector_otel::core
