#pragma once

#include <SdCardFontRegistry.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// Reader font size is stored as an actual point size (see CrossPointSettings::
// fontPointSize), not an abstract Small/Medium/Large slot. The selectable sizes
// therefore come from whichever family is active: the built-in set below, or the
// .cpfont files a user installed for an SD family.

// The built-in Noto Serif / Noto Sans families are compiled in at exactly these
// point sizes (see the global font objects in main.cpp).
inline constexpr uint8_t BUILTIN_READER_POINT_SIZES[] = {12, 14, 16, 18};
inline constexpr uint8_t KOPUB_READER_FAMILY = 3;  // 2 remains the legacy OpenDyslexic value.
inline constexpr uint8_t KOPUB_READER_POINT_SIZE = 14;

constexpr bool isBuiltinReaderFamily(const uint8_t family) {
  return family == 0 || family == 1 || family == KOPUB_READER_FAMILY;
}
constexpr uint8_t builtinReaderFamilyIndex(const uint8_t family) { return family < 2 ? family : 2; }
constexpr uint8_t builtinReaderFamilyAt(const uint8_t index) { return index < 2 ? index : KOPUB_READER_FAMILY; }

// Point sizes selectable for the active reader font, ascending: the SD family's
// installed sizes when `sdFamilyName` names one the registry knows, otherwise
// the built-in set. Never returns empty.
std::vector<uint8_t> readerFontPointSizes(const SdCardFontRegistry* registry, const char* sdFamilyName,
                                          uint8_t builtinFamily = 0);
uint8_t snapToBuiltinPointSize(uint8_t pt, uint8_t builtinFamily);

// Closest entry in `sizes` (ascending, `count` > 0) to `pt`; ties resolve to the
// smaller size. Takes a raw range rather than a vector because getReaderFontId()
// runs inside the page render loop and must not allocate.
uint8_t snapToNearestPointSize(const uint8_t* sizes, size_t count, uint8_t pt);

inline uint8_t snapToNearestPointSize(const std::vector<uint8_t>& sizes, const uint8_t pt) {
  return sizes.empty() ? pt : snapToNearestPointSize(sizes.data(), sizes.size(), pt);
}
