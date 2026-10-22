#pragma once

// RowEmitter: the extractors' view of the output. It offers a record API
// (StartRecord / Set* / EndRecord / Flush); a record is staged into a StagedRow and
// delivered by the protected emission leaves. It never touches Lua itself.
//
// Builder calls may throw (std::bad_alloc, std::length_error); they run in the extractor's outer
// frame and are caught by the process runner. EndRecord/Flush never throw: a failed emission sets
// the context's error and HasError().

#include <string_view>

#include "vector_otel/core/operation_context.hpp"
#include "vector_otel/core/staged_row.hpp"

namespace vector_otel::core {

class RowEmitter {
 public:
  RowEmitter(OperationContext& ctx, StagedRow& row) noexcept : ctx_(ctx), row_(row) {}
  RowEmitter(const RowEmitter&) = delete;
  RowEmitter& operator=(const RowEmitter&) = delete;

  void StartRecord() { row_.Reset(); }
  void SetString(Key key, std::string_view value) { row_.Str(key, value); }
  void SetInt64(Key key, std::int64_t value) { row_.Int(key, value); }
  void SetUint64(Key key, std::uint64_t value) { row_.Uint(key, value); }
  void SetDouble(Key key, double value) { row_.Dbl(key, value); }
  void SetBool(Key key, bool value) { row_.Bool(key, value); }

  // Finishes the record. Without an emit callback (dry run: ctx.dry_run) the row is discarded.
  void EndRecord() noexcept {
    if (ctx_.dry_run) {
      ++ctx_.delivered_count;  // dry runs report the rows they would have delivered
    } else if (!failed_ && !EmitStagedRow(ctx_, row_)) {
      failed_ = true;
    }
    ++staged_count_;
  }
  void Flush() noexcept {
    if (!failed_ && !ctx_.dry_run && !FlushStagedRows(ctx_)) {
      failed_ = true;
    }
  }

  bool HasError() const noexcept { return failed_; }
  std::string_view ErrorMessage() const noexcept { return ctx_.error.view(); }
  int EmittedCount() const noexcept { return ctx_.delivered_count; }
  // Records finished so far, whether or not they were delivered (dry-run counting).
  int StagedCount() const noexcept { return staged_count_; }

  StagedRow& row() noexcept { return row_; }
  OperationContext& context() noexcept { return ctx_; }

 private:
  OperationContext& ctx_;
  StagedRow& row_;
  bool failed_ = false;
  int staged_count_ = 0;
};

// Materialises a shared (per-resource / per-scope / per-metric) attributes tape into its anchor
// slot the first time a record needs it; later records reference the same table. Dry runs never
// touch Lua.
inline bool EnsureSharedAnchor(OperationContext& ctx, const StagedRow& tape, AnchorSlot slot,
                               bool& ready) {
  if (ready || ctx.dry_run) {
    return true;
  }
  if (!MaterializeToAnchor(ctx, tape, slot)) {
    return false;
  }
  ready = true;
  return true;
}

}  // namespace vector_otel::core
