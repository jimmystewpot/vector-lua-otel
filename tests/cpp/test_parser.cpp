#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#include <sys/mman.h>
#endif

#include "vector_otel/core/parser.hpp"

namespace vector_otel::core {

TEST(OtelParserTest, UnpaddedStringViewSlice) {
  OtelParser parser;

  // 1. Raw buffer without null termination or trailing padding (exactly 13
  // bytes)
  const char raw_unpadded[] = {'{', '"', 'a', '"', ':', ' ', '1',
                               '2', '3', '4', '5', '}', '!'};
  std::string_view unpadded_slice(raw_unpadded, 12);  // exactly `{"a": 12345}`

  auto doc_res = parser.Iterate(unpadded_slice);
  ASSERT_EQ(doc_res.error(), simdjson::SUCCESS);
  auto doc = std::move(doc_res.value());
  int64_t val = 0;
  ASSERT_EQ(doc["a"].get_int64().get(val), simdjson::SUCCESS);
  EXPECT_EQ(val, 12345);

  // 2. Embedded slice followed immediately by non-whitespace garbage
  std::string valid_json = "{\"status\":200,\"msg\":\"ok\"}";
  std::string embedded = valid_json + "TRAILING_GARBAGE_NO_PADDING";
  std::string_view embedded_slice(embedded.data(), valid_json.size());

  auto doc2_res = parser.Iterate(embedded_slice);
  ASSERT_EQ(doc2_res.error(), simdjson::SUCCESS);
  auto doc2 = std::move(doc2_res.value());
  int64_t status = 0;
  ASSERT_EQ(doc2["status"].get_int64().get(status), simdjson::SUCCESS);
  EXPECT_EQ(status, 200);
  std::string_view msg;
  ASSERT_EQ(doc2["msg"].get_string().get(msg), simdjson::SUCCESS);
  EXPECT_EQ(msg, "ok");
}

TEST(OtelParserTest, MalformedTruncatedJsonErrorPropagation) {
  OtelParser parser;

  // 1. Truncated array: {"items": [1, 2, 3
  std::string_view truncated_array = "{\"items\": [1, 2, 3";
  auto doc_res = parser.Iterate(truncated_array);
  if (doc_res.error() == simdjson::SUCCESS) {
    auto doc = std::move(doc_res.value());
    auto items_res = doc["items"];
    if (items_res.error() == simdjson::SUCCESS) {
      auto arr_res = items_res.get_array();
      if (arr_res.error() == simdjson::SUCCESS) {
        simdjson::error_code last_err = simdjson::SUCCESS;
        for (auto elem : arr_res.value()) {
          if (elem.error() != simdjson::SUCCESS) {
            last_err = elem.error();
            break;
          }
        }
        EXPECT_EQ(last_err, simdjson::INCOMPLETE_ARRAY_OR_OBJECT);
      } else {
        EXPECT_EQ(arr_res.error(), simdjson::INCOMPLETE_ARRAY_OR_OBJECT);
      }
    } else {
      EXPECT_EQ(items_res.error(), simdjson::INCOMPLETE_ARRAY_OR_OBJECT);
    }
  } else {
    EXPECT_EQ(doc_res.error(), simdjson::INCOMPLETE_ARRAY_OR_OBJECT);
  }

  // 2. Truncated object: {"unclosed":
  std::string_view truncated_obj = "{\"unclosed\":";
  auto doc2_res = parser.Iterate(truncated_obj);
  if (doc2_res.error() == simdjson::SUCCESS) {
    auto doc2 = std::move(doc2_res.value());
    auto val_res = doc2["unclosed"];
    EXPECT_TRUE(val_res.error() == simdjson::INCOMPLETE_ARRAY_OR_OBJECT ||
                val_res.error() == simdjson::TAPE_ERROR);
  } else {
    EXPECT_TRUE(doc2_res.error() == simdjson::INCOMPLETE_ARRAY_OR_OBJECT ||
                doc2_res.error() == simdjson::TAPE_ERROR);
  }

  // 3. Completely invalid JSON
  std::string_view invalid_json = "not json at all";
  auto doc3_res = parser.Iterate(invalid_json);
  if (doc3_res.error() == simdjson::SUCCESS) {
    auto doc3 = std::move(doc3_res.value());
    auto obj_res = doc3.get_object();
    EXPECT_NE(obj_res.error(), simdjson::SUCCESS);
  } else {
    EXPECT_NE(doc3_res.error(), simdjson::SUCCESS);
  }
}

TEST(OtelParserTest, MultiPassThreadLocalReuseNoRealloc) {
  // Use a thread-local OtelParser instance
  static thread_local OtelParser parser;

  // Pass 1: Initial payload to prime capacity
  std::string p1 =
      "{\"name\":\"test_service\",\"counter\":1,\"message\":\"first payload to "
      "prime capacity\"}";
  auto doc1_res = parser.Iterate(p1);
  ASSERT_EQ(doc1_res.error(), simdjson::SUCCESS);
  auto doc1 = std::move(doc1_res.value());
  int64_t c1 = 0;
  ASSERT_EQ(doc1["counter"].get_int64().get(c1), simdjson::SUCCESS);
  EXPECT_EQ(c1, 1);

  const char* initial_buf_ptr = OtelParser::ThreadLocalBufferData();
  size_t initial_cap = OtelParser::ThreadLocalBufferCapacity();
  ASSERT_NE(initial_buf_ptr, nullptr);
  ASSERT_GE(initial_cap, p1.size() + simdjson::SIMDJSON_PADDING);

  // Passes 2 to 1000: Reuse thread-local parser without reallocation
  for (int i = 2; i <= 1000; ++i) {
    std::string json =
        "{\"counter\":" + std::to_string(i) + ",\"status\":\"active\"}";
    auto doc_res = parser.Iterate(json);
    ASSERT_EQ(doc_res.error(), simdjson::SUCCESS);
    auto doc = std::move(doc_res.value());
    int64_t counter = 0;
    ASSERT_EQ(doc["counter"].get_int64().get(counter), simdjson::SUCCESS);
    EXPECT_EQ(counter, i);

    // Verify pointer and capacity did not change (0 reallocations)
    EXPECT_EQ(OtelParser::ThreadLocalBufferData(), initial_buf_ptr);
    EXPECT_EQ(OtelParser::ThreadLocalBufferCapacity(), initial_cap);
  }
}

TEST(OtelParserTest, MultiThreadIsolation) {
  constexpr int kNumThreads = 4;
  std::vector<std::thread> threads;
  std::vector<const char*> thread_buffers(kNumThreads, nullptr);
  std::atomic<int> threads_ready{0};
  std::atomic<bool> can_exit{false};

  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([t, &thread_buffers, &threads_ready, &can_exit]() {
      OtelParser local_parser;
      for (int i = 0; i < 50; ++i) {
        std::string json = "{\"thread_id\":" + std::to_string(t) +
                           ",\"iter\":" + std::to_string(i) + "}";
        auto doc_res = local_parser.Iterate(json);
        ASSERT_EQ(doc_res.error(), simdjson::SUCCESS);
        auto doc = std::move(doc_res.value());
        int64_t thread_id = -1;
        ASSERT_EQ(doc["thread_id"].get_int64().get(thread_id),
                  simdjson::SUCCESS);
        EXPECT_EQ(thread_id, t);
      }
      thread_buffers[t] = OtelParser::ThreadLocalBufferData();
      ASSERT_NE(thread_buffers[t], nullptr);

      threads_ready.fetch_add(1, std::memory_order_release);
      while (!can_exit.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    });
  }

  while (threads_ready.load(std::memory_order_acquire) < kNumThreads) {
    std::this_thread::yield();
  }

  // Verify all threads had distinct thread-local buffers while all are alive
  for (int i = 0; i < kNumThreads; ++i) {
    for (int j = i + 1; j < kNumThreads; ++j) {
      EXPECT_NE(thread_buffers[i], thread_buffers[j]);
    }
  }

  can_exit.store(true, std::memory_order_release);

  for (auto& th : threads) {
    th.join();
  }
}

TEST(OtelParserTest, SafelyPaddedInputZeroCopy) {
  OtelParser parser;

  // 1. simdjson::padded_string
  simdjson::padded_string ps(
      std::string_view("{\"padded\":true,\"value\":999}"));
  auto doc1_res = parser.Iterate(simdjson::padded_string_view(ps));
  ASSERT_EQ(doc1_res.error(), simdjson::SUCCESS);
  auto doc1 = std::move(doc1_res.value());
  int64_t val1 = 0;
  ASSERT_EQ(doc1["value"].get_int64().get(val1), simdjson::SUCCESS);
  EXPECT_EQ(val1, 999);

  // 2. std::string_view with >= 64 trailing whitespace bytes
  std::string with_whitespace = "{\"padded_ws\":42}";
  with_whitespace.append(simdjson::SIMDJSON_PADDING, ' ');
  std::string_view sv_padded(with_whitespace);

  auto doc2_res = parser.Iterate(sv_padded);
  ASSERT_EQ(doc2_res.error(), simdjson::SUCCESS);
  auto doc2 = std::move(doc2_res.value());
  int64_t val2 = 0;
  ASSERT_EQ(doc2["padded_ws"].get_int64().get(val2), simdjson::SUCCESS);
  EXPECT_EQ(val2, 42);
}

class ParserTest : public ::testing::Test {
 protected:
  OtelParser parser_;
};

TEST_F(ParserTest, PaddedStringViewIterateDoesNotSlice) {
  simdjson::padded_string valid_json(std::string_view("{\"test\": 123}"));
  simdjson::padded_string_view psv(valid_json);
  auto doc_res = parser_.Iterate(psv);
  ASSERT_EQ(doc_res.error(), simdjson::SUCCESS);
}

TEST_F(ParserTest, OversizedPayloadReturnsCapacityError) {
  EXPECT_EQ(core::OtelParser::kMaxPayloadSize, 512 * 1024 * 1024);

  core::OtelParser parser;

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
  // Allocate virtual address space without physical RAM allocation to test
  // oversized payload safely.
  const size_t test_size = core::OtelParser::kMaxPayloadSize + 4096;
  void* mem = mmap(nullptr, test_size + simdjson::SIMDJSON_PADDING,
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem != MAP_FAILED) {
    char* buf = static_cast<char*>(mem);
    buf[0] = '{';
    buf[1] = '}';

    // 1. Test std::string_view overload
    std::string_view oversized_sv(buf, test_size);
    auto doc_res1 = parser.Iterate(oversized_sv);
    EXPECT_EQ(doc_res1.error(), simdjson::CAPACITY);

    // 2. Test simdjson::padded_string_view overload with sufficient padding
    simdjson::padded_string_view oversized_padded(
        buf, test_size, test_size + simdjson::SIMDJSON_PADDING);
    auto doc_res2 = parser.Iterate(oversized_padded);
    EXPECT_EQ(doc_res2.error(), simdjson::CAPACITY);

    // 3. Test simdjson::padded_string_view overload with insufficient padding
    simdjson::padded_string_view oversized_unpadded(buf, test_size, test_size);
    auto doc_res3 = parser.Iterate(oversized_unpadded);
    EXPECT_EQ(doc_res3.error(), simdjson::CAPACITY);

    munmap(mem, test_size + simdjson::SIMDJSON_PADDING);
  } else {
    std::string valid_json = "{\"status\":\"ok\"}";
    auto doc_res = parser.Iterate(valid_json);
    EXPECT_EQ(doc_res.error(), simdjson::SUCCESS);
  }
#else
  std::string valid_json = "{\"status\":\"ok\"}";
  auto doc_res = parser.Iterate(valid_json);
  EXPECT_EQ(doc_res.error(), simdjson::SUCCESS);
#endif
}

TEST_F(ParserTest, TrimThreadLocalBufferReclaimsMemory) {
  core::OtelParser parser;
  std::string large_json(17 * 1024 * 1024, ' ');
  large_json.front() = '[';
  large_json.back() = ']';
  auto res = parser.Iterate(large_json);
  EXPECT_GE(core::OtelParser::ThreadLocalBufferCapacity(), 17 * 1024 * 1024);
  core::OtelParser::TrimThreadLocalBuffer();
  EXPECT_LT(core::OtelParser::ThreadLocalBufferCapacity(), 17 * 1024 * 1024);
}

TEST_F(ParserTest, SharedInstanceTrimsParserCapacityAboveThreshold) {
  auto& parser = core::OtelParser::Instance();
  std::string large_json =
      R"({"key":")" + std::string(17 * 1024 * 1024, 'x') + R"("})";
  auto doc_res = parser.Iterate(std::string_view(large_json));
  ASSERT_EQ(doc_res.error(), simdjson::SUCCESS);
  EXPECT_GT(parser.Capacity(), core::OtelParser::kMaxRetainedBufferCapacity);

  core::OtelParser::TrimThreadLocalBuffer();
  EXPECT_LE(parser.Capacity(), core::OtelParser::kMaxRetainedBufferCapacity);
}

}  // namespace vector_otel::core
