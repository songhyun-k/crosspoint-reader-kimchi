#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace kimchi_release {

inline constexpr char REPOSITORY[] = "songhyun-k/crosspoint-reader-kimchi";
inline constexpr char LATEST_URL[] = "https://api.github.com/repos/songhyun-k/crosspoint-reader-kimchi/releases/latest";
inline constexpr char DOWNLOAD_PREFIX[] = "https://github.com/songhyun-k/crosspoint-reader-kimchi/releases/download/";

// Only published kimchi versions are accepted: [v]M.m.p-kimchi.N (N >= 1).
// Board/dev display suffixes never participate in this comparison.
struct Version {
  std::array<uint32_t, 4> numbers{};
};

bool parseVersion(std::string_view text, Version& result);
bool isNewer(std::string_view candidate, std::string_view current);
bool assetName(std::string_view board, char* output, size_t capacity);
bool matchesAssetUrl(std::string_view url, std::string_view tag, std::string_view asset);

}  // namespace kimchi_release
