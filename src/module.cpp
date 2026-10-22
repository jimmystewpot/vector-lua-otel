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
  return core::ProcessDispatcher<extractors::ProcessMetrics, extractors::ProcessMetricsTable>(L);
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
  lua_pushstring(L, "0.1.0");
  lua_setfield(L, -2, "_VERSION");

  luaopen_vector_otel_logs(L);
  lua_setfield(L, -2, "logs");

  luaopen_vector_otel_traces(L);
  lua_setfield(L, -2, "traces");

  luaopen_vector_otel_metrics(L);
  lua_setfield(L, -2, "metrics");

  core::RegisterInPackageLoaded(L, "vector_otel", -1);

  return 1;
}

}  // extern "C"
