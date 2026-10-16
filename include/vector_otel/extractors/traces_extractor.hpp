#pragma once

#include <string_view>

#include "vector_otel/extractors/logs_extractor.hpp"

extern "C" {
#include <lua.h>
}

namespace vector_otel {
namespace extractors {

ExtractionResult ProcessTraces(lua_State* L, std::string_view raw_json, int emit_idx);
ExtractionResult ProcessTracesTable(lua_State* L, int table_idx, int emit_idx);

}  // namespace extractors
}  // namespace vector_otel
