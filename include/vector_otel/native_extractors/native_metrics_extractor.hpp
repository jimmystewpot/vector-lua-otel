#pragma once

#include <string_view>

extern "C" {
#include <lua.h>
}

#include "vector_otel/extractors/logs_extractor.hpp"

namespace vector_otel::native_extractors {

extractors::ExtractionResult ProcessNativeMetrics(lua_State* L, std::string_view raw_json,
                                                  int emit_idx);
extractors::ExtractionResult ProcessNativeMetricsTable(lua_State* L, int table_idx, int emit_idx);

}  // namespace vector_otel::native_extractors
