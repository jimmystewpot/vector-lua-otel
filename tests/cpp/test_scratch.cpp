#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "vector_otel/core/scratch.hpp"

namespace vector_otel::core {
namespace {

TEST(ScratchTest, KeepsCapacityAtOrBelowCap) {
  std::vector<int64_t> v;
  v.reserve(kMaxScratchVectorCapacity);
  v.push_back(42);
  v.push_back(99);

  ResetScratchVector(v);

  EXPECT_EQ(v.size(), 0u);
  EXPECT_GE(v.capacity(), kMaxScratchVectorCapacity);
}

TEST(ScratchTest, ReleasesOversizedCapacity) {
  std::vector<int64_t> v;
  v.reserve(kMaxScratchVectorCapacity + 1);
  v.push_back(1);
  ASSERT_GT(v.capacity(), kMaxScratchVectorCapacity);

  ResetScratchVector(v);

  EXPECT_EQ(v.size(), 0u);
  EXPECT_LE(v.capacity(), kMaxScratchVectorCapacity);

  v.push_back(123);
  EXPECT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0], 123);
}

TEST(ScratchTest, WorksForDouble) {
  std::vector<double> v;
  v.reserve(kMaxScratchVectorCapacity);
  v.push_back(1.5);
  ResetScratchVector(v);
  EXPECT_EQ(v.size(), 0u);
  EXPECT_GE(v.capacity(), kMaxScratchVectorCapacity);

  v.reserve(kMaxScratchVectorCapacity + 1);
  v.push_back(2.5);
  ASSERT_GT(v.capacity(), kMaxScratchVectorCapacity);

  ResetScratchVector(v);
  EXPECT_EQ(v.size(), 0u);
  EXPECT_LE(v.capacity(), kMaxScratchVectorCapacity);

  v.push_back(3.25);
  EXPECT_EQ(v.size(), 1u);
  EXPECT_DOUBLE_EQ(v[0], 3.25);
}

TEST(ScratchTest, ResetScratchStringKeepsSmallCapacity) {
  std::string s;
  s.reserve(1024);
  s.assign("abc");
  ASSERT_GE(s.capacity(), 1024u);

  ResetScratchString(s);

  EXPECT_TRUE(s.empty());
  EXPECT_GE(s.capacity(), 1024u);
}

TEST(ScratchTest, ResetScratchStringReleasesOversizedCapacity) {
  std::string s;
  // Assign more than kMaxScratchStringCapacity characters
  std::string huge(kMaxScratchStringCapacity + 1, 'x');
  s = huge;
  ASSERT_GT(s.capacity(), kMaxScratchStringCapacity);

  ResetScratchString(s);

  EXPECT_TRUE(s.empty());
  EXPECT_LT(s.capacity(), kMaxScratchStringCapacity);
}

TEST(ScratchTest, ReserveScratchVectorPreAllocatesInitialCapacity) {
  std::vector<int64_t> v;
  EXPECT_EQ(v.capacity(), 0u);

  ReserveScratchVector(v);

  EXPECT_TRUE(v.empty());
  EXPECT_GE(v.capacity(), kInitialScratchVectorCapacity);

  // Calling ReserveScratchVector again after pushing elements clears without shrinking
  v.push_back(10);
  v.push_back(20);
  const size_t cap_before = v.capacity();
  ReserveScratchVector(v);
  EXPECT_TRUE(v.empty());
  EXPECT_EQ(v.capacity(), cap_before);

  // After ResetScratchVector trims an oversized vector, ReserveScratchVector restores pre-reserved
  // capacity before the next extraction batch
  v.reserve(kMaxScratchVectorCapacity + 1);
  ResetScratchVector(v);
  EXPECT_EQ(v.capacity(), 0u);
  ReserveScratchVector(v);
  EXPECT_GE(v.capacity(), kInitialScratchVectorCapacity);
}

TEST(ScratchTest, ReserveScratchStringPreAllocatesInitialCapacity) {
  std::string s;
  ReserveScratchString(s);
  EXPECT_TRUE(s.empty());
  EXPECT_GE(s.capacity(), kInitialScratchStringCapacity);

  s.assign("service-a");
  const size_t cap_before = s.capacity();
  ReserveScratchString(s);
  EXPECT_TRUE(s.empty());
  EXPECT_EQ(s.capacity(), cap_before);
}

}  // namespace
}  // namespace vector_otel::core
