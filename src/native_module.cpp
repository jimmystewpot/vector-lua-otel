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
  return core::ProcessDispatcher<native_extractors::ProcessNativeTraces,
                                 native_extractors::ProcessNativeTracesTable>(L);
}

int ProcessNativeMetricsFn(lua_State* L) {
  return core::ProcessDispatcher<native_extractors::ProcessNativeMetrics,
                                 native_extractors::ProcessNativeMetricsTable>(L);
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
  lua_pushstring(L, "0.1.0");
  lua_setfield(L, -2, "_VERSION");

  luaopen_vector_native_logs(L);
  lua_setfield(L, -2, "logs");

  luaopen_vector_native_traces(L);
  lua_setfield(L, -2, "traces");

  luaopen_vector_native_metrics(L);
  lua_setfield(L, -2, "metrics");

  core::RegisterInPackageLoaded(L, "vector_native", -1);

  return 1;
}

}  // extern "C"
