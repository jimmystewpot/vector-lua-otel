#include "vector_otel/core/module_state.hpp"

#include <cstdio>

#include "vector_otel/core/read_keys.hpp"

namespace vector_otel::core {

namespace {

// Distinct addresses used as private registry keys. Internal linkage: each shared module owns its
// own keys, and the two module kinds can never collide.
// Non-const so each key is a distinct, unmergeable object.
char kOtelKey = 0;
char kNativeKey = 0;

const void* KeyFor(ModuleKind kind) noexcept {
  return kind == ModuleKind::Otel ? static_cast<const void*>(&kOtelKey)
                                  : static_cast<const void*>(&kNativeKey);
}

constexpr int ImmutableIndex(ImmutableSlot slot) noexcept { return 1 + static_cast<int>(slot); }
constexpr int InvocationIndex(AnchorSlot slot) noexcept {
  return 1 + kImmutableSlotCount + static_cast<int>(slot);
}

}  // namespace

std::size_t FormatPayloadLimitText(char* buffer, std::size_t capacity) noexcept {
  const int n = std::snprintf(buffer, capacity, "payload exceeds %zu-byte uncompressed JSON limit",
                              static_cast<std::size_t>(kMaxPayloadBytes));
  if (n < 0) {
    buffer[0] = '\0';
    return 0;
  }
  return static_cast<std::size_t>(n) < capacity ? static_cast<std::size_t>(n) : capacity - 1;
}

namespace {

// Runs under lua_pcall with only trivial automatic objects. Builds the anchor and publishes it as
// the LAST operation, so a failure anywhere leaves nothing published.
int BootstrapLeaf(lua_State* L) {
  const auto kind = static_cast<ModuleKind>(lua_tointeger(L, 1));
  lua_createtable(L, kImmutableSlotCount + kInvocationSlotCount, 0);  // anchor at index 2

  char limit_text[96];
  const std::size_t limit_len = FormatPayloadLimitText(limit_text, sizeof(limit_text));
  lua_pushlstring(L, limit_text, limit_len);
  lua_rawseti(L, 2, ImmutableIndex(ImmutableSlot::kPayloadLimitMessage));

  struct Fixed {
    ImmutableSlot slot;
    std::string_view text;
  };
  const Fixed fixed[] = {
      {ImmutableSlot::kRecordLimitMessage, kRecordLimitText},
      {ImmutableSlot::kMemoryMessage, kMemoryText},
      {ImmutableSlot::kLuaMessage, kLuaText},
      {ImmutableSlot::kExtractionMessage, kExtractionText},
      {ImmutableSlot::kPayloadArgMessage, kPayloadArgText},
      {ImmutableSlot::kEmitArgMessage, kEmitArgText},
      {ImmutableSlot::kReentrantMessage, kReentrantText},
      {ImmutableSlot::kStackMessage, kStackText},
  };
  static_assert(
      sizeof(fixed) / sizeof(fixed[0]) + 2 == static_cast<std::size_t>(ImmutableSlot::kCount),
      "every immutable slot must be interned (payload limit and read keys are built separately)");
  for (const Fixed& f : fixed) {
    lua_pushlstring(L, f.text.data(), f.text.size());
    lua_rawseti(L, 2, ImmutableIndex(f.slot));
  }

  // Root every table-reader key so lua_pushstring() on it can never allocate later.
  lua_createtable(L, static_cast<int>(kReadKeyCount), 0);
  for (std::size_t i = 0; i < kReadKeyCount; ++i) {
    lua_pushlstring(L, kReadKeyList[i].data(), kReadKeyList[i].size());
    lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
  }
  lua_rawseti(L, 2, ImmutableIndex(ImmutableSlot::kReadKeys));

  for (int i = 0; i < kInvocationSlotCount; ++i) {
    lua_pushboolean(L, 0);
    lua_rawseti(L, 2, InvocationIndex(static_cast<AnchorSlot>(i)));
  }

  lua_pushvalue(L, 2);
  lua_rawsetp(L, LUA_REGISTRYINDEX, KeyFor(kind));  // publish last
  return 1;
}

}  // namespace

bool EnsureModuleState(lua_State* L, ModuleKind kind) noexcept {
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, KeyFor(kind)) == LUA_TTABLE) {
    return true;
  }
  lua_pop(L, 1);
  lua_pushcfunction(L, BootstrapLeaf);
  lua_pushinteger(L, static_cast<lua_Integer>(kind));
  return lua_pcall(L, 1, 1, 0) == LUA_OK;
}

int OpenSignalTable(lua_State* L, ModuleKind kind, const luaL_Reg* funcs) {
  if (!EnsureModuleState(L, kind)) {
    return lua_error(L);  // no C++ owners on this frame
  }
  lua_createtable(L, 0, 1);
  lua_insert(L, -2);  // table, anchor
  luaL_setfuncs(L, funcs, 1);
  return 1;
}

void SetAnchor(lua_State* L, int anchor_index, AnchorSlot slot) noexcept {
  lua_rawseti(L, anchor_index, InvocationIndex(slot));
}

void ClearAnchor(lua_State* L, int anchor_index, AnchorSlot slot) noexcept {
  lua_pushboolean(L, 0);
  lua_rawseti(L, anchor_index, InvocationIndex(slot));
}

void PushAnchor(lua_State* L, int anchor_index, AnchorSlot slot) noexcept {
  lua_rawgeti(L, anchor_index, InvocationIndex(slot));
}

int AnchorType(lua_State* L, int anchor_index, AnchorSlot slot) noexcept {
  const int type = lua_rawgeti(L, anchor_index, InvocationIndex(slot));
  lua_pop(L, 1);
  return type;
}

void PushImmutable(lua_State* L, int anchor_index, ImmutableSlot slot) noexcept {
  lua_rawgeti(L, anchor_index, ImmutableIndex(slot));
}

}  // namespace vector_otel::core
