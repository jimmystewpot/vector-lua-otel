#include "vector_otel/core/fast_json_writer.hpp"

#include <charconv>
#include <cmath>

namespace vector_otel {
namespace core {

void FastJsonWriter::Reset() {
  buffer_.clear();
  stack_.clear();
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

void FastJsonWriter::AppendEscapedString(std::string_view sv) {
  buffer_.push_back('"');
  static constexpr char kHexDigits[] = "0123456789abcdef";

  size_t start = 0;
  const size_t len = sv.size();
  while (start < len) {
    size_t i = start;
    while (i < len) {
      const auto c = static_cast<unsigned char>(sv[i]);
      if (c == '"' || c == '\\' || c < 0x20) [[unlikely]] {
        break;
      }
      ++i;
    }
    if (i > start) {
      buffer_.insert(buffer_.end(), sv.data() + start, sv.data() + i);
    }
    if (i < len) [[unlikely]] {
      const auto c = static_cast<unsigned char>(sv[i]);
      switch (c) {
        case '"':
          buffer_.push_back('\\');
          buffer_.push_back('"');
          break;
        case '\\':
          buffer_.push_back('\\');
          buffer_.push_back('\\');
          break;
        case '\b':
          buffer_.push_back('\\');
          buffer_.push_back('b');
          break;
        case '\f':
          buffer_.push_back('\\');
          buffer_.push_back('f');
          break;
        case '\n':
          buffer_.push_back('\\');
          buffer_.push_back('n');
          break;
        case '\r':
          buffer_.push_back('\\');
          buffer_.push_back('r');
          break;
        case '\t':
          buffer_.push_back('\\');
          buffer_.push_back('t');
          break;
        default:
          buffer_.push_back('\\');
          buffer_.push_back('u');
          buffer_.push_back('0');
          buffer_.push_back('0');
          buffer_.push_back(kHexDigits[(c >> 4) & 0x0f]);
          buffer_.push_back(kHexDigits[c & 0x0f]);
          break;
      }
      start = i + 1;
    } else {
      start = i;
    }
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
  AppendEscapedString(key);
  buffer_.push_back(':');
}

void FastJsonWriter::String(std::string_view val) {
  BeforeValue();
  AppendEscapedString(val);
}

void FastJsonWriter::RawJson(std::string_view raw) {
  BeforeValue();
  buffer_.insert(buffer_.end(), raw.begin(), raw.end());
}

void FastJsonWriter::Int64(int64_t val) {
  BeforeValue();
  char buf[32];
  const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), val);
  buffer_.insert(buffer_.end(), buf, ptr);
}

void FastJsonWriter::Uint64(uint64_t val) {
  BeforeValue();
  char buf[32];
  const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), val);
  buffer_.insert(buffer_.end(), buf, ptr);
}

void FastJsonWriter::Double(double val) {
  BeforeValue();
  if (std::isnan(val) || std::isinf(val)) [[unlikely]] {
    buffer_.insert(buffer_.end(), {'n', 'u', 'l', 'l'});
    return;
  }
  char buf[64];
  const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), val);
  buffer_.insert(buffer_.end(), buf, ptr);
}

void FastJsonWriter::Bool(bool val) {
  BeforeValue();
  if (val) {
    buffer_.insert(buffer_.end(), {'t', 'r', 'u', 'e'});
  } else {
    buffer_.insert(buffer_.end(), {'f', 'a', 'l', 's', 'e'});
  }
}

void FastJsonWriter::Null() {
  BeforeValue();
  buffer_.insert(buffer_.end(), {'n', 'u', 'l', 'l'});
}

std::string_view FastJsonWriter::Str() const {
  return std::string_view(buffer_.data(), buffer_.size());
}

}  // namespace core
}  // namespace vector_otel
