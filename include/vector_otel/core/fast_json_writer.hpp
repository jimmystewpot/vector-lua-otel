#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vector_otel {
namespace core {

class FastJsonWriter {
 public:
  FastJsonWriter() = default;

  void Reset();
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
  void AppendEscapedString(std::string_view sv);

  std::vector<char> buffer_;
  std::vector<ContainerState> stack_;
};

}  // namespace core
}  // namespace vector_otel
