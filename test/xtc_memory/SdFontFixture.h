#pragma once

#include <SdCardFont.h>

#include "BinaryFixture.h"

namespace sd_font_fixture {
// More than the bounded advance table, with visible bitmap payloads so a
// measurement accidentally loading pixels is observable through the real HAL.
inline std::vector<uint8_t> makeWideCoverageFont() {
  using binary_fixture::append;
  using binary_fixture::put;
  constexpr uint32_t count = 1026;  // space, 1024 Hangul syllables, replacement
  std::vector<uint8_t> bytes(64);
  bytes.reserve(64 + 36 + count * 48);
  std::memcpy(bytes.data(), "CPFONT\0\0", 8);
  put(bytes, 8, CPFONT_VERSION, 2);
  bytes[12] = 1;
  put(bytes, 36, 3, 4);
  put(bytes, 40, count, 4);
  bytes[44] = 20;
  put(bytes, 45, 16, 2);
  put(bytes, 47, static_cast<uint16_t>(-4), 2);
  put(bytes, 56, 64, 4);
  append(bytes, EpdUnicodeInterval{' ', ' ', 0});
  append(bytes, EpdUnicodeInterval{0xAC00, 0xAFFF, 1});
  append(bytes, EpdUnicodeInterval{0xFFFD, 0xFFFD, count - 1});
  for (uint32_t i = 0; i < count; ++i) {
    EpdGlyph glyph{};
    glyph.width = glyph.height = glyph.top = 16;
    glyph.advanceX = (i == 0 ? 8 : i == count - 1 ? 12 : 16) * 16;
    glyph.dataLength = 32;
    glyph.dataOffset = i * 32;
    append(bytes, glyph);
  }
  bytes.insert(bytes.end(), count * 32, 0xFF);
  return bytes;
}

inline std::string hangulRange(uint32_t first, uint32_t count) {
  std::string text;
  text.reserve(count * 3);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t cp = 0xAC00 + first + i;
    text += static_cast<char>(0xE0 | (cp >> 12));
    text += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    text += static_cast<char>(0x80 | (cp & 0x3F));
  }
  return text;
}
}  // namespace sd_font_fixture
