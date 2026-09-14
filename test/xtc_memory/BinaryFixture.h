#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace binary_fixture {
inline void put(std::vector<uint8_t>& bytes, size_t offset, uint64_t value, size_t width) {
  for (size_t i = 0; i < width; ++i) bytes.at(offset + i) = static_cast<uint8_t>(value >> (i * 8));
}

template <typename T>
void append(std::vector<uint8_t>& bytes, const T& value) {
  const size_t offset = bytes.size();
  bytes.resize(offset + sizeof(T));
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}
}  // namespace binary_fixture
