#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vector_otel::core {

class FastJsonWriter {
 public:
  static constexpr size_t kMaxRetainedCapacity = 16 * 1024 * 1024;

  FastJsonWriter();

  void Reset();
  void Trim() noexcept;
  size_t Capacity() const noexcept { return buffer_.capacity(); }

  void StartObject();
  void EndObject();
  void StartArray();
  void EndArray();
  void Key(std::string_view key);
  void String(std::string_view val);
  void RawJson(std::string_view raw);
  void Int64(int64_t val);
  void Uint64(uint64_t val);
  void Double(double val);
  void Bool(bool val);
  void Null();
  std::string_view Str() const;

 private:
  enum class ContainerType : uint8_t {
    kObject,
    kArray,
  };

  struct ContainerState {
    ContainerType type;
    bool empty;
  };

  void BeforeValue();
  void WriteEscapedString(std::string_view sv);

  std::string buffer_;
  std::vector<ContainerState> stack_;
};

}  // namespace vector_otel::core
