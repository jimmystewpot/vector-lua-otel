#include "vector_otel/core/lua_bridge.hpp"

namespace vector_otel {
namespace core {

LuaEmitter::LuaEmitter(lua_State* L, int emit_index, size_t batch_size)
    : L_(L),
      emit_index_(0),
      emitted_count_(0),
      batch_size_(batch_size < 1 ? 1 : batch_size),
      current_batch_count_(0),
      in_record_(false),
      record_index_(0),
      event_index_(0),
      event_ref_(L, LUA_NOREF),
      records_ref_(L, LUA_NOREF) {
  if (L_ != nullptr) {
    emit_index_ = lua_absindex(L_, emit_index);
  }
}

void LuaEmitter::StartRecord() {
  if (L_ == nullptr || in_record_) {
    return;
  }
  in_record_ = true;

  if (batch_size_ == 1) {
    // Flat single event mode (N = 1)
    // Pre-push emit function, event table, and record (log) table
    lua_pushvalue(L_, emit_index_);  // emit function
    lua_createtable(L_, 0, 1);       // event table
    lua_createtable(L_, 0, 16);      // log table / record
    record_index_ = lua_gettop(L_);
    event_index_ = record_index_ - 1;
  } else {
    // Batch event mode (N > 1)
    if (current_batch_count_ == 0) {
      // Allocate batch event table structure:
      // event = { log = { records = pre_allocated_array } }
      lua_createtable(L_, 0, 1);                              // event table
      lua_createtable(L_, 0, 1);                              // log table
      lua_createtable(L_, static_cast<int>(batch_size_), 0);  // records array
      records_ref_.reset(
          L_, luaL_ref(L_, LUA_REGISTRYINDEX));  // pops records array
      lua_rawgeti(L_, LUA_REGISTRYINDEX, records_ref_.get());
      lua_setfield(L_, -2, "records");
      lua_setfield(L_, -2, "log");
      event_ref_.reset(L_,
                       luaL_ref(L_, LUA_REGISTRYINDEX));  // pops event table
    }
    // Create new row table
    lua_createtable(L_, 0, 16);
    record_index_ = lua_gettop(L_);
  }
}

void LuaEmitter::SetString(std::string_view key, std::string_view val) {
  if (L_ == nullptr || !in_record_) {
    return;
  }
  lua_pushlstring(L_, key.data(), key.size());
  lua_pushlstring(L_, val.data(), val.size());
  lua_rawset(L_, record_index_);
}

void LuaEmitter::SetInt64(std::string_view key, int64_t val) {
  if (L_ == nullptr || !in_record_) {
    return;
  }
  lua_pushlstring(L_, key.data(), key.size());
  lua_pushinteger(L_, static_cast<lua_Integer>(val));
  lua_rawset(L_, record_index_);
}

void LuaEmitter::SetUint64(std::string_view key, uint64_t val) {
  if (L_ == nullptr || !in_record_) {
    return;
  }
  lua_pushlstring(L_, key.data(), key.size());
  lua_pushinteger(L_, static_cast<lua_Integer>(val));
  lua_rawset(L_, record_index_);
}

void LuaEmitter::SetDouble(std::string_view key, double val) {
  if (L_ == nullptr || !in_record_) {
    return;
  }
  lua_pushlstring(L_, key.data(), key.size());
  lua_pushnumber(L_, static_cast<lua_Number>(val));
  lua_rawset(L_, record_index_);
}

void LuaEmitter::SetBool(std::string_view key, bool val) {
  if (L_ == nullptr || !in_record_) {
    return;
  }
  lua_pushlstring(L_, key.data(), key.size());
  lua_pushboolean(L_, val ? 1 : 0);
  lua_rawset(L_, record_index_);
}

void LuaEmitter::SetTableFromStack(std::string_view key) {
  if (L_ == nullptr) {
    return;
  }
  if (!in_record_) {
    lua_pop(L_, 1);
    return;
  }
  // Table from stack is currently at top of stack (record_index_ + 1)
  lua_pushlstring(L_, key.data(), key.size());
  lua_insert(L_, -2);
  lua_rawset(L_, record_index_);
}

void LuaEmitter::EndRecord() {
  if (L_ == nullptr) {
    ++emitted_count_;
    return;
  }
  if (!in_record_) {
    return;
  }
  in_record_ = false;
  ++emitted_count_;

  if (batch_size_ == 1) {
    // Stack: emit func, event, record
    lua_setfield(L_, event_index_,
                 "log");  // event["log"] = record, pops record
    lua_call(L_, 1, 0);   // calls emit(event), pops emit func and event
    record_index_ = 0;
    event_index_ = 0;
  } else {
    ++current_batch_count_;
    lua_rawgeti(L_, LUA_REGISTRYINDEX,
                records_ref_.get());   // pushes records array
    lua_pushvalue(L_, record_index_);  // pushes row table
    lua_rawseti(L_, -2,
                current_batch_count_);  // records[count] = row, pops row
    lua_pop(L_, 1);                     // pops records array
    lua_remove(L_, record_index_);      // removes original row table
    record_index_ = 0;
    if (current_batch_count_ >= static_cast<int>(batch_size_)) {
      EmitCurrentBatch();
    }
  }
}

void LuaEmitter::EmitCurrentBatch() {
  if (L_ == nullptr || current_batch_count_ == 0) {
    return;
  }
  lua_pushvalue(L_, emit_index_);
  lua_rawgeti(L_, LUA_REGISTRYINDEX, event_ref_.get());
  event_ref_.reset();
  records_ref_.reset();
  lua_call(L_, 1, 0);  // calls emit(event), pops emit func and event
  current_batch_count_ = 0;
}

void LuaEmitter::Flush() {
  if (L_ == nullptr) {
    return;
  }
  if (batch_size_ > 1 && current_batch_count_ > 0) {
    EmitCurrentBatch();
  }
}

int LuaEmitter::EmittedCount() const { return emitted_count_; }

}  // namespace core
}  // namespace vector_otel
