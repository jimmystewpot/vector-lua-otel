#include "contract_fixtures.hpp"

#include <string>

extern "C" {
int luaopen_vector_otel(lua_State* L);
int luaopen_vector_native(lua_State* L);
}

namespace vector_otel::test {

ModuleState::ModuleState() {
  L_ = alloc_.NewState();
  if (L_ == nullptr) {
    return;
  }
  luaL_openlibs(L_);
  luaL_requiref(L_, "vector_otel", luaopen_vector_otel, 1);
  lua_pop(L_, 1);
  luaL_requiref(L_, "vector_native", luaopen_vector_native, 1);
  lua_pop(L_, 1);
}

ModuleState::~ModuleState() {
  if (L_ != nullptr) {
    lua_close(L_);
  }
}

std::size_t CountRegistryRoots(lua_State* L) {
  std::size_t roots = 0;
  luaL_checkstack(L, 4, "CountRegistryRoots");
  lua_pushnil(L);
  while (lua_next(L, LUA_REGISTRYINDEX) != 0) {
    const bool integer_key = lua_type(L, -2) == LUA_TNUMBER && lua_isinteger(L, -2);
    if (integer_key && lua_tointeger(L, -2) > LUA_RIDX_LAST && lua_type(L, -1) != LUA_TNUMBER) {
      ++roots;
    }
    lua_pop(L, 1);
  }
  return roots;
}

void FullCollect(lua_State* L) {
  lua_gc(L, LUA_GCCOLLECT);
  lua_gc(L, LUA_GCCOLLECT);
}

StateSnapshot TakeSnapshot(ModuleState& state) {
  FullCollect(state.L());
  return StateSnapshot{CountRegistryRoots(state.L()), state.allocator().live_bytes()};
}

void RowCounter::PushEmit(lua_State* L) {
  lua_pushlightuserdata(L, this);
  lua_pushcclosure(L, &RowCounter::Emit, 1);
}

int RowCounter::Emit(lua_State* L) {
  auto* self = static_cast<RowCounter*>(lua_touserdata(L, lua_upvalueindex(1)));
  ++self->entered_;
  if (self->completed_ >= self->fail_after_) {
    return luaL_error(L, "injected emit failure");
  }
  ++self->completed_;
  return 0;
}

ProcessResult CallProcess(lua_State* L, const char* module, const char* signal,
                          const std::string& payload, RowCounter& counter) {
  ProcessResult out;
  const int base = lua_gettop(L);
  // Look the function up with explicit type checks: lua_getfield on a non-table would raise
  // outside lua_pcall and panic the whole test binary.
  lua_getglobal(L, module);
  if (!lua_istable(L, -1)) {
    out.status = LUA_ERRRUN;
    out.error = std::string("module not loaded: ") + module;
    lua_settop(L, base);
    return out;
  }
  lua_getfield(L, -1, signal);
  if (!lua_istable(L, -1)) {
    out.status = LUA_ERRRUN;
    out.error = std::string("signal not found: ") + module + "." + signal;
    lua_settop(L, base);
    return out;
  }
  lua_getfield(L, -1, "process");
  if (!lua_isfunction(L, -1)) {
    out.status = LUA_ERRRUN;
    out.error = std::string("process not a function: ") + module + "." + signal;
    lua_settop(L, base);
    return out;
  }
  lua_remove(L, -2);
  lua_remove(L, -2);
  lua_pushlstring(L, payload.data(), payload.size());
  counter.PushEmit(L);
  out.status = lua_pcall(L, 2, LUA_MULTRET, 0);
  if (out.status == LUA_OK) {
    out.nresults = lua_gettop(L) - base;
    if (out.nresults >= 1) {
      out.first_is_nil = lua_isnil(L, base + 1);
      if (lua_isinteger(L, base + 1)) {
        out.count = lua_tointeger(L, base + 1);
      }
    }
    if (out.nresults >= 2) {
      out.second_is_nil = lua_isnil(L, base + 2);
      if (lua_type(L, base + 2) == LUA_TSTRING) {
        out.error = lua_tostring(L, base + 2);
      }
    }
    if (out.nresults >= 3 && lua_isinteger(L, base + 3)) {
      out.partial = lua_tointeger(L, base + 3);
    }
  } else if (lua_type(L, -1) == LUA_TSTRING) {
    out.error = lua_tostring(L, -1);
  }
  lua_settop(L, base);
  return out;
}

std::string MakeOtlpLogsJson(std::size_t records) {
  std::string json =
      "{\"resourceLogs\":[{\"resource\":{\"attributes\":[{\"key\":\"service.name\",\"value\":"
      "{\"stringValue\":\"svc\"}}]},\"scopeLogs\":[{\"scope\":{\"name\":\"scope\"},\"logRecords\":"
      "[";
  for (std::size_t i = 0; i < records; ++i) {
    if (i != 0) {
      json += ',';
    }
    json +=
        "{\"timeUnixNano\":\"1700000000000000000\",\"severityText\":\"INFO\",\"body\":"
        "{\"stringValue\":\"message-" +
        std::to_string(i) + "\"}}";
  }
  json += "]}]}]}";
  return json;
}

}  // namespace vector_otel::test
