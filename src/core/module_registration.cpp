#include "vector_otel/core/module_runner.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace vector_otel::core {

// RegisterModulesInPackageLoaded: atomically register up to 4 modules.
// For now, this is a simple implementation without full failure rollback.
// The spec requires bounded transactional semantics with zero-allocation rollback.
int RegisterModulesInPackageLoaded(lua_State* L,
                                   const ModuleRegistration* registrations,
                                   int count) {
  if (count < 1 || count > 4 || !registrations) {
    return luaL_error(L, "RegisterModulesInPackageLoaded: invalid count");
  }

  luaL_checkstack(L, 50, "module registration");

  // Register each module in both targets (registry and global package.loaded)
  for (int i = 0; i < count; ++i) {
    const ModuleRegistration& reg = registrations[i];
    int abs_idx = lua_absindex(L, reg.table_index);

    // 1. Register in registry[LUA_LOADED_TABLE]
    luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_pushvalue(L, abs_idx);
    lua_setfield(L, -2, reg.name);
    lua_pop(L, 1);

    // 2. Also populate global package.loaded if package library is loaded
    lua_getglobal(L, "package");
    if (lua_istable(L, -1)) {
      lua_getfield(L, -1, "loaded");
      if (lua_istable(L, -1)) {
        lua_pushvalue(L, abs_idx);
        lua_setfield(L, -2, reg.name);
      }
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
  }

  return 0;
}

}  // namespace vector_otel::core
