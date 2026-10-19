#pragma once

#include <cstddef>
#include <vector>

namespace vector_otel::core {

inline constexpr size_t kMaxScratchVectorCapacity = 65536;

template <typename T>
void ResetScratchVector(std::vector<T>& v) noexcept {
  v.clear();
  if (v.capacity() > kMaxScratchVectorCapacity) {
    std::vector<T>().swap(v);
  }
}

}  // namespace vector_otel::core
