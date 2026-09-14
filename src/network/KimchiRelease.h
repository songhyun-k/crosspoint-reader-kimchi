#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace kimchi_release {

inline constexpr char LATEST_URL[] = "https://api.github.com/repos/songhyun-k/crosspoint-reader-kimchi/releases/latest";

// Release tag format: [v]M.m.p-kimchi.N.
// Board/dev display suffixes never participate in this comparison.
struct Version {
  std::array<uint32_t, 4> numbers{};
};

bool parseVersion(std::string_view text, Version& result);

}  // namespace kimchi_release
