#include <gtest/gtest.h>

#include "vector_otel/core/timestamp.hpp"

namespace vector_otel {
namespace core {

TEST(TimestampTest, FormatIso8601NanoGolden) {
  // Test golden timestamp from brief: 1293840000123456789ULL ->
  // "2011-01-01T00:00:00.123456789Z"
  uint64_t ts = 1293840000123456789ULL;
  EXPECT_EQ(FormatIso8601Nano(ts), "2011-01-01T00:00:00.123456789Z");

  char buf[32];
  FormatIso8601Nano(ts, buf);
  EXPECT_STREQ(buf, "2011-01-01T00:00:00.123456789Z");
}

TEST(TimestampTest, FormatIso8601NanoEpochZero) {
  uint64_t ts = 0ULL;
  EXPECT_EQ(FormatIso8601Nano(ts), "1970-01-01T00:00:00.000000000Z");

  char buf[32];
  FormatIso8601Nano(ts, buf);
  EXPECT_STREQ(buf, "1970-01-01T00:00:00.000000000Z");
}

TEST(TimestampTest, FormatIso8601NanoLeapYearAndFractions) {
  // 2024-02-29 12:34:56.000000042Z
  // 2024-02-29 00:00:00 UTC = 1709164800s
  // 12 * 3600 + 34 * 60 + 56 = 45296s
  // Total secs = 1709210096s
  uint64_t ts = 1709210096ULL * 1000000000ULL + 42ULL;
  EXPECT_EQ(FormatIso8601Nano(ts), "2024-02-29T12:34:56.000000042Z");

  // Max nanos fraction: 999999999
  uint64_t ts_max_frac = 1709210096ULL * 1000000000ULL + 999999999ULL;
  EXPECT_EQ(FormatIso8601Nano(ts_max_frac), "2024-02-29T12:34:56.999999999Z");
}

TEST(TimestampTest, FormatIso8601NanoNullBuffer) {
  // Passing nullptr should safely do nothing without crashing
  FormatIso8601Nano(1293840000123456789ULL, nullptr);
  SUCCEED();
}

TEST(TimestampTest, FormatIso8601NanoUint64Max) {
  // UINT64_MAX nanoseconds is year 2554: 2554-07-21T23:34:33.709551615Z
  uint64_t ts_max = UINT64_MAX;
  char buf[32];
  FormatIso8601Nano(ts_max, buf);
  EXPECT_STREQ(buf, "2554-07-21T23:34:33.709551615Z");
  EXPECT_EQ(FormatIso8601Nano(ts_max), "2554-07-21T23:34:33.709551615Z");
}

TEST(TimestampTest, CurrentTimeUnixNanoNonZero) {
  uint64_t now1 = CurrentTimeUnixNano();
  // Ensure timestamp is strictly in the plausible current epoch range (> year
  // 2024 in nanos)
  EXPECT_GT(now1, 1700000000000000000ULL);

  uint64_t now2 = CurrentTimeUnixNano();
  EXPECT_GE(now2, now1);
}

TEST(TimestampTest, CivilToUnixNanoAndRoundTrip) {
  uint64_t ns = 0;
  ASSERT_TRUE(CivilToUnixNano(2026, 10, 16, 12, 0, 0, 123456789, ns));
  EXPECT_EQ(FormatIso8601Nano(ns), "2026-10-16T12:00:00.123456789Z");

  ASSERT_TRUE(CivilToUnixNano(1970, 1, 1, 0, 0, 0, 0, ns));
  EXPECT_EQ(ns, 0ULL);

  EXPECT_FALSE(CivilToUnixNano(1969, 12, 31, 23, 59, 59, 0, ns));
  EXPECT_FALSE(CivilToUnixNano(2026, 13, 1, 0, 0, 0, 0, ns));
  EXPECT_FALSE(CivilToUnixNano(2026, 2, 30, 0, 0, 0, 0, ns));
  EXPECT_FALSE(CivilToUnixNano(2026, 10, 16, 12, 0, 0, 1000000000LL, ns));
}

TEST(TimestampTest, ParseIso8601ToUnixNanoVariants) {
  uint64_t ns = 0;
  ASSERT_TRUE(ParseIso8601ToUnixNano("2026-10-16T12:00:00.123456789Z", ns));
  EXPECT_EQ(FormatIso8601Nano(ns), "2026-10-16T12:00:00.123456789Z");

  ASSERT_TRUE(ParseIso8601ToUnixNano("2026-10-16T12:00:00.5Z", ns));
  EXPECT_EQ(FormatIso8601Nano(ns), "2026-10-16T12:00:00.500000000Z");

  ASSERT_TRUE(ParseIso8601ToUnixNano("2026-10-16T12:00:00Z", ns));
  EXPECT_EQ(FormatIso8601Nano(ns), "2026-10-16T12:00:00.000000000Z");

  ASSERT_TRUE(ParseIso8601ToUnixNano("2026-10-16T12:00:00.25+00:00", ns));
  EXPECT_EQ(FormatIso8601Nano(ns), "2026-10-16T12:00:00.250000000Z");

  ASSERT_TRUE(ParseIso8601ToUnixNano("1293840000123456789", ns));
  EXPECT_EQ(ns, 1293840000123456789ULL);

  EXPECT_FALSE(ParseIso8601ToUnixNano("not-a-timestamp", ns));
  EXPECT_FALSE(ParseIso8601ToUnixNano("", ns));
}

}  // namespace core
}  // namespace vector_otel
