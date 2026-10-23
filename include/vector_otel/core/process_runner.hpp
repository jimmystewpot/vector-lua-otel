#pragma once

// The shared process() runner.
//
// DispatchProcess() is the lua_CFunction body of every signal's process(). Its frame is the OUTER
// frame of an invocation: it owns the OperationContext, the parser workspace and the staging
// buffers, and it never calls anything that can raise. Every Lua operation that can allocate or
// raise (anchoring, counting a table, building and delivering rows, building the result) runs as a
// protected leaf; table input is read here with raw, allocation-free operations only.
//
// Order of an invocation:
//   1. reentrancy guard, argument validation (exact legacy messages, pre-interned strings);
//   2. anchor the input and the emit callback;
//   3. admission (byte limit, whole-event record limit) BEFORE any extraction or emission;
//   4. extraction (JSON through the retained parser workspace, or a Lua table), staging rows and
//      delivering them through the protected emission leaves;
//   5. every C++ exception becomes a fixed diagnostic; roots are cleared; the result tuple is
//      pushed as (count, nil) or (nil, message, delivered_count).

#include <cstdint>
#include <string_view>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "simdjson.h"
#include "vector_otel/core/operation_context.hpp"
#include "vector_otel/core/parser.hpp"
#include "vector_otel/extractors/logs_extractor.hpp"

namespace vector_otel::core {

// Free Lua stack slots reserved once per invocation: the raw table readers push into them without
// further checks.
inline constexpr int kJsonStackReserve = 32;
inline constexpr int kTableStackReserve = 256;

// An extractor stages and delivers rows. It returns true on success; on failure it returns false
// with ctx.error set (delivery failures are already recorded by the emission leaves). It may
// throw: the runner converts exceptions into diagnostics. It never calls Lua directly.
using JsonExtractFn = bool (*)(OperationContext& ctx, OtelParser& parser,
                               simdjson::padded_string_view admitted);
using TableExtractFn = bool (*)(OperationContext& ctx);

struct SignalExtractors {
  JsonExtractFn json;
  TableExtractFn table;
};

// The lua_CFunction body. Upvalue 1 must be the module anchor (OpenSignalTable).
int DispatchProcess(lua_State* L, ModuleKind module, Signal signal, const SignalExtractors& fns);

// Compatibility entry points for callers that already own a Lua state and pass the emit callback
// at `emit_idx` (the C++ extractor tests, native-style wrappers). They run the same admission /
// staging / protected-emission machinery as DispatchProcess() but report through ExtractionResult
// instead of pushing the result tuple. `L == nullptr` runs a dry run over JSON: rows are staged
// and counted but nothing is delivered (EmittedCount stays 0).
extractors::ExtractionResult RunCompatJson(lua_State* L, ModuleKind module, Signal signal,
                                           const SignalExtractors& fns, std::string_view raw_json,
                                           int emit_idx);
extractors::ExtractionResult RunCompatTable(lua_State* L, ModuleKind module, Signal signal,
                                            const SignalExtractors& fns, int table_idx,
                                            int emit_idx);

}  // namespace vector_otel::core
