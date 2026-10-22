#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace vector_otel::core {

std::string FormatIso8601Nano(uint64_t unix_nanos);
void FormatIso8601Nano(uint64_t unix_nanos, char* out_buf_32bytes);
uint64_t CurrentTimeUnixNano();

bool CivilToUnixNano(int64_t year, int64_t month, int64_t day, int64_t hour, int64_t min,
                     int64_t sec, int64_t nanosec, uint64_t& out_ns) noexcept;
bool ParseIso8601ToUnixNano(std::string_view sv, uint64_t& out_ns) noexcept;

}  // namespace vector_otel::core
