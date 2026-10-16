#include <cstddef>
#include <exception>
#include <string>
#include <string_view>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "vector_otel/extractors/logs_extractor.hpp"
#include "vector_otel/extractors/metrics_extractor.hpp"
#include "vector_otel/extractors/traces_extractor.hpp"

namespace {

using ExtractorStringFn = vector_otel::extractors::ExtractionResult (*)(
    lua_State*, std::string_view, int);
using ExtractorTableFn =
    vector_otel::extractors::ExtractionResult (*)(lua_State*, int, int);

template <ExtractorStringFn StringExtractor, ExtractorTableFn TableExtractor>
int ProcessDispatcher(lua_State* L) {
  try {
    int arg_type = lua_type(L, 1);
    if (arg_type != LUA_TSTRING && arg_type != LUA_TTABLE) {
      lua_pushnil(L);
      lua_pushstring(L, "payload must be a string or table");
      lua_pushinteger(L, 0);
      return 3;
    }
    if (!lua_isfunction(L, 2)) {
      lua_pushnil(L);
      lua_pushstring(L, "emit callback must be a function");
      lua_pushinteger(L, 0);
      return 3;
    }

    vector_otel::extractors::ExtractionResult result;
    if (arg_type == LUA_TSTRING) {
      size_t len = 0;
      const char* str = lua_tolstring(L, 1, &len);
      std::string_view payload(str, len);
      result = StringExtractor(L, payload, 2);
    } else {
      result = TableExtractor(L, 1, 2);
    }

    if (result.success) {
      lua_pushinteger(L, result.emitted_count);
      lua_pushnil(L);
      return 2;
    }

    lua_pushnil(L);
    if (result.error.empty()) {
      lua_pushstring(L, "unknown extraction error");
    } else {
      lua_pushlstring(L, result.error.data(), result.error.size());
    }
    lua_pushinteger(L, result.emitted_count);
    return 3;
  } catch (const std::exception& e) {
    lua_pushnil(L);
    lua_pushstring(L, e.what());
    lua_pushinteger(L, 0);
    return 3;
  } catch (...) {
    lua_pushnil(L);
    lua_pushstring(L, "unknown C++ exception");
    lua_pushinteger(L, 0);
    return 3;
  }
}

int ProcessLogs(lua_State* L) {
  return ProcessDispatcher<vector_otel::extractors::ProcessLogs,
                           vector_otel::extractors::ProcessLogsTable>(L);
}

int ProcessTraces(lua_State* L) {
  return ProcessDispatcher<vector_otel::extractors::ProcessTraces,
                           vector_otel::extractors::ProcessTracesTable>(L);
}

int ProcessMetrics(lua_State* L) {
  return ProcessDispatcher<vector_otel::extractors::ProcessMetrics,
                           vector_otel::extractors::ProcessMetricsTable>(L);
}

void RegisterInPackageLoaded(lua_State* L, const char* name, int table_idx) {
  int abs_idx = lua_absindex(L, table_idx);

  // 1. In Lua 5.2+, package.loaded is registered in registry[LUA_LOADED_TABLE]
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  lua_pushvalue(L, abs_idx);
  lua_setfield(L, -2, name);
  lua_pop(L, 1);

  // 2. Also populate global package.loaded if package library is loaded
  lua_getglobal(L, "package");
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, "loaded");
    if (lua_istable(L, -1)) {
      lua_pushvalue(L, abs_idx);
      lua_setfield(L, -2, name);
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
}

const luaL_Reg kLogsFuncs[] = {
    {"process", ProcessLogs},
    {nullptr, nullptr},
};

const luaL_Reg kTracesFuncs[] = {
    {"process", ProcessTraces},
    {nullptr, nullptr},
};

const luaL_Reg kMetricsFuncs[] = {
    {"process", ProcessMetrics},
    {nullptr, nullptr},
};

}  // namespace

extern "C" {

int luaopen_vector_otel_logs(lua_State* L) {
  lua_newtable(L);
  luaL_setfuncs(L, kLogsFuncs, 0);
  RegisterInPackageLoaded(L, "vector_otel.logs", -1);
  return 1;
}

int luaopen_vector_otel_traces(lua_State* L) {
  lua_newtable(L);
  luaL_setfuncs(L, kTracesFuncs, 0);
  RegisterInPackageLoaded(L, "vector_otel.traces", -1);
  return 1;
}

int luaopen_vector_otel_metrics(lua_State* L) {
  lua_newtable(L);
  luaL_setfuncs(L, kMetricsFuncs, 0);
  RegisterInPackageLoaded(L, "vector_otel.metrics", -1);
  return 1;
}

int luaopen_vector_otel(lua_State* L) {
  lua_newtable(L);
  lua_pushstring(L, "0.1.0");
  lua_setfield(L, -2, "_VERSION");

  luaopen_vector_otel_logs(L);
  lua_setfield(L, -2, "logs");

  luaopen_vector_otel_traces(L);
  lua_setfield(L, -2, "traces");

  luaopen_vector_otel_metrics(L);
  lua_setfield(L, -2, "metrics");

  RegisterInPackageLoaded(L, "vector_otel", -1);

  return 1;
}

}  // extern "C"
