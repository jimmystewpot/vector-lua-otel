#pragma once

// Outer-owned state of one process() invocation.
//
// The OperationContext lives in the dispatcher's C++ frame, OUTSIDE every protected Lua leaf, so a
// Lua error (longjmp) can never destroy it, and every failure path reads the same delivered count.
// It is deliberately trivially destructible and holds no owning C++ object (no std::string, no
// vector): diagnostics are copied into a fixed 512-byte array.

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" {
#include <lua.h>
}

namespace vector_otel::core {

class OtelParser;

enum class ModuleKind : std::uint8_t { Otel, Native };
enum class Signal : std::uint8_t { Logs, Traces, Metrics };
enum class RecordKind : std::uint8_t { Log, Metric };

// Argument was added to the planned enumeration so that the existing argument-validation messages
// keep their exact text; it is otherwise treated like Schema.
enum class ErrorCode : std::uint8_t {
  None,
  PayloadLimit,
  RecordLimit,
  Json,
  Schema,
  Lua,
  Memory,
  Cpp,
  Reentrant,
  Stack,
  Argument,
};

// Fixed-size diagnostic. 512 bytes including the trailing NUL: text is truncated to 511 bytes and
// only ever contains diagnostics, never event data.
struct ErrorStatus {
  static constexpr std::size_t kCapacity = 512;

  ErrorCode code = ErrorCode::None;
  std::uint16_t length = 0;
  char message[kCapacity] = {};

  void Set(ErrorCode new_code, std::string_view text) noexcept {
    code = new_code;
    const std::size_t n = text.size() < kCapacity - 1 ? text.size() : kCapacity - 1;
    for (std::size_t i = 0; i < n; ++i) {
      message[i] = text[i];
    }
    message[n] = '\0';
    length = static_cast<std::uint16_t>(n);
  }

  void Clear() noexcept {
    code = ErrorCode::None;
    length = 0;
    message[0] = '\0';
  }

  std::string_view view() const noexcept { return std::string_view(message, length); }
};

struct OperationContext {
  lua_State* L = nullptr;
  ModuleKind module = ModuleKind::Otel;
  Signal signal = Signal::Logs;

  // Rows delivered by emit callbacks that RETURNED successfully (batch length in grouped mode).
  // Updated immediately after the successful call, before any further fallible work.
  int delivered_count = 0;

  // Candidate records counted by admission (stops at kMaxIncomingRecords + 1).
  std::size_t incoming_records = 0;
  ErrorStatus error;

  // Index of the module anchor table valid in the dispatcher frame (normally
  // lua_upvalueindex(1)) and the stack top to restore when the invocation is cleaned up.
  int anchor_index = 0;
  int base_top = 0;

  // Absolute stack indexes of the process() arguments.
  int input_index = 1;
  int emit_index = 2;

  // Grouped emission bookkeeping (batch never spans invocations).
  int batch_size = 1;
  int batch_count = 0;

  // Borrowed, never Lua-owned: selected by the dispatcher, used by the extractors.
  OtelParser* parser = nullptr;
  void* row_scratch = nullptr;
  void* value_scratch = nullptr;

  // Extraction without delivery (counting, read-only verification): rows are staged and dropped.
  bool dry_run = false;

  // Payload handed to the leaf currently running under RunLuaLeaf().
  void* leaf_payload = nullptr;
};

inline void InitOperationContext(OperationContext& ctx, lua_State* L, ModuleKind module,
                                 Signal signal, int anchor_index) noexcept {
  ctx = OperationContext{};
  ctx.L = L;
  ctx.module = module;
  ctx.signal = signal;
  ctx.anchor_index = anchor_index;
  ctx.base_top = L != nullptr ? lua_gettop(L) : 0;  // dry runs have no Lua state
}

}  // namespace vector_otel::core
