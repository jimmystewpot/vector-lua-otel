#pragma once

// Admission: byte limit and whole-event record count, decided BEFORE any extraction or emission.
//
// The counting pass is admission-only. It can reject an event; it can never turn an input the
// extraction pass would reject into a success, and it does not validate rows. Structures it does
// not understand count as zero candidates and are left for extraction to diagnose with its
// existing wording. Native inputs are counted as an upper bound of candidate objects/tables (an
// object without any recognised field would not emit, but still counts).
//
// Counting rules (sum over ALL resources/scopes, stop at kMaxIncomingRecords + 1):
//   * OTLP logs / traces: object elements of logRecords / spans.
//   * OTLP metrics: object elements of dataPoints of the one metric type the extractor selects.
//   * native: one candidate per object/table element of an accepted array/wrapper, otherwise one
//     per single event; buckets, quantiles and nested exemplars/events/links are fields, never
//     records.

#include <string_view>

#include "simdjson.h"
#include "vector_otel/core/operation_context.hpp"
#include "vector_otel/core/parser.hpp"

namespace vector_otel::core {

// Checks the byte limit (before any copy or preparation), stages `raw` once into the parser's
// retained workspace, and counts candidate records with one on-demand pass whose document and
// handles are gone when this returns. On success `admitted` is the staged padded view the
// extraction pass must reuse via parser.IteratePrepared(admitted). On failure ctx.error holds the
// diagnostic (PayloadLimit / RecordLimit / Memory), delivered_count stays 0 and false is
// returned. Parse errors are NOT admission failures: extraction reports them with its own wording.
bool AdmitJson(OperationContext& ctx, OtelParser& parser, std::string_view raw,
               simdjson::padded_string_view& admitted) noexcept;

// Same, for caller-owned input that may already carry simdjson padding (borrowed, not copied).
bool AdmitJson(OperationContext& ctx, OtelParser& parser, simdjson::padded_string_view raw,
               simdjson::padded_string_view& admitted) noexcept;

// Counts candidate records of the table at `input_index` in one protected leaf using raw Lua
// access only (no metamethods, no serialisation). Anchors the table in AnchorSlot::kInput.
// Failure of the traversal itself (Lua out of memory) is reported in ctx.error as Memory.
bool AdmitTable(OperationContext& ctx, int input_index) noexcept;

}  // namespace vector_otel::core
