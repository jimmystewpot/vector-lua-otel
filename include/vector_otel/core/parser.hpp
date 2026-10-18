#pragma once

#include <string_view>

#include "simdjson.h"

namespace vector_otel::core {

class OtelParser {
 public:
  static constexpr size_t kMaxPayloadSize = 512 * 1024 * 1024;
  static constexpr size_t kMaxRetainedBufferCapacity = 16 * 1024 * 1024;

  OtelParser() = default;

  static OtelParser& Instance();

  simdjson::simdjson_result<simdjson::ondemand::document> Iterate(
      std::string_view json_str);
  simdjson::simdjson_result<simdjson::ondemand::document> Iterate(
      simdjson::padded_string_view json_str);

  size_t Capacity() const noexcept { return parser_.capacity(); }
  void TrimCapacity() noexcept;

  // Thread-local buffer inspection and memory reclamation
  static size_t ThreadLocalBufferCapacity() noexcept;
  static const char* ThreadLocalBufferData() noexcept;
  static void TrimThreadLocalBuffer() noexcept;

 private:
  simdjson::ondemand::parser parser_{kMaxPayloadSize};
};

}  // namespace vector_otel::core
