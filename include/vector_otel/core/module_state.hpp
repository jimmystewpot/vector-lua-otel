#pragma once

// Per-lua_State, per-module anchors.
//
// Each Lua state that loads a module gets one private anchor table, created during the module's
// (or any of its submodules') luaopen_ function and published in the registry under a private
// lightuserdata key. Process closures capture it as upvalue 1; nothing is keyed by OS thread and
// no C++ map of lua_State* exists, so the anchor migrates with the state.
//
// The table is a presized array. Slots 1..kImmutableSlotCount hold interned strings that are never
// cleared (fallback messages). The next kInvocationSlotCount slots are the invocation roots: they
// are prepopulated with `false`, and acquiring/releasing a root is a raw store into an existing
// array slot - it cannot allocate and cannot raise. That is what lets cleanup and the truthful
// error return run after Lua memory is exhausted.
//
// Inventory of simultaneous owners (derived from the six extractors, table_reader,
// otlp_common and the dispatcher; see the Task 4 report). Adding a slot later requires extending
// this enum BEFORE the anchor is published, never growing the table at run time.

#include <string_view>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "vector_otel/core/config.hpp"
#include "vector_otel/core/operation_context.hpp"

namespace vector_otel::core {

enum class ImmutableSlot : int {
  kPayloadLimitMessage,  // "payload exceeds <C>-byte uncompressed JSON limit"
  kRecordLimitMessage,   // "event exceeds 2048-record limit"
  kMemoryMessage,
  kLuaMessage,
  kExtractionMessage,
  kPayloadArgMessage,
  kEmitArgMessage,
  kReentrantMessage,
  kStackMessage,
  kReadKeys,  // array of every interned table-reader key (see read_keys.hpp)
  kCount,
};

enum class AnchorSlot : int {
  kInput,  // process() argument 1 (string or table)
  kEmit,   // process() argument 2
  kEvent,  // emit wrapper table {log = ...} for the current row
  kRow,    // current record table
  kResourceAttrs,
  kScopeAttrs,
  kRecordAttrs,    // log/span/data point attributes
  kFilteredAttrs,  // metrics: attributes after service/instance filtering
  kExemplars,
  kBucketCounts,
  kExplicitBounds,
  kPositiveBucketCounts,
  kNegativeBucketCounts,
  kQuantiles,
  kTableResource,  // table input: resource / wrapper node being read
  kTableScope,
  kTableRecord,
  kTableAux1,  // table input: nested node (body, events, links, exemplar element, ...)
  kTableAux2,
  kTableString1,  // coerced/borrowed strings that must outlive their reader
  kTableString2,
  kStagedValue,   // complex value being materialised
  kPendingError,  // error object of a failed leaf; dropped last, after results are pushed
  kCount,
};

inline constexpr int kImmutableSlotCount = static_cast<int>(ImmutableSlot::kCount);
inline constexpr int kInvocationSlotCount = static_cast<int>(AnchorSlot::kCount);

// Exact text of the immutable messages (kPayloadLimitMessage and kRecordLimitMessage are formatted
// at bootstrap).
inline constexpr std::string_view kMemoryText = "memory allocation failed";
inline constexpr std::string_view kLuaText = "Lua operation failed";
inline constexpr std::string_view kExtractionText = "extraction failed";
inline constexpr std::string_view kPayloadArgText = "payload must be a string or table";
inline constexpr std::string_view kEmitArgText = "emit callback must be a function";
inline constexpr std::string_view kReentrantText =
    "re-entrant vector_otel.process() invocation is not permitted";
inline constexpr std::string_view kStackText = "out of lua stack space";

// Formats "payload exceeds <C>-byte uncompressed JSON limit" into `buffer` (NUL terminated, at
// most `capacity` bytes) and returns its length. Shared by bootstrap interning and admission so
// the immutable message and the diagnostic can never drift apart.
std::size_t FormatPayloadLimitText(char* buffer, std::size_t capacity) noexcept;

// Formats "event exceeds <R>-record limit" into `buffer` (NUL terminated, at
// most `capacity` bytes) and returns its length. Shared by bootstrap interning and admission so
// the immutable message and the diagnostic can never drift apart.
std::size_t FormatRecordLimitText(char* buffer, std::size_t capacity) noexcept;

// Finds or creates the anchor for `kind` in `L`. On success the anchor table is left on top of the
// stack. On failure a protected Lua error object is left on top and NOTHING is published; a later
// call retries. The caller must have at least 4 free stack slots (every C function does).
bool EnsureModuleState(lua_State* L, ModuleKind kind) noexcept;

// Registers `funcs` in a new table, each closure capturing the module anchor as upvalue 1, and
// leaves the table on the stack. Raises the bootstrap error (no C++ owners on this frame).
int OpenSignalTable(lua_State* L, ModuleKind kind, const luaL_Reg* funcs);

// Raw, non-allocating, non-raising slot access. `anchor_index` must be an absolute or pseudo index
// (e.g. lua_upvalueindex(1) in the dispatcher, kLeafAnchorIndex inside a leaf).
// SetAnchor pops the value on top of the stack into the slot.
void SetAnchor(lua_State* L, int anchor_index, AnchorSlot slot) noexcept;
void ClearAnchor(lua_State* L, int anchor_index, AnchorSlot slot) noexcept;
void PushAnchor(lua_State* L, int anchor_index, AnchorSlot slot) noexcept;
int AnchorType(lua_State* L, int anchor_index, AnchorSlot slot) noexcept;
void PushImmutable(lua_State* L, int anchor_index, ImmutableSlot slot) noexcept;

}  // namespace vector_otel::core
