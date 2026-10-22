#pragma once

#include <string_view>

extern "C" {
#include <lua.h>
}

#include "simdjson.h"
#include "vector_otel/core/operation_context.hpp"
#include "vector_otel/core/parser.hpp"
#include "vector_otel/extractors/logs_extractor.hpp"

namespace vector_otel::native_extractors {

// Runner entry points (see core/process_runner.hpp). On failure they return false with ctx.error
// set; they may throw.
bool ExtractNativeLogsJson(core::OperationContext& ctx, core::OtelParser& parser,
                           simdjson::padded_string_view admitted);
bool ExtractNativeLogsTable(core::OperationContext& ctx);

// The runner table for the native logs signal.
const core::SignalExtractors& NativeLogsExtractors();

// Compatibility wrappers: same machinery, results reported as ExtractionResult.
extractors::ExtractionResult ProcessNativeLogs(lua_State* L, std::string_view raw_json,
                                               int emit_idx);
extractors::ExtractionResult ProcessNativeLogsTable(lua_State* L, int table_idx, int emit_idx);

}  // namespace vector_otel::native_extractors
