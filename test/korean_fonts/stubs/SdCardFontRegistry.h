#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Storage discovery is not part of the point-size policy under test.
struct SdCardFontFamilyInfo {
  std::string name;
  std::vector<uint8_t> points;
  std::vector<uint8_t> availableSizes() const { return points; }
};
class SdCardFontRegistry {
 public:
  std::vector<SdCardFontFamilyInfo> families;
  const SdCardFontFamilyInfo* findFamily(const std::string& name) const {
    for (const auto& family : families) {
      if (family.name == name) return &family;
    }
    return nullptr;
  }
};
