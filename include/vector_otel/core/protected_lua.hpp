#pragma once

// Protected Lua leaves.
//
// Lua reports memory and runtime errors with longjmp, which skips C++ destructors. The rule that
// makes this safe: a Lua error may only unwind frames that own nothing. Everything that owns C++
// resources (the OperationContext, parsers, staging buffers) stays in the outer dispatcher frame,
// and every operation that can allocate or raise inside Lua runs as a LEAF under lua_pcall.
//
// Leaf convention (a leaf is a plain lua_CFunction):
//   * argument 1 is the lightuserdata OperationContext* (use LeafContext(L)),
//     argument 2 is the module anchor table (kLeafAnchorIndex);
//   * the context's leaf_payload carries the caller's POD payload (LeafPayload(L));
//   * the leaf's whole call graph has only trivial automatic objects and non-throwing callees;
//   * a newly created object is stored in an anchor slot (SetAnchor) before the next fallible Lua
//     operation, and results travel through the context/payload, so the leaf returns 0.

#include "vector_otel/core/module_state.hpp"
#include "vector_otel/core/operation_context.hpp"

namespace vector_otel::core {

inline constexpr int kLeafAnchorIndex = 2;

inline OperationContext& LeafContext(lua_State* L) noexcept {
  return *static_cast<OperationContext*>(lua_touserdata(L, 1));
}
inline void* LeafPayload(lua_State* L) noexcept { return LeafContext(L).leaf_payload; }

// Runs `leaf` under lua_pcall. On failure fills ctx.error (memory errors as Memory, string errors
// copied into the fixed diagnostic, any other error object as Lua "Lua operation failed"), roots
// the error object in the kPendingError slot and returns false. Denied stack growth returns false
// with ErrorCode::Stack without running the leaf. Never raises, never throws.
bool RunLuaLeaf(OperationContext& ctx, lua_CFunction leaf, void* payload) noexcept;

// Consumes the failed call's error object on top of the stack (status from lua_pcall): records the
// diagnostic in ctx.error (memory errors as Memory, string errors copied, anything else generic
// Lua) and roots the object in kPendingError of the anchor at `anchor_index`. Leaves that run their
// own lua_pcall (the emit callback) pass kLeafAnchorIndex. Never raises: raw stores only.
void CaptureLuaError(OperationContext& ctx, int status, int anchor_index) noexcept;

// Drops every invocation root EXCEPT the pending error, restores the stack to ctx.base_top.
// Raw stores only: no allocation, no metamethods.
void ClearInvocationRoots(OperationContext& ctx) noexcept;

// Pushes the documented result tuple and returns its size: (count, nil) on success, otherwise
// (nil, message, count). The message is built in a protected leaf; if that fails (e.g. Lua is out
// of memory) the pre-interned fallback for ctx.error.code is returned instead, with the same
// count. The pending error root is released only after the values are on the stack.
// Precondition: the dispatcher frame has at least ctx.base_top + 3 stack slots (every C function
// starts with LUA_MINSTACK = 20 and the dispatcher holds only its two arguments at base_top).
int ReturnOperationResult(OperationContext& ctx) noexcept;

}  // namespace vector_otel::core
