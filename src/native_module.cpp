extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "vector_otel/core/module_runner.hpp"
#include "vector_otel/core/module_state.hpp"
#include "vector_otel/core/process_runner.hpp"
#include "vector_otel/native_extractors/native_logs_extractor.hpp"
#include "vector_otel/native_extractors/native_metrics_extractor.hpp"
#include "vector_otel/native_extractors/native_traces_extractor.hpp"

namespace {

namespace core = vector_otel::core;
namespace native_extractors = vector_otel::native_extractors;

int ProcessNativeLogsFn(lua_State* L) {
  return core::DispatchProcess(L, core::ModuleKind::Native, core::Signal::Logs,
                               native_extractors::NativeLogsExtractors());
}

int ProcessNativeTracesFn(lua_State* L) {
  return core::DispatchProcess(L, core::ModuleKind::Native, core::Signal::Traces,
                               native_extractors::NativeTracesExtractors());
}

int ProcessNativeMetricsFn(lua_State* L) {
  return core::DispatchProcess(L, core::ModuleKind::Native, core::Signal::Metrics,
                               native_extractors::NativeMetricsExtractors());
}

const luaL_Reg kLogsFuncs[] = {
    {"process", ProcessNativeLogsFn},
    {nullptr, nullptr},
};

const luaL_Reg kTracesFuncs[] = {
    {"process", ProcessNativeTracesFn},
    {nullptr, nullptr},
};

const luaL_Reg kMetricsFuncs[] = {
    {"process", ProcessNativeMetricsFn},
    {nullptr, nullptr},
};

}  // namespace

extern "C" {

int luaopen_vector_native_logs(lua_State* L) {
  core::OpenSignalTable(L, core::ModuleKind::Native, kLogsFuncs);
  core::RegisterInPackageLoaded(L, "vector_native.logs", -1);
  return 1;
}

int luaopen_vector_native_traces(lua_State* L) {
  core::OpenSignalTable(L, core::ModuleKind::Native, kTracesFuncs);
  core::RegisterInPackageLoaded(L, "vector_native.traces", -1);
  return 1;
}

int luaopen_vector_native_metrics(lua_State* L) {
  core::OpenSignalTable(L, core::ModuleKind::Native, kMetricsFuncs);
  core::RegisterInPackageLoaded(L, "vector_native.metrics", -1);
  return 1;
}

int luaopen_vector_native(lua_State* L) {
  lua_newtable(L);
  lua_pushstring(L, "0.3.0");
  lua_setfield(L, -2, "_VERSION");

  core::OpenSignalTable(L, core::ModuleKind::Native, kLogsFuncs);
  const int logs = lua_gettop(L);
  core::OpenSignalTable(L, core::ModuleKind::Native, kTracesFuncs);
  const int traces = lua_gettop(L);
  core::OpenSignalTable(L, core::ModuleKind::Native, kMetricsFuncs);
  const int metrics = lua_gettop(L);
  const int parent = logs - 1;
  lua_pushvalue(L, logs);
  lua_setfield(L, parent, "logs");
  lua_pushvalue(L, traces);
  lua_setfield(L, parent, "traces");
  lua_pushvalue(L, metrics);
  lua_setfield(L, parent, "metrics");
  const vector_otel::core::ModuleRegistration registrations[] = {
      {"vector_native", parent},
      {"vector_native.logs", logs},
      {"vector_native.traces", traces},
      {"vector_native.metrics", metrics},
  };
  vector_otel::core::RegisterModulesInPackageLoaded(L, registrations, 4);
  lua_settop(L, parent);
  return 1;
}

}  // extern "C"
