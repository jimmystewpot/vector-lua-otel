// Tests for the test-only C++ allocation fault harness (tests/support/fault_new.*).
//
// Assertions that use gtest machinery run AFTER the CppFaultScope under test has been destroyed
// so that gtest's own allocations are never denied. Pointers and stats are captured inside the
// scope into plain stack storage.
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <thread>

#include "fault_new.hpp"
#include "simdjson.h"
#include "vector_otel/core/fast_json_writer.hpp"

namespace vector_otel::test {
namespace {

// Prevents the optimiser from eliding allocate/free pairs.
inline void Escape(void* p) { asm volatile("" : : "r"(p) : "memory"); }

struct alignas(64) OverAligned {
  char bytes[64];
};

TEST(CppFaultHarness, DisabledOutsideAnyScopeNeverFails) {
  void* p = ::operator new(16, std::nothrow);
  Escape(p);
  ASSERT_NE(p, nullptr);
  ::operator delete(p);
}

TEST(CppFaultHarness, OneShotDeniesExactlyTheSelectedAttempt) {
  void* p[4] = {};
  CppFaultStats stats;
  {
    CppFaultScope scope(CppFaultMode::kOneShot, 2);
    for (auto& slot : p) {
      slot = ::operator new(16, std::nothrow);
      Escape(slot);
    }
    stats = scope.stats();
  }
  EXPECT_NE(p[0], nullptr);
  EXPECT_NE(p[1], nullptr);
  EXPECT_EQ(p[2], nullptr);
  EXPECT_NE(p[3], nullptr);
  EXPECT_EQ(stats.attempts, 4u);
  EXPECT_EQ(stats.denied, 1u);
  for (void* q : p) {
    ::operator delete(q);
  }
}

TEST(CppFaultHarness, PersistentDeniesEveryAttemptAfterTheSkippedOnes) {
  void* p[4] = {};
  CppFaultStats stats;
  {
    CppFaultScope scope(CppFaultMode::kPersistent, 1);
    for (auto& slot : p) {
      slot = ::operator new(16, std::nothrow);
      Escape(slot);
    }
    stats = scope.stats();
  }
  EXPECT_NE(p[0], nullptr);
  EXPECT_EQ(p[1], nullptr);
  EXPECT_EQ(p[2], nullptr);
  EXPECT_EQ(p[3], nullptr);
  EXPECT_EQ(stats.attempts, 4u);
  EXPECT_EQ(stats.denied, 3u);
  ::operator delete(p[0]);
}

TEST(CppFaultHarness, AllocationRecoversAfterScopeExit) {
  {
    CppFaultScope scope(CppFaultMode::kPersistent, 0);
    void* denied = ::operator new(16, std::nothrow);
    Escape(denied);
    EXPECT_EQ(denied, nullptr);
  }
  void* p = ::operator new(16, std::nothrow);
  Escape(p);
  EXPECT_NE(p, nullptr);
  ::operator delete(p);
}

TEST(CppFaultHarness, ThrowingFormsThrowBadAllocAndNothrowFormsReturnNull) {
  bool single_threw = false;
  bool array_threw = false;
  void* nothrow_single = reinterpret_cast<void*>(1);
  void* nothrow_array = reinterpret_cast<void*>(1);
  CppFaultStats stats;
  {
    CppFaultScope scope(CppFaultMode::kPersistent, 0);
    try {
      Escape(::operator new(16));
    } catch (const std::bad_alloc&) {
      single_threw = true;
    }
    try {
      Escape(::operator new[](16));
    } catch (const std::bad_alloc&) {
      array_threw = true;
    }
    nothrow_single = ::operator new(16, std::nothrow);
    nothrow_array = ::operator new[](16, std::nothrow);
    stats = scope.stats();
  }
  EXPECT_TRUE(single_threw);
  EXPECT_TRUE(array_threw);
  EXPECT_EQ(nothrow_single, nullptr);
  EXPECT_EQ(nothrow_array, nullptr);
  EXPECT_EQ(stats.attempts, 4u);
  EXPECT_EQ(stats.denied, 4u);
}

TEST(CppFaultHarness, ArrayFormsAreInterceptedAndPairWithArrayDelete) {
  void* first = nullptr;
  void* second = nullptr;
  {
    CppFaultScope scope(CppFaultMode::kOneShot, 0);
    first = ::operator new[](16, std::nothrow);
    second = ::operator new[](16, std::nothrow);
    Escape(first);
    Escape(second);
  }
  EXPECT_EQ(first, nullptr);
  ASSERT_NE(second, nullptr);
  ::operator delete[](second);
}

TEST(CppFaultHarness, AlignedFormsAreInterceptedAndHonourAlignment) {
  // Outside any scope: real aligned allocation, correct alignment, paired deletes.
  void* ok = ::operator new(256, std::align_val_t{64}, std::nothrow);
  Escape(ok);
  ASSERT_NE(ok, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(ok) % 64, 0u);
  ::operator delete(ok, std::align_val_t{64});
  void* ok_arr = ::operator new[](256, std::align_val_t{64}, std::nothrow);
  Escape(ok_arr);
  ASSERT_NE(ok_arr, nullptr);
  ::operator delete[](ok_arr, 256, std::align_val_t{64});

  // Over-aligned new-expression is routed through the aligned operator.
  auto* typed = new (std::nothrow) OverAligned;
  Escape(typed);
  ASSERT_NE(typed, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(typed) % 64, 0u);
  delete typed;

  void* nothrow_single = reinterpret_cast<void*>(1);
  void* nothrow_array = reinterpret_cast<void*>(1);
  OverAligned* typed_denied = reinterpret_cast<OverAligned*>(1);
  bool threw = false;
  CppFaultStats stats;
  {
    CppFaultScope scope(CppFaultMode::kPersistent, 0);
    nothrow_single = ::operator new(256, std::align_val_t{64}, std::nothrow);
    nothrow_array = ::operator new[](256, std::align_val_t{64}, std::nothrow);
    typed_denied = new (std::nothrow) OverAligned;
    try {
      Escape(::operator new(256, std::align_val_t{64}));
    } catch (const std::bad_alloc&) {
      threw = true;
    }
    stats = scope.stats();
  }
  EXPECT_EQ(nothrow_single, nullptr);
  EXPECT_EQ(nothrow_array, nullptr);
  EXPECT_EQ(typed_denied, nullptr);
  EXPECT_TRUE(threw);
  // Counting proves the aligned forms went through the replacement, not the library default.
  EXPECT_EQ(stats.attempts, 4u);
  EXPECT_EQ(stats.denied, 4u);
}

TEST(CppFaultHarness, NestedScopesRestoreTheOuterPlanAndProgress) {
  void* a = nullptr;
  void* inner_a = reinterpret_cast<void*>(1);
  void* inner_b = reinterpret_cast<void*>(1);
  void* b = nullptr;
  void* c = reinterpret_cast<void*>(1);
  void* d = nullptr;
  CppFaultStats inner_stats;
  CppFaultStats outer_mid;
  CppFaultStats outer_end;
  {
    CppFaultScope outer(CppFaultMode::kOneShot, 1);  // deny the second attempt overall
    a = ::operator new(16, std::nothrow);            // outer attempt 0: allowed
    Escape(a);
    {
      CppFaultScope inner(CppFaultMode::kPersistent, 0);
      inner_a = ::operator new(16, std::nothrow);
      inner_b = ::operator new(16, std::nothrow);
      inner_stats = inner.stats();
    }
    outer_mid = outer.stats();             // inner activity must not leak into the outer counters
    b = ::operator new(16, std::nothrow);  // outer attempt 1: the one-shot denial fires now
    c = ::operator new(16, std::nothrow);  // outer attempt 2: one-shot is already consumed
    d = ::operator new(16, std::nothrow);
    Escape(c);
    Escape(d);
    outer_end = outer.stats();
  }
  EXPECT_NE(a, nullptr);
  EXPECT_EQ(inner_a, nullptr);
  EXPECT_EQ(inner_b, nullptr);
  EXPECT_EQ(inner_stats.attempts, 2u);
  EXPECT_EQ(inner_stats.denied, 2u);
  EXPECT_EQ(outer_mid.attempts, 1u);
  EXPECT_EQ(outer_mid.denied, 0u);
  EXPECT_EQ(b, nullptr);
  EXPECT_NE(c, nullptr);
  EXPECT_NE(d, nullptr);
  EXPECT_EQ(outer_end.attempts, 4u);
  EXPECT_EQ(outer_end.denied, 1u);
  ::operator delete(a);
  ::operator delete(c);
  ::operator delete(d);
}

TEST(CppFaultHarness, ScopeOnOneThreadDoesNotAffectAnotherThread) {
  std::atomic<bool> go{false};
  std::atomic<bool> done{false};
  std::atomic<bool> other_ok{false};
  std::thread other([&] {
    while (!go.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    void* p = ::operator new(16, std::nothrow);
    Escape(p);
    other_ok.store(p != nullptr, std::memory_order_release);
    ::operator delete(p);
    done.store(true, std::memory_order_release);
  });
  void* mine = reinterpret_cast<void*>(1);
  {
    CppFaultScope scope(CppFaultMode::kPersistent, 0);
    go.store(true, std::memory_order_release);
    while (!done.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    mine = ::operator new(16, std::nothrow);
  }
  other.join();
  EXPECT_TRUE(other_ok.load());
  EXPECT_EQ(mine, nullptr);
}

// F1 preservation: Trim() must stay non-throwing and leave a usable, reduced-capacity writer when
// every allocation is denied.
TEST(CppFaultHarness, WriterTrimUnderPersistentDenialDegradesToEmptyAndRecovers) {
  core::FastJsonWriter writer;
  writer.String(std::string(core::FastJsonWriter::kMaxRetainedCapacity + 1, 'x'));
  ASSERT_GT(writer.Capacity(), core::FastJsonWriter::kMaxRetainedCapacity);

  CppFaultStats stats;
  {
    CppFaultScope scope(CppFaultMode::kPersistent, 0);
    writer.Trim();
    stats = scope.stats();
  }
  EXPECT_GT(stats.denied, 0u);  // the post-release reserve was actually attempted and refused
  EXPECT_TRUE(writer.Str().empty());
  EXPECT_LE(writer.Capacity(), core::FastJsonWriter::kMaxRetainedCapacity);

  writer.Reset();
  writer.StartObject();
  writer.Key("a");
  writer.Int64(1);
  writer.EndObject();
  EXPECT_EQ(writer.Str(), "{\"a\":1}");
}

// simdjson allocates with new (std::nothrow); the harness must reach it.
TEST(CppFaultHarness, SimdjsonParserAllocateReportsMemallocUnderDenial) {
  simdjson::ondemand::parser parser;
  simdjson::error_code denied_code = simdjson::SUCCESS;
  CppFaultStats stats;
  {
    CppFaultScope scope(CppFaultMode::kPersistent, 0);
    denied_code = parser.allocate(1024 * 1024);
    stats = scope.stats();
  }
  EXPECT_EQ(denied_code, simdjson::MEMALLOC);
  EXPECT_GT(stats.denied, 0u);
  EXPECT_EQ(parser.allocate(1024 * 1024), simdjson::SUCCESS);
}

}  // namespace
}  // namespace vector_otel::test
