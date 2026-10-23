#include "vector_otel/core/module_runner.hpp"

namespace vector_otel::core {
namespace {
constexpr int kRegistryKey = 1;
constexpr int kOldRegistry = 2;
constexpr int kRegistryTarget = 3;
constexpr int kGlobalTarget = 4;
constexpr int kNewRegistry = 5;
constexpr int kRecords = 6;
constexpr int kStart = 7;
constexpr int kStride = 4;

struct Transaction {
  ModuleRegistration entries[4];
  int count;
};

int Prepare(lua_State* L) {
  const auto* tx = static_cast<const Transaction*>(lua_touserdata(L, 1));
  lua_createtable(L, kStart + 8 * kStride, 0);
  const int journal = lua_gettop(L);
  lua_pushliteral(L, LUA_LOADED_TABLE);
  lua_pushvalue(L, -1);
  lua_rawget(L, LUA_REGISTRYINDEX);
  lua_pushvalue(L, -1);
  lua_rawseti(L, journal, kOldRegistry);
  const bool create = !lua_istable(L, -1);
  if (create) {
    lua_pop(L, 1);
    lua_newtable(L);
  }
  lua_rawseti(L, journal, kRegistryTarget);
  lua_rawseti(L, journal, kRegistryKey);
  lua_pushboolean(L, create);
  lua_rawseti(L, journal, kNewRegistry);

  lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
  lua_pushliteral(L, "package");
  lua_rawget(L, -2);
  if (lua_istable(L, -1)) {
    lua_pushliteral(L, "loaded");
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
      lua_rawgeti(L, journal, kRegistryTarget);
      const bool same = lua_rawequal(L, -1, -2);
      lua_pop(L, 1);
      if (!same) {
        lua_pushvalue(L, -1);
        lua_rawseti(L, journal, kGlobalTarget);
      }
    }
  }
  lua_settop(L, journal);

  int records = 0;
  for (int target_slot = kRegistryTarget; target_slot <= kGlobalTarget; ++target_slot) {
    lua_rawgeti(L, journal, target_slot);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      continue;
    }
    const int target = lua_gettop(L);
    for (int i = 0; i < tx->count; ++i) {
      const int slot = kStart + records++ * kStride;
      lua_pushvalue(L, target);
      lua_rawseti(L, journal, slot);
      lua_pushstring(L, tx->entries[i].name);
      lua_pushvalue(L, -1);
      lua_rawget(L, target);
      lua_rawseti(L, journal, slot + 2);
      lua_rawseti(L, journal, slot + 1);
      lua_pushvalue(L, i + 2);
      lua_rawseti(L, journal, slot + 3);
    }
    lua_pop(L, 1);
  }
  lua_pushinteger(L, records);
  lua_rawseti(L, journal, kRecords);
  return 1;
}

int Publish(lua_State* L) {
  lua_rawgeti(L, 1, kRecords);
  const int count = static_cast<int>(lua_tointeger(L, -1));
  lua_pop(L, 1);
  for (int i = 0; i < count; ++i) {
    const int slot = kStart + i * kStride;
    lua_rawgeti(L, 1, slot);
    lua_rawgeti(L, 1, slot + 1);
    lua_rawgeti(L, 1, slot + 3);
    lua_rawset(L, -3);
    lua_pop(L, 1);
  }
  lua_rawgeti(L, 1, kNewRegistry);
  const bool install = lua_toboolean(L, -1);
  lua_pop(L, 1);
  if (install) {
    lua_rawgeti(L, 1, kRegistryKey);
    lua_rawgeti(L, 1, kRegistryTarget);
    lua_rawset(L, LUA_REGISTRYINDEX);
  }
  return 0;
}

// Existing-value replacement and nil deletion use existing Lua 5.4 table slots;
// writing nil to an absent slot does not insert it. All keys/values are rooted.
void Rollback(lua_State* L, int journal) {
  lua_rawgeti(L, journal, kRecords);
  const int count = static_cast<int>(lua_tointeger(L, -1));
  lua_pop(L, 1);
  for (int i = 0; i < count; ++i) {
    const int slot = kStart + i * kStride;
    lua_rawgeti(L, journal, slot);
    lua_rawgeti(L, journal, slot + 1);
    lua_rawgeti(L, journal, slot + 2);
    lua_rawset(L, -3);
    lua_pop(L, 1);
  }
  // A new registry target is installed last, so failure leaves the old slot
  // untouched. No fallible work follows its successful installation.
}
}  // namespace

int RegisterModulesInPackageLoaded(lua_State* L, const ModuleRegistration* registrations,
                                   int count) {
  if (!registrations || count < 1 || count > 4) {
    return luaL_error(L, "invalid module registration count");
  }
  const int base = lua_gettop(L);
  Transaction tx{};
  tx.count = count;
  for (int i = 0; i < count; ++i) {
    tx.entries[i] = {registrations[i].name, lua_absindex(L, registrations[i].table_index)};
  }
  luaL_checkstack(L, 64, "module registration");
  lua_pushcfunction(L, Prepare);
  lua_pushlightuserdata(L, &tx);
  for (int i = 0; i < count; ++i) lua_pushvalue(L, tx.entries[i].table_index);
  if (lua_pcall(L, count + 1, 1, 0) != LUA_OK) return lua_error(L);
  const int journal = lua_gettop(L);
  lua_pushcfunction(L, Publish);
  lua_pushvalue(L, journal);
  if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
#ifdef VECTOR_OTEL_TEST_REGISTRATION_OBSERVER
    if (tls_registration_rollback_observer) tls_registration_rollback_observer(L, journal, true);
#endif
    Rollback(L, journal);
#ifdef VECTOR_OTEL_TEST_REGISTRATION_OBSERVER
    if (tls_registration_rollback_observer) tls_registration_rollback_observer(L, journal, false);
#endif
    lua_remove(L, journal);
    return lua_error(L);
  }
  lua_settop(L, base);
  return 0;
}
}  // namespace vector_otel::core
