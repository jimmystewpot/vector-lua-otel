#include "vector_otel/core/process_runner.hpp"

#include <exception>
#include <new>
#include <string>

#include "vector_otel/core/admission.hpp"
#include "vector_otel/core/module_runner.hpp"
#include "vector_otel/core/module_state.hpp"
#include "vector_otel/core/protected_lua.hpp"
#include "vector_otel/core/staged_row.hpp"

namespace vector_otel::core {

namespace {

// Records a C++ exception in the fixed diagnostic. Allocation failure keeps its own wording so a
// caller can tell memory exhaustion from a logic error; everything else is prefixed.
void RecordException(OperationContext& ctx, const std::exception* e) noexcept {
  if (e == nullptr) {
    ctx.error.Set(ErrorCode::Cpp, "Unknown C++ exception");
    return;
  }
  if (dynamic_cast<const std::bad_alloc*>(e) != nullptr) {
    ctx.error.Set(ErrorCode::Memory, kMemoryText);
    return;
  }
  constexpr std::string_view kPrefix = "C++ exception: ";
  char buffer[ErrorStatus::kCapacity];
  std::size_t n = 0;
  for (char c : kPrefix) {
    buffer[n++] = c;
  }
  const char* what = e->what();
  while (what != nullptr && *what != '\0' && n < sizeof(buffer) - 1) {
    buffer[n++] = *what++;
  }
  ctx.error.Set(ErrorCode::Cpp, std::string_view(buffer, n));
}

void AnchorArguments(OperationContext& ctx) noexcept {
  lua_State* L = ctx.L;
  lua_pushvalue(L, ctx.input_index);
  SetAnchor(L, ctx.anchor_index, AnchorSlot::kInput);
  lua_pushvalue(L, ctx.emit_index);
  SetAnchor(L, ctx.anchor_index, AnchorSlot::kEmit);
}

// Admission + extraction. Never throws and never raises: every Lua operation that can fail is a
// protected leaf, and the table readers use raw non-allocating operations only.
void RunExtraction(OperationContext& ctx, const SignalExtractors& fns, bool is_json,
                   std::string_view raw_json) noexcept {
  try {
    bool ok = false;
    if (is_json) {
      OtelParser& parser = OtelParser::Instance();
      ctx.parser = &parser;
      simdjson::padded_string_view admitted;
      if (!AdmitJson(ctx, parser, raw_json, admitted)) {
        return;
      }
      ok = fns.json(ctx, parser, admitted);
    } else {
      if (!AdmitTable(ctx, ctx.input_index)) {
        return;
      }
      ok = fns.table(ctx);
    }
    if (!ok && ctx.error.code == ErrorCode::None) {
      ctx.error.Set(ErrorCode::Schema, kExtractionText);  // extractor contract violation
    }
  } catch (const std::exception& e) {
    RecordException(ctx, &e);
  } catch (...) {
    RecordException(ctx, nullptr);
  }
}

int ReturnImmutableError(lua_State* L, ImmutableSlot message) noexcept {
  lua_pushnil(L);
  PushImmutable(L, lua_upvalueindex(1), message);
  lua_pushinteger(L, 0);
  return 3;
}

}  // namespace

int DispatchProcess(lua_State* L, ModuleKind module, Signal signal, const SignalExtractors& fns) {
  if (tls_active_extraction) [[unlikely]] {
    return ReturnImmutableError(L, ImmutableSlot::kReentrantMessage);
  }
  const int arg_type = lua_type(L, 1);
  if (arg_type != LUA_TSTRING && arg_type != LUA_TTABLE) {
    return ReturnImmutableError(L, ImmutableSlot::kPayloadArgMessage);
  }
  if (!lua_isfunction(L, 2)) {
    return ReturnImmutableError(L, ImmutableSlot::kEmitArgMessage);
  }
  const bool is_json = arg_type == LUA_TSTRING;
  if (!lua_checkstack(L, is_json ? kJsonStackReserve : kTableStackReserve)) {
    return ReturnImmutableError(L, ImmutableSlot::kStackMessage);
  }

  OperationContext ctx;
  InitOperationContext(ctx, L, module, signal, lua_upvalueindex(1));
  ctx.input_index = 1;
  ctx.emit_index = 2;

  {
    ActiveExtractionGuard guard;  // nothing below can unwind this frame with a longjmp
    AnchorArguments(ctx);
    std::string_view raw;
    if (is_json) {
      std::size_t len = 0;
      const char* text = lua_tolstring(L, 1, &len);  // already a string: no conversion
      raw = std::string_view(text, len);
    }
    RunExtraction(ctx, fns, is_json, raw);
  }
  ClearInvocationRoots(ctx);
  return ReturnOperationResult(ctx);
}

namespace {

extractors::ExtractionResult ResultOf(const OperationContext& ctx) {
  if (ctx.error.code == ErrorCode::None) {
    return extractors::ExtractionResult{true, "", ctx.delivered_count};
  }
  return extractors::ExtractionResult{false, std::string(ctx.error.view()), ctx.delivered_count};
}

// Runs an extraction with the state's module anchor pushed for its duration.
extractors::ExtractionResult RunCompat(lua_State* L, ModuleKind module, Signal signal,
                                       const SignalExtractors& fns, bool is_json,
                                       std::string_view raw_json, int input_idx, int emit_idx) {
  if (L == nullptr) {  // JSON dry run: no Lua at all
    OperationContext ctx;
    InitOperationContext(ctx, nullptr, module, signal, 0);
    ctx.dry_run = true;
    ActiveExtractionGuard guard;
    RunExtraction(ctx, fns, true, raw_json);
    return ResultOf(ctx);
  }

  const int entry_top = lua_gettop(L);
  const int input_abs = is_json ? 0 : lua_absindex(L, input_idx);
  const int emit_abs = lua_absindex(L, emit_idx);
  if (!lua_checkstack(L, is_json ? kJsonStackReserve + 4 : kTableStackReserve + 4)) {
    return extractors::ExtractionResult{false, std::string(kStackText), 0};
  }
  if (!EnsureModuleState(L, module)) {
    lua_settop(L, entry_top);
    return extractors::ExtractionResult{false, std::string(kMemoryText), 0};
  }
  OperationContext ctx;
  InitOperationContext(ctx, L, module, signal, lua_gettop(L));  // the anchor on top of the stack
  ctx.input_index = is_json ? emit_abs : input_abs;
  ctx.emit_index = emit_abs;
  {
    ActiveExtractionGuard guard;
    if (is_json) {
      lua_pushvalue(L, emit_abs);
      SetAnchor(L, ctx.anchor_index, AnchorSlot::kEmit);
    } else {
      AnchorArguments(ctx);
    }
    RunExtraction(ctx, fns, is_json, raw_json);
  }
  ClearInvocationRoots(ctx);
  ClearAnchor(L, ctx.anchor_index, AnchorSlot::kPendingError);
  lua_settop(L, entry_top);
  return ResultOf(ctx);
}

}  // namespace

extractors::ExtractionResult RunCompatJson(lua_State* L, ModuleKind module, Signal signal,
                                           const SignalExtractors& fns, std::string_view raw_json,
                                           int emit_idx) {
  return RunCompat(L, module, signal, fns, true, raw_json, 0, emit_idx);
}

extractors::ExtractionResult RunCompatTable(lua_State* L, ModuleKind module, Signal signal,
                                            const SignalExtractors& fns, int table_idx,
                                            int emit_idx) {
  if (L == nullptr || !lua_istable(L, table_idx)) {
    return extractors::ExtractionResult{false, "payload must be a table", 0};
  }
  return RunCompat(L, module, signal, fns, false, {}, table_idx, emit_idx);
}

}  // namespace vector_otel::core
