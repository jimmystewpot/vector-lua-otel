// Verifies the generated build-time extraction limits (include/vector_otel/core/config.hpp.in).
// VECTOR_OTEL_TEST_EXPECTED_MAX_PAYLOAD_BYTES is set from the CMake variable that configured the
// build, so this checks that the generated header faithfully encodes the configured value.
#include <gtest/gtest.h>

#include <cstddef>

#include "simdjson.h"
#include "vector_otel/core/config.hpp"

namespace vector_otel::core {
namespace {

TEST(ExtractionLimitsTest, CompiledValuesMatchConfiguration) {
  EXPECT_EQ(kMaxIncomingRecords, 2048u);
  EXPECT_EQ(kMaxPayloadBytes, std::size_t{VECTOR_OTEL_TEST_EXPECTED_MAX_PAYLOAD_BYTES});
  EXPECT_EQ(kAbsoluteMaxPayloadBytes, 536870912u);
  EXPECT_GE(kMaxPayloadBytes, 1u);
  EXPECT_LE(kMaxPayloadBytes, kAbsoluteMaxPayloadBytes);
}

TEST(ExtractionLimitsTest, PaddingReserveCoversSimdjsonPaddingWithoutOverflow) {
  EXPECT_GE(kPayloadPaddingBytes, static_cast<std::size_t>(simdjson::SIMDJSON_PADDING));
  EXPECT_LE(kMaxPayloadBytes, static_cast<std::size_t>(-1) - kPayloadPaddingBytes);
}

}  // namespace
}  // namespace vector_otel::core
