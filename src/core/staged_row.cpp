#include "vector_otel/core/staged_row.hpp"

#include <limits>
#include <stdexcept>

#include "vector_otel/core/protected_lua.hpp"

namespace vector_otel::core {

// ---- builder ----------------------------------------------------------------------------------

void StagedRow::Reset() noexcept {
  // A single huge record must not pin its buffers forever. Releasing never allocates.
  if (arena_.capacity() > kRetainedArenaBytes) {
    std::string().swap(arena_);
  }
  if (nodes_.capacity() * sizeof(Node) > kRetainedNodeBytes) {
    std::vector<Node>().swap(nodes_);
  }
  nodes_.clear();
  arena_.clear();
  root_children_ = 0;
  depth_ = 0;
}

std::uint32_t StagedRow::CopyToArena(const char* data, std::size_t size) {
  if (size > std::numeric_limits<std::uint32_t>::max() - arena_.size()) {
    throw std::length_error("staged row arena exceeds 4 GiB");
  }
  const auto offset = static_cast<std::uint32_t>(arena_.size());
  arena_.append(data, size);
  return offset;
}

StagedRow::Node& StagedRow::Append(Key key, NodeKind kind) {
  if (nodes_.size() >= std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error("staged row has too many nodes");
  }
  Node node;
  node.kind = kind;
  if (key.keyed()) {
    node.keyed = true;
    node.key_len = static_cast<std::uint32_t>(key.size());
    if (key.is_static()) {
      node.static_key = true;
      node.static_key_data = key.data();
    } else {
      node.key_off = CopyToArena(key.data(), key.size());
    }
  }
  nodes_.push_back(node);
  if (depth_ == 0) {
    ++root_children_;
  } else {
    ++open_children_[depth_ - 1];
  }
  return nodes_.back();
}

void StagedRow::Str(Key key, std::string_view value) {
  Node& node = Append(key, NodeKind::kString);
  node.str_off = CopyToArena(value.data(), value.size());
  node.str_len = static_cast<std::uint32_t>(value.size());
}

void StagedRow::Int(Key key, std::int64_t value) { Append(key, NodeKind::kInt).num.i = value; }

void StagedRow::Uint(Key key, std::uint64_t value) {
  constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  Append(key, NodeKind::kInt).num.i =
      value > kMax ? std::numeric_limits<std::int64_t>::max() : static_cast<std::int64_t>(value);
}

void StagedRow::Dbl(Key key, double value) { Append(key, NodeKind::kDouble).num.d = value; }

void StagedRow::Bool(Key key, bool value) { Append(key, NodeKind::kBool).flag = value; }

void StagedRow::Anchor(Key key, AnchorSlot slot) {
  Append(key, NodeKind::kAnchor).num.i = static_cast<std::int64_t>(slot);
}

void StagedRow::BeginMap(Key key) { Begin(key, NodeKind::kMap); }

void StagedRow::BeginArray(Key key) { Begin(key, NodeKind::kArray); }

void StagedRow::Begin(Key key, NodeKind kind) {
  if (depth_ >= kMaxDepth) {
    throw std::length_error("staged row nesting too deep");
  }
  Append(key, kind);
  open_node_[depth_] = static_cast<std::uint32_t>(nodes_.size() - 1);
  open_children_[depth_] = 0;
  ++depth_;
}

void StagedRow::End() {
  if (depth_ == 0) {
    throw std::logic_error("staged row End() without an open container");
  }
  --depth_;
  nodes_[open_node_[depth_]].children = open_children_[depth_];
}

// ---- materialisation (runs only inside protected leaves) ----------------------------------------
//
// Everything below is called with only trivial automatic objects on the C++ stack; a Lua error may
// unwind it freely. Recursion depth is bounded by StagedRow::kMaxDepth.

namespace {

constexpr int kLeafStackSlots = 64;

using Node = StagedRow::Node;

void PushKey(lua_State* L, const Node& n, const char* arena) {
  lua_pushlstring(L, n.static_key ? n.static_key_data : arena + n.key_off, n.key_len);
}

const Node* PushValue(lua_State* L, const Node* n, const char* arena);

// Fills the table at stack index `t` from `count` consecutive sibling subtrees starting at `n`.
const Node* FillTable(lua_State* L, int t, const Node* n, std::uint32_t count, bool array,
                      const char* arena) {
  for (std::uint32_t i = 0; i < count; ++i) {
    if (array) {
      n = PushValue(L, n, arena);
      lua_rawseti(L, t, static_cast<lua_Integer>(i) + 1);
    } else {
      PushKey(L, *n, arena);
      n = PushValue(L, n, arena);
      lua_rawset(L, t);
    }
  }
  return n;
}

// Pushes the value of node `n` and returns the node following its subtree.
const Node* PushValue(lua_State* L, const Node* n, const char* arena) {
  switch (n->kind) {
    case StagedRow::NodeKind::kString:
      lua_pushlstring(L, arena + n->str_off, n->str_len);
      return n + 1;
    case StagedRow::NodeKind::kInt:
      lua_pushinteger(L, static_cast<lua_Integer>(n->num.i));
      return n + 1;
    case StagedRow::NodeKind::kDouble:
      lua_pushnumber(L, static_cast<lua_Number>(n->num.d));
      return n + 1;
    case StagedRow::NodeKind::kBool:
      lua_pushboolean(L, n->flag ? 1 : 0);
      return n + 1;
    case StagedRow::NodeKind::kAnchor:
      PushAnchor(L, kLeafAnchorIndex, static_cast<AnchorSlot>(n->num.i));
      return n + 1;
    case StagedRow::NodeKind::kMap:
    case StagedRow::NodeKind::kArray: {
      const bool array = n->kind == StagedRow::NodeKind::kArray;
      const std::uint32_t children = n->children;
      lua_createtable(L, array ? static_cast<int>(children) : 0,
                      array ? 0 : static_cast<int>(children));
      return FillTable(L, lua_gettop(L), n + 1, children, array, arena);
    }
  }
  return n + 1;
}

// Pushes a new table holding the row's root fields.
void PushRowTable(lua_State* L, const StagedRow& row) {
  lua_createtable(L, 0, static_cast<int>(row.root_children()));
  FillTable(L, lua_gettop(L), row.nodes(), static_cast<std::uint32_t>(row.root_children()), false,
            row.arena());
}

struct RowPayload {
  const StagedRow* row = nullptr;
  AnchorSlot slot = AnchorSlot::kRow;
  bool callback_failed = false;
};

// Calls emit(event) for the event on top of the stack (emit is just below it). Returns true when
// the callback returned normally. On failure the error is captured and rooted.
bool CallEmit(OperationContext& ctx, RowPayload& payload) {
  const int status = lua_pcall(ctx.L, 1, 0, 0);
  if (status == LUA_OK) {
    return true;
  }
  CaptureLuaError(ctx, status, kLeafAnchorIndex);
  payload.callback_failed = true;
  return false;
}

// Delivers the anchored batch (event on kEvent) and releases it whether or not the callback
// succeeded: a batch never survives its callback.
void DeliverBatch(OperationContext& ctx, RowPayload& payload) {
  lua_State* L = ctx.L;
  PushAnchor(L, kLeafAnchorIndex, AnchorSlot::kEmit);
  PushAnchor(L, kLeafAnchorIndex, AnchorSlot::kEvent);
  const int rows = ctx.batch_count;
  if (CallEmit(ctx, payload)) {
    ctx.delivered_count += rows;  // immediately: nothing fallible runs between return and count
  }
  ctx.batch_count = 0;
  ClearAnchor(L, kLeafAnchorIndex, AnchorSlot::kEvent);
  ClearAnchor(L, kLeafAnchorIndex, AnchorSlot::kBatch);
}

// Creates {log = {records = <array of batch_size>}} and anchors it (kEvent, kBatch).
void StartBatch(OperationContext& ctx) {
  lua_State* L = ctx.L;
  lua_createtable(L, 0, 1);  // event
  lua_pushvalue(L, -1);
  SetAnchor(L, kLeafAnchorIndex, AnchorSlot::kEvent);
  const int event = lua_gettop(L);
  lua_createtable(L, 0, 1);  // {records = ...}
  const int container = lua_gettop(L);
  lua_pushliteral(L, "records");
  lua_createtable(L, ctx.batch_size, 0);
  lua_pushvalue(L, -1);
  SetAnchor(L, kLeafAnchorIndex, AnchorSlot::kBatch);
  lua_rawset(L, container);
  lua_pushliteral(L, "log");
  lua_pushvalue(L, container);
  lua_rawset(L, event);
  lua_settop(L, event - 1);
}

int EmitRowLeaf(lua_State* L) {
  OperationContext& ctx = LeafContext(L);
  auto& payload = *static_cast<RowPayload*>(ctx.leaf_payload);
  luaL_checkstack(L, kLeafStackSlots, "staged row too deep");

  if (ctx.batch_size <= 1) {
    PushAnchor(L, kLeafAnchorIndex, AnchorSlot::kEmit);
    lua_createtable(L, 0, 1);  // event
    const int event = lua_gettop(L);
    lua_pushliteral(L, "log");
    PushRowTable(L, *payload.row);
    lua_rawset(L, event);
    if (CallEmit(ctx, payload)) {
      ctx.delivered_count += 1;
    }
    return 0;
  }

  if (ctx.batch_count == 0) {
    StartBatch(ctx);
  }
  PushAnchor(L, kLeafAnchorIndex, AnchorSlot::kBatch);
  const int records = lua_gettop(L);
  PushRowTable(L, *payload.row);
  lua_rawseti(L, records, static_cast<lua_Integer>(ctx.batch_count) + 1);
  ctx.batch_count += 1;  // the row is now part of the anchored batch
  lua_settop(L, records - 1);
  if (ctx.batch_count >= ctx.batch_size) {
    DeliverBatch(ctx, payload);
  }
  return 0;
}

int FlushLeaf(lua_State* L) {
  OperationContext& ctx = LeafContext(L);
  auto& payload = *static_cast<RowPayload*>(ctx.leaf_payload);
  luaL_checkstack(L, kLeafStackSlots, "staged row too deep");
  DeliverBatch(ctx, payload);
  return 0;
}

int MaterializeLeaf(lua_State* L) {
  OperationContext& ctx = LeafContext(L);
  auto& payload = *static_cast<RowPayload*>(ctx.leaf_payload);
  luaL_checkstack(L, kLeafStackSlots, "staged row too deep");
  PushRowTable(L, *payload.row);
  SetAnchor(L, kLeafAnchorIndex, payload.slot);
  return 0;
}

}  // namespace

bool EmitStagedRow(OperationContext& ctx, const StagedRow& row) noexcept {
  RowPayload payload;
  payload.row = &row;
  const bool ok = RunLuaLeaf(ctx, EmitRowLeaf, &payload) && !payload.callback_failed;
  if (!ok) {
    ctx.batch_count = 0;  // pending rows are discarded, never counted, never carried over
  }
  return ok;
}

bool FlushStagedRows(OperationContext& ctx) noexcept {
  if (ctx.batch_size <= 1 || ctx.batch_count == 0) {
    return true;
  }
  RowPayload payload;
  const bool ok = RunLuaLeaf(ctx, FlushLeaf, &payload) && !payload.callback_failed;
  if (!ok) {
    ctx.batch_count = 0;
  }
  return ok;
}

bool MaterializeToAnchor(OperationContext& ctx, const StagedRow& tape, AnchorSlot slot) noexcept {
  RowPayload payload;
  payload.row = &tape;
  payload.slot = slot;
  return RunLuaLeaf(ctx, MaterializeLeaf, &payload);
}

}  // namespace vector_otel::core
