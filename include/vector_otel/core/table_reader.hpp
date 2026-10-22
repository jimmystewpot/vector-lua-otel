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

// Pushes table[name1] (or table[name2] if name1 is nil, or table[name3] if name2 is nil).
// Returns true if field was found and is not nil.
// If nil or not found, pops the nil and returns false.
bool GetTableField(lua_State* L, int table_idx, const char* name1, const char* name2 = nullptr,
                   const char* name3 = nullptr);

// Raw table lookup (lua_rawget, no metamethods) for table[name1] (or
// table[name2] if name1 is nil, or table[name3] if name2 is nil).
bool GetTableFieldRaw(lua_State* L, int table_idx, const char* name1, const char* name2 = nullptr,
                      const char* name3 = nullptr);

// O(1) check returning true if the table at table_idx is not a table or has 0
// keys (using a single lua_next probe without string interning).
bool IsLuaTableEmpty(lua_State* L, int table_idx) noexcept;

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

// Copies correlation ID field from table at table_idx to emitter, normalizing
// binary strings (16B trace ID, 8B span ID) or uppercase hex into canonical
// lowercase hex. If absent or non-string, emits empty string "".
void CopyIdField(lua_State* L, int table_idx, const char* name1, const char* name2,
                 size_t expected_binary_len, LuaEmitter& emitter, std::string_view dst_key,
                 const char* name3 = nullptr);

// Always sets dst_key on emitter (even when default_val is empty), reading
// table[name1] or table[name2] via raw table lookup.
void CopyStringFieldAlways(lua_State* L, int table_idx, const char* name1, const char* name2,
                           LuaEmitter& emitter, std::string_view dst_key,
                           std::string_view default_val = "");

// Parses int64/uint64/double value at Lua stack index val_idx (handles
// integers, finite floats, and numeric strings).
bool TryParseTableInt64At(lua_State* L, int val_idx, int64_t& out);
bool TryParseTableUint64At(lua_State* L, int val_idx, uint64_t& out);
bool TryParseTableDoubleAt(lua_State* L, int val_idx, double& out);

// Reads int64/uint64/double field from table at table_idx into `out`, returning
// true on valid parse and false if missing or invalid.
bool TryReadTableInt64(lua_State* L, int table_idx, const char* name1, const char* name2,
                       int64_t& out, const char* name3 = nullptr);
bool TryReadTableUint64(lua_State* L, int table_idx, const char* name1, const char* name2,
                        uint64_t& out, const char* name3 = nullptr);
bool TryReadTableDouble(lua_State* L, int table_idx, const char* name1, const char* name2,
                        double& out, const char* name3 = nullptr);

// Reads int64 field from table at table_idx (handles integers and numeric
// strings).
int64_t ReadTableInt64(lua_State* L, int table_idx, const char* name1, const char* name2 = nullptr,
                       int64_t default_val = 0, const char* name3 = nullptr);

// Reads uint64 field from table at table_idx (handles integers and numeric
// strings).
uint64_t ReadTableUint64(lua_State* L, int table_idx, const char* name1,
                         const char* name2 = nullptr, uint64_t default_val = 0,
                         const char* name3 = nullptr);

// Reads double field from table at table_idx.
double ReadTableDouble(lua_State* L, int table_idx, const char* name1, const char* name2 = nullptr,
                       double default_val = 0.0, const char* name3 = nullptr);

// Parses OTLP severityNumber (handles integers and string enum names like
// "SEVERITY_NUMBER_INFO").
int64_t ParseTableSeverityNumber(lua_State* L, int val_idx);

// Reads nanosecond timestamp from record (checks timeUnixNano, falls back to
// observedTimeUnixNano).
uint64_t ReadTableTimestampNano(lua_State* L, int rec_idx);

// Reads a Lua timestamp value (Vector timestamp table, ISO-8601 string, or
// non-negative numeric nanoseconds).
bool ReadLuaTimestampValue(lua_State* L, int val_idx, uint64_t& out_ns) noexcept;

// Reads a named timestamp field from table at table_idx (checking name1, then
// name2).
bool ReadTableTimestampField(lua_State* L, int table_idx, const char* name1, const char* name2,
                             uint64_t& out_ns) noexcept;

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
// If out_service_name is non-null, captures string "service.name" (or
// "service_name") in-pass without a second hash lookup.
void PushTableAttributes(lua_State* L, int attrs_idx, FastJsonWriter& writer,
                         std::string_view* out_service_name = nullptr);

// Serialises any Lua value or OTLP AnyValue table to JSON via FastJsonWriter
// with maximum recursion depth protection against stack exhaustion.
void SerializeLuaValueToJson(lua_State* L, int val_idx, FastJsonWriter& writer, int depth = 0);

// Serialises attributes table (OTLP array-of-KeyValue or direct key-value map)
// directly to FastJsonWriter as a flattened JSON object without calling writer.Reset().
void WriteTableAttributesJson(lua_State* L, int attrs_idx, FastJsonWriter& writer);

// Serialises span events array to FastJsonWriter in ClickHouse JSON format
// [{"Timestamp":"...","Name":"...","Attributes":{...}}, ...] without calling writer.Reset().
void SerializeTableSpanEventsToJson(lua_State* L, int events_idx, FastJsonWriter& writer);

// Serialises span links array to FastJsonWriter in ClickHouse JSON format
// [{"TraceId":"...","SpanId":"...","TraceState":"...","Attributes":{...}}, ...]
// with normalized hex IDs and without calling writer.Reset().
void SerializeTableSpanLinksToJson(lua_State* L, int links_idx, FastJsonWriter& writer);

// Pushes a new normalized array table of metric exemplars onto the Lua stack,
// matching ParseExemplars ClickHouse schema (FilteredAttributes, TimeUnix, Value, SpanId, TraceId).
void PushTableExemplars(lua_State* L, int exemplars_idx, FastJsonWriter& writer);

// Pushes an array of Events table for traces onto the Lua stack.
void PushTableSpanEvents(lua_State* L, int events_idx, FastJsonWriter& writer);

// Pushes an array of Links table for traces onto the Lua stack.
void PushTableSpanLinks(lua_State* L, int links_idx, FastJsonWriter& writer);

}  // namespace vector_otel::core
