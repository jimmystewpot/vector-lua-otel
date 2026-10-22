#pragma once

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace vector_otel::core {

inline thread_local bool tls_active_extraction = false;

struct ActiveExtractionGuard {
  ActiveExtractionGuard() noexcept { tls_active_extraction = true; }
  ~ActiveExtractionGuard() noexcept { tls_active_extraction = false; }
  ActiveExtractionGuard(const ActiveExtractionGuard&) = delete;
  ActiveExtractionGuard& operator=(const ActiveExtractionGuard&) = delete;
};

inline void RegisterInPackageLoaded(lua_State* L, const char* name, int table_idx) {
  int abs_idx = lua_absindex(L, table_idx);

  // 1. In Lua 5.2+, package.loaded is registered in registry[LUA_LOADED_TABLE]
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  lua_pushvalue(L, abs_idx);
  lua_setfield(L, -2, name);
  lua_pop(L, 1);

  // 2. Also populate global package.loaded if package library is loaded
  lua_getglobal(L, "package");
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, "loaded");
    if (lua_istable(L, -1)) {
      lua_pushvalue(L, abs_idx);
      lua_setfield(L, -2, name);
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
}

}  // namespace vector_otel::core
