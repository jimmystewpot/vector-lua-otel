#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace vector_otel::core {

inline constexpr size_t kMaxScratchVectorCapacity = 65536;
inline constexpr size_t kMaxScratchStringCapacity = 65536;

template <typename T>
void ResetScratchVector(std::vector<T>& v) noexcept {
  v.clear();
  if (v.capacity() > kMaxScratchVectorCapacity) {
    std::vector<T>().swap(v);
  }
}

inline void ResetScratchString(std::string& s) noexcept {
  s.clear();
  if (s.capacity() > kMaxScratchStringCapacity) {
    std::string().swap(s);
  }
}

}  // namespace vector_otel::core
