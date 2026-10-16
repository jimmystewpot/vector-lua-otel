#pragma once

#include <cstdint>
#include <string>

namespace vector_otel {
namespace core {

std::string FormatIso8601Nano(uint64_t unix_nanos);
void FormatIso8601Nano(uint64_t unix_nanos, char* out_buf_32bytes);
uint64_t CurrentTimeUnixNano();

}  // namespace core
}  // namespace vector_otel
