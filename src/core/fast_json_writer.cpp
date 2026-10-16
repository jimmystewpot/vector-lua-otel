#include "vector_otel/core/fast_json_writer.hpp"

#include <charconv>
#include <cmath>
#include <utility>

namespace vector_otel {
namespace core {

FastJsonWriter::FastJsonWriter() {
  buffer_.reserve(4096);
  stack_.reserve(32);
}

void FastJsonWriter::Reset() {
  buffer_.clear();
  stack_.clear();
}

void FastJsonWriter::Trim() noexcept {
  if (buffer_.capacity() > kMaxRetainedCapacity) {
    std::string trimmed;
    trimmed.reserve(4096);
    buffer_ = std::move(trimmed);
    stack_.clear();
  }
}

void FastJsonWriter::BeforeValue() {
  if (!stack_.empty()) [[likely]] {
    auto& top = stack_.back();
    if (top.type == ContainerType::kArray) {
      if (!top.empty) {
        buffer_.push_back(',');
      }
      top.empty = false;
    }
  }
}

void FastJsonWriter::WriteEscapedString(std::string_view sv) {
  buffer_.reserve(buffer_.size() + sv.size() + 2);
  buffer_.push_back('"');
  static constexpr char kHexDigits[] = "0123456789abcdef";

  size_t chunk_start = 0;
  const size_t len = sv.size();
  size_t i = 0;
  while (i < len) {
    const auto c = static_cast<unsigned char>(sv[i]);
    if (c == '"' || c == '\\' || c < 0x20) [[unlikely]] {
      if (i > chunk_start) {
        buffer_.append(sv.data() + chunk_start, i - chunk_start);
      }
      switch (c) {
        case '"':
          buffer_.append("\\\"", 2);
          break;
        case '\\':
          buffer_.append("\\\\", 2);
          break;
        case '\b':
          buffer_.append("\\b", 2);
          break;
        case '\f':
          buffer_.append("\\f", 2);
          break;
        case '\n':
          buffer_.append("\\n", 2);
          break;
        case '\r':
          buffer_.append("\\r", 2);
          break;
        case '\t':
          buffer_.append("\\t", 2);
          break;
        default: {
          const char hex_esc[6] = {
              '\\', 'u', '0', '0', kHexDigits[(c >> 4) & 0x0f], kHexDigits[c & 0x0f]};
          buffer_.append(hex_esc, 6);
          break;
        }
      }
      chunk_start = i + 1;
    }
    ++i;
  }
  if (i > chunk_start) {
    buffer_.append(sv.data() + chunk_start, i - chunk_start);
  }
  buffer_.push_back('"');
}

void FastJsonWriter::StartObject() {
  BeforeValue();
  buffer_.push_back('{');
  stack_.push_back({ContainerType::kObject, true});
}

void FastJsonWriter::EndObject() {
  if (!stack_.empty() && stack_.back().type == ContainerType::kObject) [[likely]] {
    stack_.pop_back();
  }
  buffer_.push_back('}');
}

void FastJsonWriter::StartArray() {
  BeforeValue();
  buffer_.push_back('[');
  stack_.push_back({ContainerType::kArray, true});
}

void FastJsonWriter::EndArray() {
  if (!stack_.empty() && stack_.back().type == ContainerType::kArray) [[likely]] {
    stack_.pop_back();
  }
  buffer_.push_back(']');
}

void FastJsonWriter::Key(std::string_view key) {
  if (!stack_.empty() && stack_.back().type == ContainerType::kObject) [[likely]] {
    if (!stack_.back().empty) {
      buffer_.push_back(',');
    }
    stack_.back().empty = false;
  }
  WriteEscapedString(key);
  buffer_.push_back(':');
}

void FastJsonWriter::String(std::string_view val) {
  BeforeValue();
  WriteEscapedString(val);
}

void FastJsonWriter::RawJson(std::string_view raw) {
  BeforeValue();
  buffer_.append(raw.data(), raw.size());
}

void FastJsonWriter::Int64(int64_t val) {
  BeforeValue();
  char buf[32];
  const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), val);
  buffer_.append(buf, static_cast<size_t>(ptr - buf));
}

void FastJsonWriter::Uint64(uint64_t val) {
  BeforeValue();
  char buf[32];
  const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), val);
  buffer_.append(buf, static_cast<size_t>(ptr - buf));
}

void FastJsonWriter::Double(double val) {
  BeforeValue();
  if (std::isnan(val) || std::isinf(val)) [[unlikely]] {
    buffer_.append("null", 4);
    return;
  }
  char buf[64];
  const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), val);
  buffer_.append(buf, static_cast<size_t>(ptr - buf));
}

void FastJsonWriter::Bool(bool val) {
  BeforeValue();
  if (val) {
    buffer_.append("true", 4);
  } else {
    buffer_.append("false", 5);
  }
}

void FastJsonWriter::Null() {
  BeforeValue();
  buffer_.append("null", 4);
}

std::string_view FastJsonWriter::Str() const {
  return std::string_view(buffer_.data(), buffer_.size());
}

}  // namespace core
}  // namespace vector_otel
