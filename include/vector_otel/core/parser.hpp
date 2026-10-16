#pragma once

#include <string_view>

#include "simdjson.h"

namespace vector_otel {
namespace core {

class OtelParser {
 public:
  static constexpr size_t kMaxPayloadSize = 512 * 1024 * 1024;

  OtelParser() = default;

  simdjson::simdjson_result<simdjson::ondemand::document> Iterate(std::string_view json_str);
  simdjson::simdjson_result<simdjson::ondemand::document> Iterate(
      simdjson::padded_string_view json_str);

  // Thread-local buffer inspection and memory reclamation
  static size_t ThreadLocalBufferCapacity() noexcept;
  static const char* ThreadLocalBufferData() noexcept;
  static void TrimThreadLocalBuffer() noexcept;

 private:
  simdjson::ondemand::parser parser_;
};

}  // namespace core
}  // namespace vector_otel
