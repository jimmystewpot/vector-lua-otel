#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "simdjson.h"
#include "vector_otel/core/fast_json_writer.hpp"

extern "C" {
#include <lua.h>
}

namespace vector_otel::core {

std::string_view ExtractServiceName(simdjson::ondemand::object& resource_obj);
void PushAttributesTable(lua_State* L, simdjson::ondemand::value& attrs_val,
                         FastJsonWriter& json_writer, std::string_view* out_service_name = nullptr);
void PushAnyValue(lua_State* L, simdjson::ondemand::value& val, FastJsonWriter& json_writer);
int64_t ParseInt64(simdjson::ondemand::value& val);
uint64_t ParseUint64(simdjson::ondemand::value& val);
double ParseDouble(simdjson::ondemand::value& val);
bool TryParseDouble(simdjson::ondemand::value& val, double& out);
std::string_view ExtractStringField(simdjson::ondemand::object& obj, std::string_view key,
                                    std::string_view default_val = "");

// Reset the thread-local scratch buffer used by PushAttributesTable to store service.name.
// Must be called at the end of each batch to ensure the string_view is invalidated.
void ResetServiceNameScratch() noexcept;

}  // namespace vector_otel::core
