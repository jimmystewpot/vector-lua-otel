// Verifies the generated build-time extraction limits (include/vector_otel/core/config.hpp.in).
// VECTOR_OTEL_TEST_EXPECTED_MAX_PAYLOAD_BYTES and VECTOR_OTEL_TEST_EXPECTED_MAX_BATCH_RECORDS
// are set from the CMake variables that configured the build, so this checks that the generated
// header faithfully encodes the configured values.
#include <gtest/gtest.h>

#include <cstddef>

#include "simdjson.h"
#include "vector_otel/core/config.hpp"
#include "vector_otel/core/module_state.hpp"

namespace vector_otel::core {
namespace {

TEST(ExtractionLimitsTest, CompiledValuesMatchConfiguration) {
  EXPECT_EQ(kMaxIncomingRecords, std::size_t{VECTOR_OTEL_TEST_EXPECTED_MAX_BATCH_RECORDS});
  EXPECT_EQ(kMaxPayloadBytes, std::size_t{VECTOR_OTEL_TEST_EXPECTED_MAX_PAYLOAD_BYTES});
  EXPECT_EQ(kAbsoluteMaxPayloadBytes, 536870912u);
  EXPECT_EQ(kAbsoluteMaxBatchRecords, 1048576u);
  EXPECT_GE(kMaxPayloadBytes, 1u);
  EXPECT_LE(kMaxPayloadBytes, kAbsoluteMaxPayloadBytes);
  EXPECT_GE(kMaxIncomingRecords, 1u);
  EXPECT_LE(kMaxIncomingRecords, kAbsoluteMaxBatchRecords);
}

TEST(ExtractionLimitsTest, PaddingReserveCoversSimdjsonPaddingWithoutOverflow) {
  EXPECT_GE(kPayloadPaddingBytes, static_cast<std::size_t>(simdjson::SIMDJSON_PADDING));
  EXPECT_LE(kMaxPayloadBytes, static_cast<std::size_t>(-1) - kPayloadPaddingBytes);
}

TEST(ExtractionLimitsTest, FormatRecordLimitTextBehaviors) {
  char buf[96];
  const std::size_t len = FormatRecordLimitText(buf, sizeof(buf));
  EXPECT_GT(len, 0u);
  EXPECT_LT(len, sizeof(buf));
  EXPECT_NE(std::string_view(buf, len).find("record limit"), std::string_view::npos);

  // Truncation when capacity is smaller than formatted text
  char small_buf[10];
  const std::size_t truncated_len = FormatRecordLimitText(small_buf, sizeof(small_buf));
  EXPECT_EQ(truncated_len, sizeof(small_buf) - 1);
  EXPECT_EQ(small_buf[sizeof(small_buf) - 1], '\0');

  // Zero capacity edge case
  EXPECT_EQ(FormatRecordLimitText(buf, 0), 0u);
}

TEST(ExtractionLimitsTest, FormatPayloadLimitTextBehaviors) {
  char buf[96];
  const std::size_t len = FormatPayloadLimitText(buf, sizeof(buf));
  EXPECT_GT(len, 0u);
  EXPECT_LT(len, sizeof(buf));
  EXPECT_NE(std::string_view(buf, len).find("byte uncompressed JSON limit"),
            std::string_view::npos);

  // Truncation when capacity is smaller than formatted text
  char small_buf[10];
  const std::size_t truncated_len = FormatPayloadLimitText(small_buf, sizeof(small_buf));
  EXPECT_EQ(truncated_len, sizeof(small_buf) - 1);
  EXPECT_EQ(small_buf[sizeof(small_buf) - 1], '\0');

  // Zero capacity edge case
  EXPECT_EQ(FormatPayloadLimitText(buf, 0), 0u);
}

}  // namespace
}  // namespace vector_otel::core
