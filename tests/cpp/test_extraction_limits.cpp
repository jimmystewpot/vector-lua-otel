// Verifies the generated build-time extraction limits (include/vector_otel/core/config.hpp.in).
// VECTOR_OTEL_TEST_EXPECTED_MAX_PAYLOAD_BYTES and VECTOR_OTEL_TEST_EXPECTED_MAX_BATCH_RECORDS
// are set from the CMake variables that configured the build, so this checks that the generated
// header faithfully encodes the configured values.
#include <gtest/gtest.h>

#include <cstddef>

#include "simdjson.h"
#include "vector_otel/core/config.hpp"

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

}  // namespace
}  // namespace vector_otel::core
