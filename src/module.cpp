extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "vector_otel/core/module_runner.hpp"
#include "vector_otel/core/module_state.hpp"
#include "vector_otel/core/process_runner.hpp"
#include "vector_otel/extractors/logs_extractor.hpp"
#include "vector_otel/extractors/metrics_extractor.hpp"
#include "vector_otel/extractors/traces_extractor.hpp"

namespace {

namespace core = vector_otel::core;
namespace extractors = vector_otel::extractors;

int ProcessLogs(lua_State* L) {
  return core::DispatchProcess(L, core::ModuleKind::Otel, core::Signal::Logs,
                               extractors::LogsExtractors());
}

int ProcessTraces(lua_State* L) {
  return core::DispatchProcess(L, core::ModuleKind::Otel, core::Signal::Traces,
                               extractors::TracesExtractors());
}

int ProcessMetrics(lua_State* L) {
  return core::DispatchProcess(L, core::ModuleKind::Otel, core::Signal::Metrics,
                               extractors::MetricsExtractors());
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
  core::OpenSignalTable(L, core::ModuleKind::Otel, kLogsFuncs);
  core::RegisterInPackageLoaded(L, "vector_otel.logs", -1);
  return 1;
}

int luaopen_vector_otel_traces(lua_State* L) {
  core::OpenSignalTable(L, core::ModuleKind::Otel, kTracesFuncs);
  core::RegisterInPackageLoaded(L, "vector_otel.traces", -1);
  return 1;
}

int luaopen_vector_otel_metrics(lua_State* L) {
  core::OpenSignalTable(L, core::ModuleKind::Otel, kMetricsFuncs);
  core::RegisterInPackageLoaded(L, "vector_otel.metrics", -1);
  return 1;
}

int luaopen_vector_otel(lua_State* L) {
  lua_newtable(L);
  lua_pushstring(L, "0.3.0");
  lua_setfield(L, -2, "_VERSION");

  core::OpenSignalTable(L, core::ModuleKind::Otel, kLogsFuncs);
  const int logs = lua_gettop(L);
  core::OpenSignalTable(L, core::ModuleKind::Otel, kTracesFuncs);
  const int traces = lua_gettop(L);
  core::OpenSignalTable(L, core::ModuleKind::Otel, kMetricsFuncs);
  const int metrics = lua_gettop(L);
  const int parent = logs - 1;
  lua_pushvalue(L, logs);
  lua_setfield(L, parent, "logs");
  lua_pushvalue(L, traces);
  lua_setfield(L, parent, "traces");
  lua_pushvalue(L, metrics);
  lua_setfield(L, parent, "metrics");
  const vector_otel::core::ModuleRegistration registrations[] = {
      {"vector_otel", parent},
      {"vector_otel.logs", logs},
      {"vector_otel.traces", traces},
      {"vector_otel.metrics", metrics},
  };
  vector_otel::core::RegisterModulesInPackageLoaded(L, registrations, 4);
  lua_settop(L, parent);
  return 1;
}

}  // extern "C"
