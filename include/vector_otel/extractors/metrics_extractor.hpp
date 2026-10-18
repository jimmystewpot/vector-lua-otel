#pragma once

#include <string_view>

#include "vector_otel/extractors/logs_extractor.hpp"

extern "C" {
#include <lua.h>
}

namespace vector_otel::extractors {

/// Process OTLP Metrics JSON and emit individual metric events matching
/// ClickHouse schema. Returns (success, error_msg, emitted_count).
ExtractionResult ProcessMetrics(lua_State* L, std::string_view raw_json, int emit_idx);
ExtractionResult ProcessMetricsTable(lua_State* L, int table_idx, int emit_idx);

}  // namespace vector_otel::extractors
