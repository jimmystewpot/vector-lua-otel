#pragma once

// Test-only reference implementations of the original Lua-table-building helpers. They may raise
// Lua errors and throw C++ exceptions, so they are never part of the shipped modules; tests use
// them as an oracle for the allocation-free staged equivalents.

#include <string_view>

#include "simdjson.h"
#include "vector_otel/core/fast_json_writer.hpp"
#include "vector_otel/core/read_keys.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace vector_otel::core {

void PushAttributesTable(lua_State* L, simdjson::ondemand::value& attrs_val,
                         FastJsonWriter& json_writer, std::string_view* out_service_name = nullptr);
void PushAnyValue(lua_State* L, simdjson::ondemand::value& val, FastJsonWriter& json_writer);
void PushTableAttributes(lua_State* L, int attrs_idx, FastJsonWriter& writer,
                         std::string_view* out_service_name = nullptr);
void PushTableSpanEvents(lua_State* L, int events_idx, FastJsonWriter& writer);
void PushTableSpanLinks(lua_State* L, int links_idx, FastJsonWriter& writer);

}  // namespace vector_otel::core
