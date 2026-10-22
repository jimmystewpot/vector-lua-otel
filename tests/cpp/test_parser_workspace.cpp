// Prepared parser / padded-input workspace contract (fault target only: it counts and denies
// C++ allocations through tests/support/fault_new.cpp).

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#include <sys/mman.h>
#define VECTOR_OTEL_TEST_HAVE_MMAP 1
#endif

#include "fault_new.hpp"
#include "vector_otel/core/config.hpp"
#include "vector_otel/core/parser.hpp"

namespace vector_otel::core {
namespace {

using test::CppFaultMode;
using test::CppFaultScope;

constexpr std::size_t kC = kMaxPayloadBytes;
constexpr std::size_t kPad = kPayloadPaddingBytes;

// A valid JSON document of exactly `n` bytes (n >= 1).
std::string JsonOfSize(std::size_t n) {
  if (n == 1) {
    return "1";
  }
  std::string s(n, ' ');
  s.front() = '[';
  s.back() = ']';
  return s;
}

// Number of array elements, or -1 on any error. Allocation free.
long CountArray(simdjson::simdjson_result<simdjson::ondemand::document>& res) {
  if (res.error() != simdjson::SUCCESS) {
    return -1;
  }
  simdjson::ondemand::array arr;
  if (res.value().get_array().get(arr) != simdjson::SUCCESS) {
    return -1;
  }
  long n = 0;
  for (auto elem : arr) {
    if (elem.error() != simdjson::SUCCESS) {
      return -1;
    }
    ++n;
  }
  return n;
}

bool PaddingIsZero(simdjson::padded_string_view v) {
  for (std::size_t i = 0; i < kPad; ++i) {
    if (v.data()[v.size() + i] != '\0') {
      return false;
    }
  }
  return true;
}

TEST(ParserWorkspaceTest, PreparedCapacitySurvivesMaximumAndSmallInputs) {
  OtelParser parser;
  ASSERT_FALSE(parser.IsPrepared());
  ASSERT_EQ(parser.Prepare(), simdjson::SUCCESS);
  ASSERT_TRUE(parser.IsPrepared());

  const char* input = parser.InputData();
  const std::size_t input_capacity = parser.InputCapacity();
  const std::size_t parser_capacity = parser.Capacity();
  ASSERT_NE(input, nullptr);
  EXPECT_EQ(input_capacity, kC + kPad);
  EXPECT_GE(parser_capacity, std::max<std::size_t>(kC, 32));

  const std::string big = JsonOfSize(kC);
  // Tests run against any configured limit (down to 1 byte): the small input shrinks with it.
  const std::string small = kC >= 7 ? std::string(R"({"a":1})") : std::string("1");
  const std::string* const inputs[] = {&small, &big, &small, &big};

  // No parser or input allocation may happen once the workspace is prepared: every attempt is
  // denied, so any allocation would surface as a failed call below.
  bool all_ok = true;
  std::size_t calls = 0;
  std::size_t attempts = 0;
  {
    CppFaultScope scope(CppFaultMode::kPersistent, 0);
    for (int pass = 0; pass < 2; ++pass) {
      for (const std::string* json : inputs) {
        auto staged = parser.StageInput(std::string_view(*json));
        if (staged.error() != simdjson::SUCCESS) {
          all_ok = false;
          continue;
        }
        const simdjson::padded_string_view view = staged.value_unsafe();
        all_ok = all_ok && view.data() == input && view.size() == json->size() &&
                 view.has_sufficient_padding() && PaddingIsZero(view) &&
                 std::memcmp(view.data(), json->data(), json->size()) == 0;
        auto doc = parser.IteratePrepared(view);
        if (json == &big && kC >= 2) {
          all_ok = all_ok && CountArray(doc) == 0;
        } else {
          int64_t a = 0;
          all_ok = all_ok && doc.error() == simdjson::SUCCESS;
          if (all_ok) {
            all_ok = (small.size() == 1
                          ? doc.value_unsafe().get_int64().get(a)
                          : doc.value_unsafe()["a"].get_int64().get(a)) == simdjson::SUCCESS &&
                     a == 1;
          }
        }
        all_ok = all_ok && parser.InputData() == input &&
                 parser.InputCapacity() == input_capacity && parser.Capacity() == parser_capacity;
        ++calls;
      }
    }
    attempts = scope.stats().attempts;
  }
  EXPECT_TRUE(all_ok);
  EXPECT_EQ(calls, 8u);
  EXPECT_EQ(attempts, 0u);
}

#ifdef VECTOR_OTEL_TEST_HAVE_MMAP
TEST(ParserWorkspaceTest, RejectsCPlusOneBeforePreparationOrCopy) {
  // A mapping with no access rights: any read of the payload would fault, so passing proves
  // the oversize decision is made from the length alone.
  const std::size_t mapped = kC + 1 + kPad;
  void* mem = mmap(nullptr, mapped, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(mem, MAP_FAILED);
  const char* data = static_cast<const char*>(mem);

  OtelParser parser;
  simdjson::error_code stage_sv = simdjson::SUCCESS;
  simdjson::error_code stage_padded = simdjson::SUCCESS;
  simdjson::error_code stage_unpadded = simdjson::SUCCESS;
  simdjson::error_code iterate_sv = simdjson::SUCCESS;
  simdjson::error_code iterate_padded = simdjson::SUCCESS;
  simdjson::error_code prepared = simdjson::SUCCESS;
  std::size_t attempts = 0;
  {
    CppFaultScope scope(CppFaultMode::kPersistent, 0);
    stage_sv = parser.StageInput(std::string_view(data, kC + 1)).error();
    stage_padded =
        parser.StageInput(simdjson::padded_string_view(data, kC + 1, kC + 1 + kPad)).error();
    stage_unpadded = parser.StageInput(simdjson::padded_string_view(data, kC + 1, kC + 1)).error();
    iterate_sv = parser.Iterate(std::string_view(data, kC + 1)).error();
    iterate_padded =
        parser.Iterate(simdjson::padded_string_view(data, kC + 1, kC + 1 + kPad)).error();
    prepared =
        parser.IteratePrepared(simdjson::padded_string_view(data, kC + 1, kC + 1 + kPad)).error();
    attempts = scope.stats().attempts;
  }
  munmap(mem, mapped);

  EXPECT_EQ(stage_sv, simdjson::CAPACITY);
  EXPECT_EQ(stage_padded, simdjson::CAPACITY);
  EXPECT_EQ(stage_unpadded, simdjson::CAPACITY);
  EXPECT_EQ(iterate_sv, simdjson::CAPACITY);
  EXPECT_EQ(iterate_padded, simdjson::CAPACITY);
  EXPECT_EQ(prepared, simdjson::CAPACITY);
  EXPECT_EQ(attempts, 0u);
  EXPECT_FALSE(parser.IsPrepared());
  EXPECT_EQ(parser.InputData(), nullptr);
  EXPECT_EQ(parser.InputCapacity(), 0u);
  EXPECT_EQ(parser.Capacity(), 0u);
}
#endif  // VECTOR_OTEL_TEST_HAVE_MMAP

TEST(ParserWorkspaceTest, EmbeddedNulIsCopiedAndRejectedByTheParser) {
  if (kC < 8) {
    GTEST_SKIP() << "payload limit below the 8-byte fixture";
  }
  OtelParser parser;
  const char raw[] = {'{', '"', 'a', '"', ':', '\0', '1', '}'};
  const std::string_view payload(raw, sizeof(raw));

  auto staged = parser.StageInput(payload);
  ASSERT_EQ(staged.error(), simdjson::SUCCESS);
  EXPECT_EQ(staged.value_unsafe().size(), payload.size());
  EXPECT_EQ(std::memcmp(staged.value_unsafe().data(), raw, sizeof(raw)), 0);

  auto doc = parser.IteratePrepared(staged.value_unsafe());
  bool rejected = doc.error() != simdjson::SUCCESS;
  if (!rejected) {
    int64_t a = 0;
    rejected = doc.value_unsafe()["a"].get_int64().get(a) != simdjson::SUCCESS;
  }
  EXPECT_TRUE(rejected);
}

TEST(ParserWorkspaceTest, SufficientlyPaddedCallerInputIsBorrowedNotCopied) {
  if (kC < 7) {
    GTEST_SKIP() << "payload limit below the 7-byte fixture";
  }
  OtelParser parser;
  ASSERT_EQ(parser.Prepare(), simdjson::SUCCESS);
  simdjson::padded_string owned(std::string_view(R"({"a":7})"));
  auto staged = parser.StageInput(simdjson::padded_string_view(owned));
  ASSERT_EQ(staged.error(), simdjson::SUCCESS);
  EXPECT_EQ(staged.value_unsafe().data(), owned.data());
  EXPECT_NE(staged.value_unsafe().data(), parser.InputData());
}

TEST(ParserWorkspaceTest, PreparationFailureIsRetryable) {
  // Count the allocations a successful Prepare performs.
  std::size_t total = 0;
  {
    OtelParser probe;
    CppFaultScope scope(CppFaultMode::kOneShot, static_cast<std::size_t>(-1));
    ASSERT_EQ(probe.Prepare(), simdjson::SUCCESS);
    total = scope.stats().attempts;
  }
  // input storage, string buffer, parser implementation, structural indexes, container stack
  // and array-flag stack.
  ASSERT_GE(total, 6u);

  for (std::size_t skip = 0; skip < total; ++skip) {
    SCOPED_TRACE("denied allocation index " + std::to_string(skip));
    OtelParser parser;
    simdjson::error_code err = simdjson::SUCCESS;
    std::size_t denied = 0;
    {
      CppFaultScope scope(CppFaultMode::kOneShot, skip);
      err = parser.Prepare();
      denied = scope.stats().denied;
    }
    EXPECT_EQ(denied, 1u);
    EXPECT_NE(err, simdjson::SUCCESS);
    EXPECT_FALSE(parser.IsPrepared());
    EXPECT_EQ(parser.Capacity(), 0u);
    EXPECT_EQ(parser.InputCapacity(), 0u);
    EXPECT_EQ(parser.InputData(), nullptr);

    ASSERT_EQ(parser.Prepare(), simdjson::SUCCESS);
    ASSERT_TRUE(parser.IsPrepared());
    // A string value exercises simdjson's string buffer, which must have been allocated.
    const bool fits = kC >= 13;
    auto staged =
        parser.StageInput(fits ? std::string_view(R"({"s":"hello"})") : std::string_view("1"));
    ASSERT_EQ(staged.error(), simdjson::SUCCESS);
    auto doc = parser.IteratePrepared(staged.value_unsafe());
    ASSERT_EQ(doc.error(), simdjson::SUCCESS);
    if (fits) {
      std::string_view s;
      ASSERT_EQ(doc.value_unsafe()["s"].get_string().get(s), simdjson::SUCCESS);
      EXPECT_EQ(s, "hello");
    } else {
      int64_t one = 0;
      ASSERT_EQ(doc.value_unsafe().get_int64().get(one), simdjson::SUCCESS);
      EXPECT_EQ(one, 1);
    }
  }

  OtelParser persistent;
  {
    CppFaultScope scope(CppFaultMode::kPersistent, 0);
    EXPECT_NE(persistent.Prepare(), simdjson::SUCCESS);
  }
  EXPECT_FALSE(persistent.IsPrepared());
  EXPECT_EQ(persistent.Prepare(), simdjson::SUCCESS);
}

TEST(ParserWorkspaceTest, TrimApisRetainThePreparedWorkspace) {
  OtelParser& parser = OtelParser::Instance();
  ASSERT_EQ(parser.Prepare(), simdjson::SUCCESS);
  const char* input = parser.InputData();
  const std::size_t input_capacity = parser.InputCapacity();
  const std::size_t capacity = parser.Capacity();

  parser.TrimCapacity();
  OtelParser::TrimThreadLocalBuffer();

  EXPECT_TRUE(parser.IsPrepared());
  EXPECT_EQ(parser.InputData(), input);
  EXPECT_EQ(parser.InputCapacity(), input_capacity);
  EXPECT_EQ(parser.Capacity(), capacity);
}

TEST(ParserWorkspaceTest, IteratePreparedRequiresAPreparedWorkspace) {
  OtelParser parser;
  simdjson::padded_string owned(std::string_view("1"));
  EXPECT_EQ(parser.IteratePrepared(simdjson::padded_string_view(owned)).error(),
            simdjson::UNINITIALIZED);
  EXPECT_FALSE(parser.IsPrepared());
}

TEST(ParserWorkspaceTest, ConfiguredLimitIsTheParserLimit) {
  EXPECT_EQ(OtelParser::kMaxPayloadSize, kMaxPayloadBytes);
}

}  // namespace
}  // namespace vector_otel::core
