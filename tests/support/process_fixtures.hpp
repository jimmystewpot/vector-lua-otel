#pragma once

// Process-level harness for the fault target: loads the real module into a RawState (fault
// allocator, no standard library), calls `<module>.<signal>.process(payload, emit)` under
// lua_pcall and records what the emit callback saw. The callback is a plain C function; its
// recorder lives in upvalue 1 as a light userdata, and counting-only mode never allocates (so it
// is safe inside a C++ fault scope).

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <lua.hpp>
#include <string>
#include <utility>
#include <vector>

#include "operation_fixtures.hpp"

extern "C" {
int luaopen_vector_otel(lua_State* L);
int luaopen_vector_native(lua_State* L);
}

namespace vector_otel::core::fixtures {

struct ProcRecorder {
  bool dump = false;  // record event text and shared-table identity (allocates)
  int calls = 0;
  int rows = 0;
  int fail_at_call = -1;    // zero-based call that raises
  int reenter_result = -1;  // reentrancy probe: 0 = no probe, 1 = inner call returned (nil,msg)
  bool reenter = false;     // the first call re-invokes process() from inside the callback
  std::string reenter_message;
  std::vector<std::string> events;
  std::vector<const void*> resource_ptrs;
  std::vector<const void*> scope_ptrs;
};

inline void DumpValue(lua_State* L, int idx, std::string& out) {
  idx = lua_absindex(L, idx);
  switch (lua_type(L, idx)) {
    case LUA_TSTRING: {
      std::size_t n = 0;
      const char* s = lua_tolstring(L, idx, &n);
      out += '"';
      out.append(s, n);
      out += '"';
      break;
    }
    case LUA_TNUMBER:
      if (lua_isinteger(L, idx)) {
        out += std::to_string(lua_tointeger(L, idx));
      } else {
        out += std::to_string(lua_tonumber(L, idx)) + "f";
      }
      break;
    case LUA_TBOOLEAN:
      out += lua_toboolean(L, idx) ? "true" : "false";
      break;
    case LUA_TTABLE: {
      const lua_Unsigned n = lua_rawlen(L, idx);
      if (n > 0) {
        out += '[';
        for (lua_Unsigned i = 1; i <= n; ++i) {
          if (i > 1) out += ',';
          lua_rawgeti(L, idx, static_cast<lua_Integer>(i));
          DumpValue(L, -1, out);
          lua_pop(L, 1);
        }
        out += ']';
        break;
      }
      std::vector<std::string> fields;
      lua_pushnil(L);
      while (lua_next(L, idx) != 0) {
        std::string field;
        DumpValue(L, -2, field);
        field += ':';
        DumpValue(L, -1, field);
        fields.push_back(std::move(field));
        lua_pop(L, 1);
      }
      std::sort(fields.begin(), fields.end());
      out += '{';
      for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) out += ',';
        out += fields[i];
      }
      out += '}';
      break;
    }
    default:
      out += "?";
  }
}

// Notes the shared-table identity of one row table (index `row`).
inline void NoteRowTables(lua_State* L, int row, ProcRecorder& rec) {
  row = lua_absindex(L, row);
  lua_pushstring(L, "ResourceAttributes");
  rec.resource_ptrs.push_back(lua_rawget(L, row) == LUA_TTABLE ? lua_topointer(L, -1) : nullptr);
  lua_pop(L, 1);
  lua_pushstring(L, "ScopeAttributes");
  rec.scope_ptrs.push_back(lua_rawget(L, row) == LUA_TTABLE ? lua_topointer(L, -1) : nullptr);
  lua_pop(L, 1);
}

