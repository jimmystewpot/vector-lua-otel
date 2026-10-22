#pragma once

#include <string_view>

#include "vector_otel/extractors/logs_extractor.hpp"

extern "C" {
#include <lua.h>
}

namespace vector_otel::extractors {

// Runner entry points (see core/process_runner.hpp). One record is emitted per data point.
bool ExtractMetricsJson(core::OperationContext& ctx, core::OtelParser& parser,
                        simdjson::padded_string_view admitted);
bool ExtractMetricsTable(core::OperationContext& ctx);
const core::SignalExtractors& MetricsExtractors();

// Compatibility wrappers (L == nullptr runs a JSON dry run).
ExtractionResult ProcessMetrics(lua_State* L, std::string_view raw_json, int emit_idx);
ExtractionResult ProcessMetricsTable(lua_State* L, int table_idx, int emit_idx);

}  // namespace vector_otel::extractors
