#include "vector_otel/core/parser.hpp"

#include <string>

namespace vector_otel {
namespace core {

namespace {

thread_local std::string tls_padded_buffer;

simdjson::simdjson_result<simdjson::ondemand::document>
IterateWithThreadLocalPadding(simdjson::ondemand::parser& parser,
                              std::string_view json_str) {
  const size_t needed_capacity = json_str.size() + simdjson::SIMDJSON_PADDING;
  if (tls_padded_buffer.capacity() < needed_capacity) {
    tls_padded_buffer.reserve(needed_capacity);
  }
  tls_padded_buffer.assign(json_str.data(), json_str.size());
  tls_padded_buffer.append(simdjson::SIMDJSON_PADDING, '\0');
  return parser.iterate(tls_padded_buffer.data(), json_str.size(),
                        tls_padded_buffer.size());
}

}  // namespace

simdjson::simdjson_result<simdjson::ondemand::document> OtelParser::Iterate(
    std::string_view json_str) {
  simdjson::padded_string_view psv(json_str.data(), json_str.size(),
                                   json_str.size());
  if (psv.has_sufficient_padding()) {
    return parser_.iterate(psv);
  }
  return IterateWithThreadLocalPadding(parser_, json_str);
}

simdjson::simdjson_result<simdjson::ondemand::document> OtelParser::Iterate(
    simdjson::padded_string_view json_str) {
  if (json_str.has_sufficient_padding()) {
    return parser_.iterate(json_str);
  }
  return IterateWithThreadLocalPadding(
      parser_, std::string_view(json_str.data(), json_str.size()));
}

size_t OtelParser::ThreadLocalBufferCapacity() noexcept {
  return tls_padded_buffer.capacity();
}

const char* OtelParser::ThreadLocalBufferData() noexcept {
  return tls_padded_buffer.empty() ? nullptr : tls_padded_buffer.data();
}

}  // namespace core
}  // namespace vector_otel
