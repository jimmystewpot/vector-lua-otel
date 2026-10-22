#pragma once

#include <string>
#include <string_view>

extern "C" {
#include <lua.h>
}

#include "simdjson.h"
#include "vector_otel/core/operation_context.hpp"
#include "vector_otel/core/parser.hpp"

namespace vector_otel::core {
struct SignalExtractors;
}

namespace vector_otel::extractors {

struct ExtractionResult {
  bool success{false};
  std::string error;
  int emitted_count{0};
};

// Runner entry points (see core/process_runner.hpp): stage rows and deliver them through the
// protected emission leaves; on failure return false with ctx.error set. They may throw.
bool ExtractLogsJson(core::OperationContext& ctx, core::OtelParser& parser,
                     simdjson::padded_string_view admitted);
bool ExtractLogsTable(core::OperationContext& ctx);

// The runner table for the logs signal (JSON + table entry points).
const core::SignalExtractors& LogsExtractors();

// Compatibility wrappers: same machinery, results reported as ExtractionResult. L == nullptr
// runs a JSON dry run (rows staged and dropped).
ExtractionResult ProcessLogs(lua_State* L, std::string_view raw_json, int emit_idx);
ExtractionResult ProcessLogsTable(lua_State* L, int table_idx, int emit_idx);

}  // namespace vector_otel::extractors
