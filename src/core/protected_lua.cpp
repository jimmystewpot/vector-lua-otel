#include "vector_otel/core/protected_lua.hpp"

namespace vector_otel::core {

namespace {

ImmutableSlot FallbackFor(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::PayloadLimit:
      return ImmutableSlot::kPayloadLimitMessage;
    case ErrorCode::RecordLimit:
      return ImmutableSlot::kRecordLimitMessage;
    case ErrorCode::Memory:
      return ImmutableSlot::kMemoryMessage;
    case ErrorCode::Lua:
      return ImmutableSlot::kLuaMessage;
    case ErrorCode::Reentrant:
      return ImmutableSlot::kReentrantMessage;
    case ErrorCode::Stack:
      return ImmutableSlot::kStackMessage;
    case ErrorCode::None:
    case ErrorCode::Json:
    case ErrorCode::Schema:
    case ErrorCode::Cpp:
    case ErrorCode::Argument:
      break;
  }
  return ImmutableSlot::kExtractionMessage;
}

// Protected leaf building (nil, message, count). Only trivial automatic objects.
int ReturnLeaf(lua_State* L) {
  const OperationContext& ctx = LeafContext(L);
  lua_pushnil(L);
  const std::string_view message = ctx.error.view();
  if (message.empty()) {
    PushImmutable(L, kLeafAnchorIndex, ImmutableSlot::kExtractionMessage);
  } else {
    lua_pushlstring(L, message.data(), message.size());
  }
  lua_pushinteger(L, ctx.delivered_count);
  return 3;
}

// Called with the failed call's error object on top of the stack; consumes it.
void CaptureLuaError(OperationContext& ctx, int status) noexcept {
  lua_State* L = ctx.L;
  if (status == LUA_ERRMEM) {
    ctx.error.Set(ErrorCode::Memory, kMemoryText);
  } else if (lua_type(L, -1) == LUA_TSTRING) {
    // A string is read in place; other object types are never coerced (that would allocate).
    std::size_t len = 0;
    const char* text = lua_tolstring(L, -1, &len);
    ctx.error.Set(ErrorCode::Lua, std::string_view(text, len));
  } else {
    ctx.error.Set(ErrorCode::Lua, kLuaText);
  }
  SetAnchor(L, ctx.anchor_index, AnchorSlot::kPendingError);  // roots the object, pops it
}

}  // namespace

bool RunLuaLeaf(OperationContext& ctx, lua_CFunction leaf, void* payload) noexcept {
  lua_State* L = ctx.L;
  if (!lua_checkstack(L, 4)) [[unlikely]] {
    ctx.error.Set(ErrorCode::Stack, kStackText);
    return false;
  }
  ctx.leaf_payload = payload;
  lua_pushcfunction(L, leaf);  // light C function: no allocation
  lua_pushlightuserdata(L, &ctx);
  lua_pushvalue(L, ctx.anchor_index);
  const int status = lua_pcall(L, 2, 0, 0);
  ctx.leaf_payload = nullptr;
  if (status == LUA_OK) [[likely]] {
    return true;
  }
  CaptureLuaError(ctx, status);
  return false;
}

void ClearInvocationRoots(OperationContext& ctx) noexcept {
  lua_State* L = ctx.L;
  ctx.leaf_payload = nullptr;
  lua_settop(L, ctx.base_top);
  for (int i = 0; i < kInvocationSlotCount; ++i) {
    const auto slot = static_cast<AnchorSlot>(i);
    if (slot != AnchorSlot::kPendingError) {
      ClearAnchor(L, ctx.anchor_index, slot);
    }
  }
}

int ReturnOperationResult(OperationContext& ctx) noexcept {
  lua_State* L = ctx.L;
  lua_settop(L, ctx.base_top);
  if (ctx.error.code == ErrorCode::None) {
    lua_pushinteger(L, ctx.delivered_count);
    lua_pushnil(L);
    ClearAnchor(L, ctx.anchor_index, AnchorSlot::kPendingError);
    return 2;
  }
  lua_pushcfunction(L, ReturnLeaf);
  lua_pushlightuserdata(L, &ctx);
  lua_pushvalue(L, ctx.anchor_index);
  if (lua_pcall(L, 2, 3, 0) != LUA_OK) {
    // Lua could not build the detailed message: discard the error and return the pre-interned
    // fallback. These pushes neither allocate nor raise.
    lua_settop(L, ctx.base_top);
    lua_pushnil(L);
    PushImmutable(L, ctx.anchor_index, FallbackFor(ctx.error.code));
    lua_pushinteger(L, ctx.delivered_count);
  }
  ClearAnchor(L, ctx.anchor_index, AnchorSlot::kPendingError);
  return 3;
}

}  // namespace vector_otel::core
