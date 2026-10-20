#include "vector_otel/core/lua_bridge.hpp"

#include <algorithm>
#include <limits>

namespace vector_otel::core {

LuaEmitter::LuaEmitter(lua_State* L, int emit_index, size_t batch_size,
                       std::string_view record_field_name)
    : L_(L),
      initial_top_(L != nullptr ? lua_gettop(L) : 0),
      emit_index_(L != nullptr ? lua_absindex(L, emit_index) : 0),
      batch_size_(std::clamp<size_t>(batch_size, 1, 65536)),
      record_field_name_(record_field_name),
      event_ref_(L, LUA_NOREF),
      records_ref_(L, LUA_NOREF) {}

void LuaEmitter::StartRecord() {
  if (L_ == nullptr || in_record_ || has_error_) [[unlikely]] {
    return;
  }
  if (!lua_checkstack(L_, 10)) [[unlikely]] {
    has_error_ = true;
    error_message_ = "out of lua stack space during StartRecord";
    return;
  }
  in_record_ = true;

  if (batch_size_ == 1) [[likely]] {
    // Flat single event mode (N = 1)
    // Pre-push emit function, event table, and record table
    lua_pushvalue(L_, emit_index_);  // emit function
    lua_createtable(L_, 0, 1);       // event table
    lua_createtable(L_, 0, 32);      // record table
    record_index_ = lua_gettop(L_);
    event_index_ = record_index_ - 1;
  } else {
    // Batch event mode (N > 1)
    if (current_batch_count_ == 0) {
      // Allocate batch event table structure:
      // event = { [record_field_name] = { records = pre_allocated_array } }
      lua_createtable(L_, 0, 1);                                // event table
      lua_createtable(L_, 0, 1);                                // container table
      lua_createtable(L_, static_cast<int>(batch_size_), 0);    // records array
      records_ref_.reset(L_, luaL_ref(L_, LUA_REGISTRYINDEX));  // pops records array
      lua_rawgeti(L_, LUA_REGISTRYINDEX, records_ref_.get());
      lua_setfield(L_, -2, "records");
      lua_pushlstring(L_, record_field_name_.data(), record_field_name_.size());
      lua_insert(L_, -2);
      lua_rawset(L_, -3);  // event[record_field_name] = container
      event_ref_.reset(L_, luaL_ref(L_, LUA_REGISTRYINDEX));  // pops event table
    }
    // Create new row table
    lua_createtable(L_, 0, 32);
    record_index_ = lua_gettop(L_);
  }
}

void LuaEmitter::SetString(std::string_view key, std::string_view val) {
  if (L_ == nullptr || !in_record_ || has_error_) [[unlikely]] {
    return;
  }
  lua_pushlstring(L_, key.data(), key.size());
  lua_pushlstring(L_, val.data(), val.size());
  lua_rawset(L_, record_index_);
}

void LuaEmitter::SetInt64(std::string_view key, int64_t val) {
  if (L_ == nullptr || !in_record_ || has_error_) [[unlikely]] {
    return;
  }
  lua_pushlstring(L_, key.data(), key.size());
  lua_pushinteger(L_, static_cast<lua_Integer>(val));
  lua_rawset(L_, record_index_);
}

void LuaEmitter::SetUint64(std::string_view key, uint64_t val) {
  if (L_ == nullptr || !in_record_ || has_error_) [[unlikely]] {
    return;
  }
  constexpr uint64_t kMaxLuaInt = static_cast<uint64_t>(std::numeric_limits<lua_Integer>::max());
  const lua_Integer clamped =
      val > kMaxLuaInt ? std::numeric_limits<lua_Integer>::max() : static_cast<lua_Integer>(val);
  lua_pushlstring(L_, key.data(), key.size());
  lua_pushinteger(L_, clamped);
  lua_rawset(L_, record_index_);
}

void LuaEmitter::SetDouble(std::string_view key, double val) {
  if (L_ == nullptr || !in_record_ || has_error_) [[unlikely]] {
    return;
  }
  lua_pushlstring(L_, key.data(), key.size());
  lua_pushnumber(L_, static_cast<lua_Number>(val));
  lua_rawset(L_, record_index_);
}

void LuaEmitter::SetBool(std::string_view key, bool val) {
  if (L_ == nullptr || !in_record_ || has_error_) [[unlikely]] {
    return;
  }
  lua_pushlstring(L_, key.data(), key.size());
  lua_pushboolean(L_, val ? 1 : 0);
  lua_rawset(L_, record_index_);
}

void LuaEmitter::SetTableFromStack(std::string_view key) {
  if (L_ == nullptr) [[unlikely]] {
    return;
  }
  if (!in_record_ || has_error_) [[unlikely]] {
    lua_pop(L_, 1);
    return;
  }
  // Table from stack is currently at top of stack (record_index_ + 1)
  lua_pushlstring(L_, key.data(), key.size());
  lua_insert(L_, -2);
  lua_rawset(L_, record_index_);
}

void LuaEmitter::EndRecord() {
  if (L_ == nullptr) [[unlikely]] {
    ++emitted_count_;
    return;
  }
  if (!in_record_ || has_error_) [[unlikely]] {
    return;
  }
  in_record_ = false;

  if (batch_size_ == 1) [[likely]] {
    // Stack: emit func, event, record
    lua_pushlstring(L_, record_field_name_.data(), record_field_name_.size());
    lua_insert(L_, -2);
    lua_rawset(L_, event_index_);  // event[record_field_name] = record

    const int status = lua_pcall(L_, 1, 0, 0);  // protected call emit(event)
    record_index_ = 0;
    event_index_ = 0;
    if (status != LUA_OK) [[unlikely]] {
      has_error_ = true;
      size_t len = 0;
      const char* err = lua_tolstring(L_, -1, &len);
      if (err != nullptr && len > 0) {
        error_message_.assign(err, len);
      } else {
        error_message_ = "unknown lua emit error";
      }
      lua_pop(L_, 1);
      return;
    }
    ++emitted_count_;
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
  if (L_ == nullptr || current_batch_count_ == 0 || has_error_) [[unlikely]] {
    return;
  }
  if (!lua_checkstack(L_, 4)) [[unlikely]] {
    has_error_ = true;
    error_message_ = "out of lua stack space during batch emit";
    return;
  }
  lua_pushvalue(L_, emit_index_);
  lua_rawgeti(L_, LUA_REGISTRYINDEX, event_ref_.get());
  event_ref_.reset();
  records_ref_.reset();
  const int batch_count = current_batch_count_;
  current_batch_count_ = 0;
  const int status = lua_pcall(L_, 1, 0, 0);  // protected call emit(event)
  if (status == LUA_OK) {
    emitted_count_ += batch_count;
  } else [[unlikely]] {
    has_error_ = true;
    size_t len = 0;
    const char* err = lua_tolstring(L_, -1, &len);
    if (err != nullptr && len > 0) {
      error_message_.assign(err, len);
    } else {
      error_message_ = "unknown lua emit error";
    }
    lua_pop(L_, 1);
  }
}

void LuaEmitter::Flush() {
  if (L_ == nullptr || has_error_) [[unlikely]] {
    return;
  }
  if (batch_size_ > 1 && current_batch_count_ > 0) {
    EmitCurrentBatch();
  }
}

int LuaEmitter::EmittedCount() const { return emitted_count_; }

}  // namespace vector_otel::core
