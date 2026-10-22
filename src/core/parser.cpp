#include "vector_otel/core/parser.hpp"

#include <atomic>
#include <cstring>
#include <mutex>
#include <new>
#include <type_traits>

namespace vector_otel::core {

namespace {

static_assert(kPayloadPaddingBytes >= simdjson::SIMDJSON_PADDING,
              "payload padding must cover simdjson's SIMD over-read");

static_assert(std::is_nothrow_move_assignable_v<simdjson::ondemand::parser>,
              "Prepare() resets a failed parser by move assignment inside a noexcept function");

// simdjson's ondemand::parser::allocate() ignores a failed (nothrow) string-buffer allocation
// and still reports SUCCESS, leaving a null buffer that the first parsed string would write
// through. The buffer is a private member, so read it through the explicit-instantiation access
// idiom (which names the member without modifying the vendored sources). A rename in a future
// simdjson release is a compile error here, not a silent regression.
struct StringBufAccess {
  using Type = std::unique_ptr<uint8_t[]> simdjson::ondemand::parser::*;
  friend Type StringBufMember(StringBufAccess);
};
template <StringBufAccess::Type Member>
struct StringBufExposer {
  friend StringBufAccess::Type StringBufMember(StringBufAccess) { return Member; }
};
template struct StringBufExposer<&simdjson::ondemand::parser::string_buf>;

bool HasStringBuffer(const simdjson::ondemand::parser& parser) noexcept {
  return (parser.*StringBufMember(StringBufAccess{})) != nullptr;
}

}  // namespace

OtelParser& OtelParser::Instance() {
  // simdjson selects its CPU implementation lazily with unsynchronised first-use writes, which is
  // a data race when several worker threads parse for the first time concurrently (ThreadSanitizer
  // reports it). Force the selection once, behind an explicit acquire/release flag, before any
  // thread's first parse.
  static std::atomic<bool> implementation_selected{false};
  if (!implementation_selected.load(std::memory_order_acquire)) [[unlikely]] {
    static std::mutex select_mutex;
    const std::lock_guard<std::mutex> lock(select_mutex);
    if (!implementation_selected.load(std::memory_order_relaxed)) {
      (void)simdjson::get_active_implementation();
      implementation_selected.store(true, std::memory_order_release);
    }
  }
  static thread_local OtelParser instance;
  return instance;
}

simdjson::error_code OtelParser::Prepare() noexcept {
  if (ready_) [[likely]] {
    return simdjson::SUCCESS;
  }
  constexpr size_t kInputBytes = kMaxPayloadBytes + kPayloadPaddingBytes;
  std::unique_ptr<char[]> input(new (std::nothrow) char[kInputBytes]);
  if (!input) {
    return simdjson::MEMALLOC;
  }
  simdjson::error_code err = parser_.allocate(kWorkspaceCapacity);
  if (err == simdjson::SUCCESS && !HasStringBuffer(parser_)) {
    err = simdjson::MEMALLOC;
  }
  if (err != simdjson::SUCCESS) {
    // Drop whatever the failed allocation left behind so nothing is retained half-built.
    parser_ = simdjson::ondemand::parser(kWorkspaceCapacity);
    return err;
  }
  input_ = std::move(input);
  input_capacity_ = kInputBytes;
  ready_ = true;
  return simdjson::SUCCESS;
}

simdjson::simdjson_result<simdjson::padded_string_view> OtelParser::StageInput(
    std::string_view json_str) noexcept {
  if (json_str.size() > kMaxPayloadSize) [[unlikely]] {
    return simdjson::CAPACITY;
  }
  if (const simdjson::error_code err = Prepare(); err != simdjson::SUCCESS) [[unlikely]] {
    return err;
  }
  if (!json_str.empty()) {
    std::memcpy(input_.get(), json_str.data(), json_str.size());
  }
  std::memset(input_.get() + json_str.size(), 0, kPayloadPaddingBytes);
  return simdjson::padded_string_view(input_.get(), json_str.size(),
                                      json_str.size() + kPayloadPaddingBytes);
}

simdjson::simdjson_result<simdjson::padded_string_view> OtelParser::StageInput(
    simdjson::padded_string_view json_str) noexcept {
  if (json_str.size() > kMaxPayloadSize) [[unlikely]] {
    return simdjson::CAPACITY;
  }
  if (json_str.has_sufficient_padding()) [[likely]] {
    return json_str;
  }
  return StageInput(std::string_view(json_str.data(), json_str.size()));
}

simdjson::simdjson_result<simdjson::ondemand::document> OtelParser::IteratePrepared(
    simdjson::padded_string_view json_str) noexcept {
  if (json_str.size() > kMaxPayloadSize) [[unlikely]] {
    return simdjson::CAPACITY;
  }
  if (!ready_) [[unlikely]] {
    return simdjson::UNINITIALIZED;
  }
  return parser_.iterate(json_str);
}

simdjson::simdjson_result<simdjson::ondemand::document> OtelParser::Iterate(
    std::string_view json_str) {
  auto staged = StageInput(json_str);
  if (staged.error() != simdjson::SUCCESS) [[unlikely]] {
    return staged.error();
  }
  return IteratePrepared(staged.value_unsafe());
}

simdjson::simdjson_result<simdjson::ondemand::document> OtelParser::Iterate(
    simdjson::padded_string_view json_str) {
  auto staged = StageInput(json_str);
  if (staged.error() != simdjson::SUCCESS) [[unlikely]] {
    return staged.error();
  }
  if (const simdjson::error_code err = Prepare(); err != simdjson::SUCCESS) [[unlikely]] {
    return err;
  }
  return IteratePrepared(staged.value_unsafe());
}

}  // namespace vector_otel::core
