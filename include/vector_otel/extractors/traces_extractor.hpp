#pragma once

#include <string_view>

#include "vector_otel/extractors/logs_extractor.hpp"

extern "C" {
#include <lua.h>
}

namespace vector_otel::extractors {

// Runner entry points (see core/process_runner.hpp).
bool ExtractTracesJson(core::OperationContext& ctx, core::OtelParser& parser,
                       simdjson::padded_string_view admitted);
bool ExtractTracesTable(core::OperationContext& ctx);
const core::SignalExtractors& TracesExtractors();

// Compatibility wrappers (L == nullptr runs a JSON dry run).
ExtractionResult ProcessTraces(lua_State* L, std::string_view raw_json, int emit_idx);
ExtractionResult ProcessTracesTable(lua_State* L, int table_idx, int emit_idx);

}  // namespace vector_otel::extractors
