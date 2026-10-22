#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string_view>

#include "simdjson.h"
#include "vector_otel/core/config.hpp"

namespace vector_otel::core {

// A prepared simdjson on-demand parser plus the padded input storage it reads from.
//
// The workspace is sized once, from the configured payload limit C (kMaxPayloadBytes), by
// Prepare(): parser capacity max(C, 32), depth 1024 and C + kPayloadPaddingBytes of input
// storage. After a successful Prepare() no call in this class allocates, so a request admitted
// against C can never hit an allocation failure while parsing. Preparation is lazy (first use of
// a thread's instance), failure leaves the object unprepared and holding nothing, and the next
// call retries.
//
// The workspace is retained for the lifetime of the object: the Trim* functions below never
// release it. Instances are not thread-safe; Instance() is thread-local.
class OtelParser {
 public:
  static constexpr size_t kMaxPayloadSize = kMaxPayloadBytes;
  // simdjson refuses capacities below its minimal document capacity.
  static constexpr size_t kWorkspaceCapacity = std::max<size_t>(kMaxPayloadBytes, 32);

  OtelParser() = default;

  static OtelParser& Instance();

  // Allocates the workspace if it is not ready. Idempotent; never throws.
  simdjson::error_code Prepare() noexcept;
  bool IsPrepared() const noexcept { return ready_; }

  // Makes `json_str` readable with SIMDJSON_PADDING trailing bytes. A payload larger than C is
  // rejected with CAPACITY before preparation and without touching the payload. Sufficiently
  // padded caller input is borrowed (valid only while the caller keeps it alive and only for
  // the current invocation); anything else is copied into the workspace, which invalidates any
  // earlier staged view. Embedded NUL bytes are preserved.
  simdjson::simdjson_result<simdjson::padded_string_view> StageInput(
      std::string_view json_str) noexcept;
  simdjson::simdjson_result<simdjson::padded_string_view> StageInput(
      simdjson::padded_string_view json_str) noexcept;

  // Parses already staged input. Requires a prepared workspace and size <= C; never allocates.
  simdjson::simdjson_result<simdjson::ondemand::document> IteratePrepared(
      simdjson::padded_string_view json_str) noexcept;

  // Convenience adapters: Prepare() + StageInput() + IteratePrepared().
  simdjson::simdjson_result<simdjson::ondemand::document> Iterate(std::string_view json_str);
  simdjson::simdjson_result<simdjson::ondemand::document> Iterate(
      simdjson::padded_string_view json_str);

  // Workspace inspection (zero / nullptr while unprepared).
  size_t Capacity() const noexcept { return parser_.capacity(); }
  const char* InputData() const noexcept { return input_.get(); }
  size_t InputCapacity() const noexcept { return input_capacity_; }

  // Retained for callers that trim after a request: the prepared workspace is kept.
  void TrimCapacity() noexcept {}
  static void TrimThreadLocalBuffer() noexcept {}

 private:
  simdjson::ondemand::parser parser_{kWorkspaceCapacity};
  std::unique_ptr<char[]> input_;
  size_t input_capacity_ = 0;
  bool ready_ = false;
};

}  // namespace vector_otel::core
