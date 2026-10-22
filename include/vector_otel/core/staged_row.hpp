#pragma once

// Staged rows and protected emission.
//
// Extractors never touch Lua while they parse. They stage one output row into a StagedRow: a flat
// "tape" of typed nodes in an outer-owned, reusable buffer. Every string (keys that are not
// literals, string values) is COPIED into the row's arena, so nothing in the tape points at
// scratch buffers, parser memory or the Lua stack. Once a row is complete, a protected leaf
// materialises the tape into Lua tables and (for the default batch size of 1) calls the emit
// callback, all under lua_pcall. A Lua error can therefore only unwind the leaf, which owns
// nothing.
//
// The builder methods may throw (std::bad_alloc, std::length_error); they run in the extractors'
// outer C++ frame, never inside a Lua leaf. The emission functions never throw and never raise.
//
// Row buffers are reused between rows (Reset() keeps capacity), so the steady state performs no
// C++ allocation per record.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vector_otel/core/module_state.hpp"
#include "vector_otel/core/operation_context.hpp"

namespace vector_otel::core {

// A map key. Literal keys are stored by pointer (consteval-checked: only string literals / static
// arrays convert implicitly); dynamic keys must be spelled Key::Dynamic() and are copied into the
// row's arena. A default-constructed Key is "unkeyed" and is used for array elements.
class Key {
 public:
  constexpr Key() noexcept = default;
  template <std::size_t N>
  consteval Key(const char (&literal)[N])  // NOLINT(google-explicit-constructor)
      : data_(literal), size_(N - 1), kind_(Kind::kStatic) {
    for (std::size_t i = 0; i + 1 < N; ++i) {
      if (literal[i] == '\0') {
        throw "static keys must not contain NUL";  // compile-time error: this ctor is consteval
      }
    }
  }

  static Key Dynamic(std::string_view text) noexcept {
    Key k;
    k.data_ = text.data();
    k.size_ = text.size();
    k.kind_ = Kind::kDynamic;
    return k;
  }

  // Lets helpers templated over the legacy emitter (std::string_view keys) accept a Key.
  constexpr operator std::string_view() const noexcept {  // NOLINT(google-explicit-constructor)
    return std::string_view(data_, size_);
  }

  bool keyed() const noexcept { return kind_ != Kind::kNone; }
  bool is_static() const noexcept { return kind_ == Kind::kStatic; }
  const char* data() const noexcept { return data_; }
  std::size_t size() const noexcept { return size_; }

 private:
  enum class Kind : std::uint8_t { kNone, kStatic, kDynamic };
  const char* data_ = nullptr;
  std::size_t size_ = 0;
  Kind kind_ = Kind::kNone;
};

class StagedRow {
 public:
  static constexpr int kMaxDepth = 8;
  // Buffers larger than this are released by Reset() so that one huge record cannot pin memory.
  static constexpr std::size_t kRetainedArenaBytes = 16u * 1024u * 1024u;
  static constexpr std::size_t kRetainedNodeBytes = 16u * 1024u * 1024u;

  enum class NodeKind : std::uint8_t { kString, kInt, kDouble, kBool, kAnchor, kMap, kArray };

  struct Node {
    NodeKind kind = NodeKind::kInt;
    bool keyed = false;
    bool static_key = false;
    bool flag = false;  // kBool
    std::uint32_t key_len = 0;
    std::uint32_t str_off = 0;  // arena offset of a string value / a dynamic key
    std::uint32_t str_len = 0;
    std::uint32_t children = 0;  // kMap / kArray: direct children, set by End()
    const char* static_key_data = nullptr;
    std::uint32_t key_off = 0;
    union {
      std::int64_t i;
      double d;
    } num = {0};
  };

  StagedRow() = default;
  StagedRow(const StagedRow&) = delete;
  StagedRow& operator=(const StagedRow&) = delete;

  // Starts a new row: clears the tape, keeps capacity.
  void Reset() noexcept;

  void Str(Key key, std::string_view value);
  void Int(Key key, std::int64_t value);
  // Unsigned values above INT64_MAX are clamped to INT64_MAX (Lua integers are signed).
  void Uint(Key key, std::uint64_t value);
  void Dbl(Key key, double value);
  void Bool(Key key, bool value);
  // References the table currently stored in an anchor slot (shared resource/scope attributes).
  void Anchor(Key key, AnchorSlot slot);

  void BeginMap(Key key);
  void BeginArray(Key key);
  void End();

  bool Complete() const noexcept { return depth_ == 0; }
  int depth() const noexcept { return depth_; }

  // Introspection (tests, diagnostics).
  std::size_t node_count() const noexcept { return nodes_.size(); }
  std::size_t root_children() const noexcept { return root_children_; }
  std::size_t arena_size() const noexcept { return arena_.size(); }
  std::size_t node_capacity() const noexcept { return nodes_.capacity(); }
  std::size_t arena_capacity() const noexcept { return arena_.capacity(); }

  const Node* nodes() const noexcept { return nodes_.data(); }
  const char* arena() const noexcept { return arena_.data(); }

 private:
  Node& Append(Key key, NodeKind kind);
  void Begin(Key key, NodeKind kind);
  std::uint32_t CopyToArena(const char* data, std::size_t size);

  std::vector<Node> nodes_;
  std::string arena_;
  std::uint32_t root_children_ = 0;
  std::uint32_t open_node_[kMaxDepth] = {};
  std::uint32_t open_children_[kMaxDepth] = {};
  int depth_ = 0;
};

// ---- protected emission (never throw, never raise) -------------------------------------------
//
// All three return false after recording the failure in ctx.error (and rooting the error object).
// They require a dispatcher-frame context (ctx.anchor_index valid, kEmit anchored).

// Materialises `row` as {log = <row table>} and delivers it. With ctx.batch_size == 1 the emit
// callback is invoked inside the same protected leaf and ctx.delivered_count is advanced as soon as
// it returns successfully. With a larger batch size the row is appended to the anchored batch and
// the callback is invoked when the batch is full (delivered_count then advances by the batch
// length). A failed callback discards the pending rows, which are never counted.
bool EmitStagedRow(OperationContext& ctx, const StagedRow& row) noexcept;

// Delivers a partially filled batch (no-op when nothing is pending or batch_size == 1).
bool FlushStagedRows(OperationContext& ctx) noexcept;

// Materialises `tape` (its root children become the table's fields) into an anchor slot, so that
// later rows can share one table through StagedRow::Anchor().
bool MaterializeToAnchor(OperationContext& ctx, const StagedRow& tape, AnchorSlot slot) noexcept;

}  // namespace vector_otel::core
