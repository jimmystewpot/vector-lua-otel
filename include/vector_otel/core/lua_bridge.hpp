#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#ifndef VECTOR_OTEL_EMIT_BATCH_SIZE
#define VECTOR_OTEL_EMIT_BATCH_SIZE 1
#endif

namespace vector_otel::core {

class ScopedLuaRef {
 public:
  constexpr ScopedLuaRef() noexcept : L_(nullptr), ref_(LUA_NOREF) {}
  ScopedLuaRef(lua_State* L, int ref) noexcept : L_(L), ref_(ref) {}

  ~ScopedLuaRef() { reset(); }

  // Non-copyable
  ScopedLuaRef(const ScopedLuaRef&) = delete;
  ScopedLuaRef& operator=(const ScopedLuaRef&) = delete;

  // Move-constructible and move-assignable
  ScopedLuaRef(ScopedLuaRef&& other) noexcept : L_(other.L_), ref_(other.ref_) {
    other.L_ = nullptr;
    other.ref_ = LUA_NOREF;
  }

  ScopedLuaRef& operator=(ScopedLuaRef&& other) noexcept {
    if (this != &other) {
      reset();
      L_ = other.L_;
      ref_ = other.ref_;
      other.L_ = nullptr;
      other.ref_ = LUA_NOREF;
    }
    return *this;
  }

  int get() const noexcept { return ref_; }
  bool valid() const noexcept { return L_ != nullptr && ref_ != LUA_NOREF && ref_ != LUA_REFNIL; }

  void reset(int new_ref = LUA_NOREF) noexcept {
    if (valid()) {
      luaL_unref(L_, LUA_REGISTRYINDEX, ref_);
    }
    ref_ = new_ref;
  }

  void reset(lua_State* L, int new_ref) noexcept {
    if (valid()) {
      luaL_unref(L_, LUA_REGISTRYINDEX, ref_);
    }
    L_ = L;
    ref_ = new_ref;
  }

 private:
  lua_State* L_{nullptr};
  int ref_{LUA_NOREF};
};

constexpr size_t kCompileTimeBatchSize = VECTOR_OTEL_EMIT_BATCH_SIZE;

class LuaEmitter {
 public:
  explicit LuaEmitter(lua_State* L, int emit_index, size_t batch_size = kCompileTimeBatchSize,
                      std::string_view record_field_name = "log");
  ~LuaEmitter() {
    if (L_ != nullptr && (in_record_ || has_error_)) {
      lua_settop(L_, initial_top_);
    }
  }

  // Non-copyable
  LuaEmitter(const LuaEmitter&) = delete;
  LuaEmitter& operator=(const LuaEmitter&) = delete;

  // Move-constructible and move-assignable
  LuaEmitter(LuaEmitter&& other) noexcept
      : L_(other.L_),
        initial_top_(other.initial_top_),
        emit_index_(other.emit_index_),
        emitted_count_(other.emitted_count_),
        batch_size_(other.batch_size_),
        record_field_name_(other.record_field_name_),
        current_batch_count_(other.current_batch_count_),
        in_record_(other.in_record_),
        has_error_(other.has_error_),
        error_message_(std::move(other.error_message_)),
        record_index_(other.record_index_),
        event_index_(other.event_index_),
        event_ref_(std::move(other.event_ref_)),
        records_ref_(std::move(other.records_ref_)) {
    other.L_ = nullptr;
    other.in_record_ = false;
    other.has_error_ = false;
  }

  LuaEmitter& operator=(LuaEmitter&& other) noexcept {
    if (this != &other) {
      if (L_ != nullptr && (in_record_ || has_error_)) {
        lua_settop(L_, initial_top_);
      }
      L_ = other.L_;
      initial_top_ = other.initial_top_;
      emit_index_ = other.emit_index_;
      emitted_count_ = other.emitted_count_;
      batch_size_ = other.batch_size_;
      record_field_name_ = other.record_field_name_;
      current_batch_count_ = other.current_batch_count_;
      in_record_ = other.in_record_;
      has_error_ = other.has_error_;
      error_message_ = std::move(other.error_message_);
      record_index_ = other.record_index_;
      event_index_ = other.event_index_;
      event_ref_ = std::move(other.event_ref_);
      records_ref_ = std::move(other.records_ref_);
      other.L_ = nullptr;
      other.in_record_ = false;
      other.has_error_ = false;
    }
    return *this;
  }

  void StartRecord();
  void SetString(std::string_view key, std::string_view val);
  void SetInt64(std::string_view key, int64_t val);
  void SetUint64(std::string_view key, uint64_t val);
  void SetDouble(std::string_view key, double val);
  void SetBool(std::string_view key, bool val);
  void SetTableFromStack(std::string_view key);
  void EndRecord();
  void Flush();
  int EmittedCount() const;
  size_t BatchSize() const { return batch_size_; }
  bool HasError() const noexcept { return has_error_; }
  std::string_view ErrorMessage() const noexcept { return error_message_; }

 private:
  void EmitCurrentBatch();

  lua_State* L_{nullptr};
  int initial_top_{0};
  int emit_index_{0};
  int emitted_count_{0};
  size_t batch_size_{kCompileTimeBatchSize};
  std::string_view record_field_name_{"log"};
  int current_batch_count_{0};
  bool in_record_{false};
  bool has_error_{false};
  std::string error_message_;
  int record_index_{0};
  int event_index_{0};
  ScopedLuaRef event_ref_;
  ScopedLuaRef records_ref_;
};

}  // namespace vector_otel::core
