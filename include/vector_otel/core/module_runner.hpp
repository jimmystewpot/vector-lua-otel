#pragma once

#include <cstddef>
#include <exception>
#include <string_view>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "vector_otel/core/parser.hpp"
#include "vector_otel/extractors/logs_extractor.hpp"

namespace vector_otel::core {

using ExtractorStringFn = extractors::ExtractionResult (*)(lua_State*, std::string_view, int);
using ExtractorTableFn = extractors::ExtractionResult (*)(lua_State*, int, int);

inline thread_local bool tls_active_extraction = false;

struct ActiveExtractionGuard {
  ActiveExtractionGuard() noexcept { tls_active_extraction = true; }
  ~ActiveExtractionGuard() noexcept { tls_active_extraction = false; }
  ActiveExtractionGuard(const ActiveExtractionGuard&) = delete;
  ActiveExtractionGuard& operator=(const ActiveExtractionGuard&) = delete;
};

template <ExtractorStringFn StringExtractor, ExtractorTableFn TableExtractor>
int ProcessBody(lua_State* L) {
  try {
    int arg_type = lua_type(L, 1);
    if (arg_type != LUA_TSTRING && arg_type != LUA_TTABLE) {
      lua_pushnil(L);
      lua_pushstring(L, "payload must be a string or table");
      lua_pushinteger(L, 0);
      return 3;
    }
    if (!lua_isfunction(L, 2)) {
      lua_pushnil(L);
      lua_pushstring(L, "emit callback must be a function");
      lua_pushinteger(L, 0);
      return 3;
    }

    extractors::ExtractionResult result;
    if (arg_type == LUA_TSTRING) {
      struct ParserTrimGuard {
        ~ParserTrimGuard() noexcept { OtelParser::TrimThreadLocalBuffer(); }
      } parser_trim_guard;
      size_t len = 0;
      const char* str = lua_tolstring(L, 1, &len);
      std::string_view payload(str, len);
      result = StringExtractor(L, payload, 2);
    } else {
      result = TableExtractor(L, 1, 2);
    }

    if (result.success) {
      lua_pushinteger(L, result.emitted_count);
      lua_pushnil(L);
      return 2;
    }

    lua_pushnil(L);
    if (result.error.empty()) {
      lua_pushstring(L, "unknown extraction error");
    } else {
      lua_pushlstring(L, result.error.data(), result.error.size());
    }
    lua_pushinteger(L, result.emitted_count);
    return 3;
  } catch (const std::exception& e) {
    lua_pushnil(L);
    lua_pushstring(L, e.what());
    lua_pushinteger(L, 0);
    return 3;
  } catch (...) {
    lua_pushnil(L);
    lua_pushstring(L, "unknown C++ exception");
    lua_pushinteger(L, 0);
    return 3;
  }
}

template <ExtractorStringFn StringExtractor, ExtractorTableFn TableExtractor>
int ProcessDispatcher(lua_State* L) {
  if (tls_active_extraction) [[unlikely]] {
    lua_pushnil(L);
    lua_pushstring(L, "re-entrant vector_otel.process() invocation is not permitted");
    lua_pushinteger(L, 0);
    return 3;
  }

  int status = LUA_OK;
  {
    // CRITICAL: The ActiveExtractionGuard must live in THIS frame (outer).
    // If we put it inside ProcessBody and Lua raises a memory error
    // (lua_error/longjmp via a failing allocator), the C++ destructor never
    // runs, leaving tls_active_extraction=true. By running the extraction
    // through lua_pcall here, we catch the longjmp BEFORE returning from
    // this frame, ensuring the guard is destroyed.
    ActiveExtractionGuard extraction_guard;

    lua_CFunction body_fn = ProcessBody<StringExtractor, TableExtractor>;
    lua_pushcfunction(L, body_fn);
    lua_insert(L, 1);  // Move the C function to stack position 1
    status = lua_pcall(L, lua_gettop(L) - 1, LUA_MULTRET, 0);
  }  // Guard is destroyed here, BEFORE anything that can allocate

  if (status != LUA_OK) {
    // lua_pcall failed; error object is on top of stack
    lua_pushnil(L);
    lua_insert(L, -2);  // nil, error_msg
    lua_pushinteger(L, 0);
    return 3;
  }

  return lua_gettop(L);  // Return all values that ProcessBody pushed
}

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
