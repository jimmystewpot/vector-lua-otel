#include "vector_otel/core/parser.hpp"

#include <string>

namespace vector_otel::core {

namespace {

thread_local std::string tls_padded_buffer;

simdjson::simdjson_result<simdjson::ondemand::document> IterateWithThreadLocalPadding(
    simdjson::ondemand::parser& parser, std::string_view json_str) {
  if (json_str.size() > OtelParser::kMaxPayloadSize ||
      json_str.size() > (std::string().max_size() - simdjson::SIMDJSON_PADDING)) [[unlikely]] {
    return simdjson::CAPACITY;
  }
  const size_t needed_capacity = json_str.size() + simdjson::SIMDJSON_PADDING;
  if (tls_padded_buffer.capacity() < needed_capacity) [[unlikely]] {
    tls_padded_buffer.reserve(needed_capacity);
  }
  tls_padded_buffer.assign(json_str.data(), json_str.size());
  tls_padded_buffer.append(simdjson::SIMDJSON_PADDING, '\0');
  return parser.iterate(tls_padded_buffer.data(), json_str.size(), tls_padded_buffer.size());
}

}  // namespace

OtelParser& OtelParser::Instance() {
  static thread_local OtelParser instance;
  return instance;
}

simdjson::simdjson_result<simdjson::ondemand::document> OtelParser::Iterate(
    std::string_view json_str) {
  return IterateWithThreadLocalPadding(parser_, json_str);
}

simdjson::simdjson_result<simdjson::ondemand::document> OtelParser::Iterate(
    simdjson::padded_string_view json_str) {
  if (json_str.size() > kMaxPayloadSize) [[unlikely]] {
    return simdjson::CAPACITY;
  }
  if (json_str.has_sufficient_padding()) [[likely]] {
    return parser_.iterate(json_str);
  }
  return IterateWithThreadLocalPadding(parser_, std::string_view(json_str.data(), json_str.size()));
}

void OtelParser::TrimCapacity() noexcept {
  if (parser_.capacity() > kMaxRetainedBufferCapacity) [[unlikely]] {
    parser_ = simdjson::ondemand::parser(kMaxPayloadSize);
  }
}

size_t OtelParser::ThreadLocalBufferCapacity() noexcept { return tls_padded_buffer.capacity(); }

const char* OtelParser::ThreadLocalBufferData() noexcept {
  return tls_padded_buffer.empty() ? nullptr : tls_padded_buffer.data();
}

void OtelParser::TrimThreadLocalBuffer() noexcept {
  if (tls_padded_buffer.capacity() > kMaxRetainedBufferCapacity) [[unlikely]] {
    std::string().swap(tls_padded_buffer);
  }
  Instance().TrimCapacity();
}

}  // namespace vector_otel::core
