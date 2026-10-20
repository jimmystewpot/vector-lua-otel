#pragma once

#include <cstdint>
#include <string>
#include <string_view>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "vector_otel/core/fast_json_writer.hpp"

namespace vector_otel::core {

class LuaEmitter;

// Pushes table[name1] (or table[name2] if name1 is nil).
// Returns true if field was found and is not nil.
// If nil or not found, pops the nil and returns false.
bool GetTableField(lua_State* L, int table_idx, const char* name1, const char* name2 = nullptr);

// Reads string field from table at table_idx, or returns default_val.
std::string ReadTableString(lua_State* L, int table_idx, const char* name1,
                            const char* name2 = nullptr, std::string_view default_val = "");

// String field reader using raw table access (lua_rawget, no metamethods).
// Copies string fields into `scratch` and returns a view into `scratch`.
// Otherwise returns `default_val`.
// Lifetime contract: non-default results view `scratch`; valid until `scratch`
// is reused; never borrows Lua memory. Callers must ensure no simultaneously-live
// views share one `scratch`.
std::string_view ReadTableStringView(lua_State* L, int table_idx, const char* name1,
                                     const char* name2, std::string& scratch,
                                     std::string_view default_val = "");

// Copies string field from table at table_idx to emitter without heap
// allocations.
void CopyStringField(lua_State* L, int table_idx, const char* name1, const char* name2,
                     LuaEmitter& emitter, std::string_view dst_key,
                     std::string_view default_val = "");

// Reads int64 field from table at table_idx (handles integers and numeric
// strings).
int64_t ReadTableInt64(lua_State* L, int table_idx, const char* name1, const char* name2 = nullptr,
                       int64_t default_val = 0);

// Reads uint64 field from table at table_idx (handles integers and numeric
// strings).
uint64_t ReadTableUint64(lua_State* L, int table_idx, const char* name1,
                         const char* name2 = nullptr, uint64_t default_val = 0);

// Reads double field from table at table_idx.
double ReadTableDouble(lua_State* L, int table_idx, const char* name1, const char* name2 = nullptr,
                       double default_val = 0.0);

// Parses OTLP severityNumber (handles integers and string enum names like
// "SEVERITY_NUMBER_INFO").
int64_t ParseTableSeverityNumber(lua_State* L, int val_idx);

// Reads nanosecond timestamp from record (checks timeUnixNano, falls back to
// observedTimeUnixNano).
uint64_t ReadTableTimestampNano(lua_State* L, int rec_idx);

// Reads body string or serialises structured body to JSON string.
std::string ReadTableBodyString(lua_State* L, int rec_idx, FastJsonWriter& writer);

// Copies or serialises body field from record to emitter without heap
// allocations.
void CopyTableBody(lua_State* L, int rec_idx, FastJsonWriter& writer, LuaEmitter& emitter);

// Extracts service.name from resource table, with fallback to
// "unknown_service".
std::string ExtractTableServiceName(lua_State* L, int res_idx);

// Variant of ExtractTableServiceName (first service.name / service_name wins;
// "unknown_service" default); copies string values into `scratch`.
// Lifetime contract: non-default results view `scratch`; valid until `scratch`
// is reused; never borrows Lua memory.
std::string_view ExtractTableServiceNameView(lua_State* L, int res_idx, std::string& scratch);

// Pushes a new flattened key-value table of attributes onto the Lua stack.
// Handles both OTLP array-of-KeyValue and direct key-value maps.
void PushTableAttributes(lua_State* L, int attrs_idx, FastJsonWriter& writer);

// Serialises any Lua value or OTLP AnyValue table to JSON via FastJsonWriter
// with maximum recursion depth protection against stack exhaustion.
void SerializeLuaValueToJson(lua_State* L, int val_idx, FastJsonWriter& writer, int depth = 0);

// Pushes an array of Events table for traces onto the Lua stack.
void PushTableSpanEvents(lua_State* L, int events_idx, FastJsonWriter& writer);

// Pushes an array of Links table for traces onto the Lua stack.
void PushTableSpanLinks(lua_State* L, int links_idx, FastJsonWriter& writer);

}  // namespace vector_otel::core
