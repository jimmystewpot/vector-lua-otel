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

namespace vector_otel {
namespace core {

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
  ~LuaEmitter() = default;

  // Non-copyable
  LuaEmitter(const LuaEmitter&) = delete;
  LuaEmitter& operator=(const LuaEmitter&) = delete;
  LuaEmitter(LuaEmitter&&) = default;
  LuaEmitter& operator=(LuaEmitter&&) = default;

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

}  // namespace core
}  // namespace vector_otel