inline int ProcRecordEmit(lua_State* L) {
  auto* rec = static_cast<ProcRecorder*>(lua_touserdata(L, lua_upvalueindex(1)));
  const int call = rec->calls++;
  if (call == rec->fail_at_call) {
    lua_pushliteral(L, "boom");
    return lua_error(L);
  }
  int rows = 1;
  if (lua_type(L, 1) == LUA_TTABLE) {
    lua_pushstring(L, "log");
    if (lua_rawget(L, 1) == LUA_TTABLE) {
      lua_pushstring(L, "records");
      if (lua_rawget(L, -2) == LUA_TTABLE) {
        rows = static_cast<int>(lua_rawlen(L, -1));
        if (rec->dump) {
          for (int i = 1; i <= rows; ++i) {
            lua_rawgeti(L, -1, i);
            NoteRowTables(L, -1, *rec);
            lua_pop(L, 1);
          }
        }
      } else if (rec->dump) {
        NoteRowTables(L, -2, *rec);
      }
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
  }
  rec->rows += rows;
  if (rec->dump) {
    std::string text;
    DumpValue(L, 1, text);
    rec->events.push_back(std::move(text));
  }
  if (rec->reenter && call == 0 && lua_isfunction(L, lua_upvalueindex(2))) {
    lua_pushvalue(L, lua_upvalueindex(2));
    lua_pushstring(L, "{}");
    lua_pushvalue(L, lua_upvalueindex(3));
    lua_call(L, 2, LUA_MULTRET);
    rec->reenter_result = lua_isnil(L, -3) ? 1 : 0;
    rec->reenter_message = lua_tostring(L, -2) != nullptr ? lua_tostring(L, -2) : "";
    lua_settop(L, 1);
  }
  return 0;
}

struct ProcResult {
  int status = LUA_OK;
  int nresults = 0;
  bool first_nil = false;
  lua_Integer count = -1;  // success: result 1; failure: result 3 (partial count)
  std::string error;       // failure: result 2
};

enum class PayloadKind { kJson, kTable, kRaw };

// A payload is JSON text, a Lua chunk that returns the table input, or "nothing / wrong type".
struct Payload {
  PayloadKind kind = PayloadKind::kJson;
  std::string text;
  int raw_type = LUA_TNIL;  // kRaw: nil / number
};

inline Payload JsonPayload(std::string s) { return {PayloadKind::kJson, std::move(s), 0}; }
inline Payload TablePayload(std::string chunk) {
  return {PayloadKind::kTable, std::move(chunk), 0};
}

// Loads the module into `s` once; returns the absolute index of `<module>.<signal>.process`
// (pushed fresh on every call).
inline void PushProcessFunction(lua_State* L, bool native, const char* signal) {
  if (native) {
    luaopen_vector_native(L);
  } else {
    luaopen_vector_otel(L);
  }
  lua_getfield(L, -1, signal);
  lua_getfield(L, -1, "process");
  lua_remove(L, -2);
  lua_remove(L, -2);
}

// `around(run)` invokes `run()` (the lua_pcall) possibly inside a fault scope.
template <typename Around>
inline ProcResult CallProcess(RawState& s, bool native, const char* signal, const Payload& payload,
                              ProcRecorder& rec, Around around, bool pass_emit = true) {
  lua_State* L = s.L();
  ProcResult r;
  const int base = lua_gettop(L);
  PushProcessFunction(L, native, signal);
  const int fn = lua_gettop(L);
  if (payload.kind == PayloadKind::kJson) {
    lua_pushlstring(L, payload.text.data(), payload.text.size());
  } else if (payload.kind == PayloadKind::kTable) {
    if (luaL_loadstring(L, payload.text.c_str()) != LUA_OK || lua_pcall(L, 0, 1, 0) != LUA_OK) {
      ADD_FAILURE() << "bad table chunk: " << lua_tostring(L, -1);
      lua_settop(L, base);
      return r;
    }
  } else if (payload.raw_type == LUA_TNUMBER) {
    lua_pushinteger(L, 7);
  } else {
    lua_pushnil(L);
  }
  if (pass_emit) {
    lua_pushlightuserdata(L, &rec);
    lua_pushvalue(L, fn);
    lua_pushnil(L);  // upvalue 3 (the emit itself) is patched below for the reentrancy probe
    lua_pushcclosure(L, &ProcRecordEmit, 3);
    lua_pushvalue(L, -1);
    lua_setupvalue(L, -2, 3);
  } else {
    lua_pushinteger(L, 5);  // not a function
  }
  const int top_before = lua_gettop(L);
  (void)top_before;
  around([&] { r.status = lua_pcall(L, 2, LUA_MULTRET, 0); });
  r.nresults = lua_gettop(L) - base - 0;
  if (r.status == LUA_OK) {
    r.nresults = lua_gettop(L) - (fn - 1);
    const int first = fn;
    r.first_nil = lua_isnil(L, first);
    if (r.nresults == 2) {
      r.count = lua_tointeger(L, first);
    } else if (r.nresults == 3) {
      if (lua_type(L, first + 1) == LUA_TSTRING) r.error = lua_tostring(L, first + 1);
      r.count = lua_tointeger(L, first + 2);
    }
  }
  lua_settop(L, base);
  return r;
}

inline ProcResult CallProcess(RawState& s, bool native, const char* signal, const Payload& payload,
                              ProcRecorder& rec) {
  return CallProcess(s, native, signal, payload, rec, [](auto&& run) { run(); });
}

}  // namespace vector_otel::core::fixtures
