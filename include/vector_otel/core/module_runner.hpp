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

// ModuleRegistration represents a single module entry for atomic registration.
// name: module name (e.g., "vector_otel.logs")
// table_index: stack index of the module table to register (will be converted to
// absolute)
struct ModuleRegistration {
  const char* name;
  int table_index;
};

#ifdef VECTOR_OTEL_TEST_REGISTRATION_OBSERVER
// Fault-target-only observer; never compiled into either production module.
inline thread_local void (*tls_registration_rollback_observer)(lua_State*, int, bool) = nullptr;
#endif

// RegisterModulesInPackageLoaded atomically registers up to 4 modules across up
// to 2 distinct loaded-table targets (registry LUA_LOADED_TABLE and/or global
// package.loaded). On failure, all targets are restored to their exact pre-call
// state with zero allocations during rollback.
//
// Reserves and roots its own bounded stack workspace before public mutation.
// Like luaopen_*, errors must be caught by a caller-owned Lua protection boundary.
//
// Parameters:
//   L: Lua state
//   registrations: array of up to 4 ModuleRegistration entries
//   count: number of entries (1-4)
//
// Returns: 0 on success, or longjmp error on failure (restored state).
int RegisterModulesInPackageLoaded(lua_State* L, const ModuleRegistration* registrations,
                                   int count);

// RegisterInPackageLoaded is a compatibility wrapper for single-entry registration.
// It uses RegisterModulesInPackageLoaded internally.
inline void RegisterInPackageLoaded(lua_State* L, const char* name, int table_idx) {
  ModuleRegistration reg = {name, table_idx};
  RegisterModulesInPackageLoaded(L, &reg, 1);
}

}  // namespace vector_otel::core
