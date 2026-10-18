#pragma once

#include <string>
#include <string_view>

extern "C" {
#include <lua.h>
}

namespace vector_otel::extractors {

struct ExtractionResult {
  bool success{false};
  std::string error;
  int emitted_count{0};
};

ExtractionResult ProcessLogs(lua_State* L, std::string_view raw_json,
                             int emit_idx);
ExtractionResult ProcessLogsTable(lua_State* L, int table_idx, int emit_idx);

}  // namespace vector_otel::extractors
