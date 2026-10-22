#include "vector_otel/core/timestamp.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <string_view>

namespace vector_otel::core {

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
  if (y < 1000 || y > 9999) {
    std::memcpy(out_buf_32bytes, "1970-01-01T00:00:00.000000000Z", 30);
    out_buf_32bytes[30] = '\0';
    return;
  }
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
  std::array<char, 32> buf;
  FormatIso8601Nano(unix_nanos, buf.data());
  return std::string(buf.data(), 30);
}

uint64_t CurrentTimeUnixNano() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

bool CivilToUnixNano(int64_t year, int64_t month, int64_t day, int64_t hour, int64_t min,
                     int64_t sec, int64_t nanosec, uint64_t& out_ns) noexcept {
  if (year < 1970 || year > 2262 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 ||
      hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 60 || nanosec < 0 ||
      nanosec > 999'999'999LL) {
    return false;
  }

  using namespace std::chrono;
  const year_month_day ymd{std::chrono::year{static_cast<int>(year)},
                           std::chrono::month{static_cast<unsigned>(month)},
                           std::chrono::day{static_cast<unsigned>(day)}};
  if (!ymd.ok()) {
    return false;
  }

  const auto dp = sys_days{ymd};
  const int64_t days_since_epoch = dp.time_since_epoch().count();
  if (days_since_epoch < 0) {
    return false;
  }

  const uint64_t total_secs = static_cast<uint64_t>(days_since_epoch) * 86400ULL +
                              static_cast<uint64_t>(hour) * 3600ULL +
                              static_cast<uint64_t>(min) * 60ULL + static_cast<uint64_t>(sec);
  out_ns = total_secs * 1'000'000'000ULL + static_cast<uint64_t>(nanosec);
  return true;
}

bool ParseIso8601ToUnixNano(std::string_view sv, uint64_t& out_ns) noexcept {
  while (!sv.empty() &&
         (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() &&
         (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
    sv.remove_suffix(1);
  }
  if (sv.empty()) {
    return false;
  }

  if (sv.size() >= 20 && sv[4] == '-' && sv[7] == '-' &&
      (sv[10] == 'T' || sv[10] == 't' || sv[10] == ' ') && sv[13] == ':' && sv[16] == ':') {
    auto parse_fixed_digits = [](std::string_view s, int64_t& out) noexcept -> bool {
      int64_t v = 0;
      for (char c : s) {
        if (c < '0' || c > '9') {
          return false;
        }
        v = v * 10 + (c - '0');
      }
      out = v;
      return true;
    };

    int64_t year = 0;
    int64_t month = 0;
    int64_t day = 0;
    int64_t hour = 0;
    int64_t min = 0;
    int64_t sec = 0;
    if (!parse_fixed_digits(sv.substr(0, 4), year) || !parse_fixed_digits(sv.substr(5, 2), month) ||
        !parse_fixed_digits(sv.substr(8, 2), day) || !parse_fixed_digits(sv.substr(11, 2), hour) ||
        !parse_fixed_digits(sv.substr(14, 2), min) || !parse_fixed_digits(sv.substr(17, 2), sec)) {
      return false;
    }

    size_t pos = 19;
    int64_t nanosec = 0;
    if (pos < sv.size() && sv[pos] == '.') {
      ++pos;
      int frac_digits = 0;
      while (pos < sv.size() && sv[pos] >= '0' && sv[pos] <= '9') {
        if (frac_digits < 9) {
          nanosec = nanosec * 10 + (sv[pos] - '0');
        }
        ++frac_digits;
        ++pos;
      }
      if (frac_digits < 1 || frac_digits > 9) {
        return false;
      }
      for (int i = frac_digits; i < 9; ++i) {
        nanosec *= 10;
      }
    }

    const std::string_view tz = sv.substr(pos);
    int64_t offset_secs = 0;
    if (tz == "Z" || tz == "z") {
      offset_secs = 0;
    } else if (!tz.empty() && (tz[0] == '+' || tz[0] == '-')) {
      const int64_t sign = (tz[0] == '+') ? 1 : -1;
      int64_t tz_hh = 0;
      int64_t tz_mm = 0;
      if (tz.size() == 6 && tz[3] == ':') {
        if (!parse_fixed_digits(tz.substr(1, 2), tz_hh) ||
            !parse_fixed_digits(tz.substr(4, 2), tz_mm)) {
          return false;
        }
      } else if (tz.size() == 5) {
        if (!parse_fixed_digits(tz.substr(1, 2), tz_hh) ||
            !parse_fixed_digits(tz.substr(3, 2), tz_mm)) {
          return false;
        }
      } else {
        return false;
      }
      if (tz_hh < 0 || tz_hh > 23 || tz_mm < 0 || tz_mm > 59) {
        return false;
      }
      offset_secs = sign * (tz_hh * 3600LL + tz_mm * 60LL);
    } else {
      return false;
    }

    if (offset_secs == 0) {
      return CivilToUnixNano(year, month, day, hour, min, sec, nanosec, out_ns);
    }

    if (year < 1969 || year > 2263 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 ||
        hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 60 || nanosec < 0 ||
        nanosec > 999'999'999LL) {
      return false;
    }

    using namespace std::chrono;
    const year_month_day ymd{std::chrono::year{static_cast<int>(year)},
                             std::chrono::month{static_cast<unsigned>(month)},
                             std::chrono::day{static_cast<unsigned>(day)}};
    if (!ymd.ok()) {
      return false;
    }

    const int64_t days_since_epoch = sys_days{ymd}.time_since_epoch().count();
    const int64_t local_secs = days_since_epoch * 86400LL + hour * 3600LL + min * 60LL + sec;
    const int64_t utc_secs = local_secs - offset_secs;
    constexpr int64_t kMaxUnixSecs =
        sys_days{std::chrono::year{2262} / 12 / 31}.time_since_epoch().count() * 86400LL +
        23 * 3600LL + 59 * 60LL + 60LL;
    if (utc_secs < 0 || utc_secs > kMaxUnixSecs) {
      return false;
    }

    out_ns = static_cast<uint64_t>(utc_secs) * 1'000'000'000ULL + static_cast<uint64_t>(nanosec);
    return true;
  }

  uint64_t uval = 0;
  const char* begin = sv.data();
  const char* end = sv.data() + sv.size();
  auto [ptr, ec] = std::from_chars(begin, end, uval);
  if (ec == std::errc() && ptr == end) {
    out_ns = uval;
    return true;
  }
  return false;
}

}  // namespace vector_otel::core
