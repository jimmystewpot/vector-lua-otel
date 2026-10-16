#include "vector_otel/core/timestamp.hpp"

#include <chrono>

namespace vector_otel {
namespace core {

void FormatIso8601Nano(uint64_t unix_nanos, char* out_buf_32bytes) {
  if (out_buf_32bytes == nullptr) {
    return;
  }

  const uint64_t secs = unix_nanos / 1'000'000'000ULL;
  const uint32_t nanos = static_cast<uint32_t>(unix_nanos % 1'000'000'000ULL);

  using namespace std::chrono;
  const sys_seconds tp{seconds{secs}};
  const auto dp = floor<days>(tp);
  const year_month_day ymd{dp};
  const hh_mm_ss hms{tp - dp};

  const int y = static_cast<int>(ymd.year());
  const unsigned m = static_cast<unsigned>(ymd.month());
  const unsigned d = static_cast<unsigned>(ymd.day());
  const long long hh = hms.hours().count();
  const long long mm = hms.minutes().count();
  const long long ss = hms.seconds().count();

  out_buf_32bytes[0] = static_cast<char>('0' + (y / 1000));
  out_buf_32bytes[1] = static_cast<char>('0' + ((y / 100) % 10));
  out_buf_32bytes[2] = static_cast<char>('0' + ((y / 10) % 10));
  out_buf_32bytes[3] = static_cast<char>('0' + (y % 10));
  out_buf_32bytes[4] = '-';
  out_buf_32bytes[5] = static_cast<char>('0' + (m / 10));
  out_buf_32bytes[6] = static_cast<char>('0' + (m % 10));
  out_buf_32bytes[7] = '-';
  out_buf_32bytes[8] = static_cast<char>('0' + (d / 10));
  out_buf_32bytes[9] = static_cast<char>('0' + (d % 10));
  out_buf_32bytes[10] = 'T';
  out_buf_32bytes[11] = static_cast<char>('0' + (hh / 10));
  out_buf_32bytes[12] = static_cast<char>('0' + (hh % 10));
  out_buf_32bytes[13] = ':';
  out_buf_32bytes[14] = static_cast<char>('0' + (mm / 10));
  out_buf_32bytes[15] = static_cast<char>('0' + (mm % 10));
  out_buf_32bytes[16] = ':';
  out_buf_32bytes[17] = static_cast<char>('0' + (ss / 10));
  out_buf_32bytes[18] = static_cast<char>('0' + (ss % 10));
  out_buf_32bytes[19] = '.';

  uint32_t rem = nanos;
  for (int i = 28; i >= 20; --i) {
    out_buf_32bytes[i] = static_cast<char>('0' + (rem % 10));
    rem /= 10;
  }
  out_buf_32bytes[29] = 'Z';
  out_buf_32bytes[30] = '\0';
}

std::string FormatIso8601Nano(uint64_t unix_nanos) {
  char buf[32];
  FormatIso8601Nano(unix_nanos, buf);
  return std::string(buf, 30);
}

uint64_t CurrentTimeUnixNano() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

}  // namespace core
}  // namespace vector_otel
