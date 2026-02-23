#pragma once

#include "EpdFontFamily.h"
#include "FontDecompressor.h"
#include "SdFontFamily.h"

/**
 * Unified font family that can hold either EpdFontFamily (flash) or SdFontFamily (SD card).
 * For flash fonts, integrates with FontDecompressor for compressed bitmap access.
 * Callers do not need to know the font source (flash vs SD).
 */
class UnifiedFontFamily {
 public:
  enum class Type { FLASH, SD };

 private:
  Type type;
  const EpdFontFamily* flashFont;       // Non-owning pointer for flash fonts
  FontDecompressor* fontDecompressor;    // Non-owning pointer for flash font decompression
  SdFontFamily* sdFont;                 // Owned pointer for SD fonts

 public:
  // Construct from flash font with its decompressor
  UnifiedFontFamily(const EpdFontFamily* font, FontDecompressor* decompressor);

  // Construct from SD font family (takes ownership)
  explicit UnifiedFontFamily(SdFontFamily* font);

  ~UnifiedFontFamily();

  UnifiedFontFamily(const UnifiedFontFamily&) = delete;
  UnifiedFontFamily& operator=(const UnifiedFontFamily&) = delete;

  UnifiedFontFamily(UnifiedFontFamily&& other) noexcept;
  UnifiedFontFamily& operator=(UnifiedFontFamily&& other) noexcept;

  Type getType() const { return type; }
  bool isSdFont() const { return type == Type::SD; }

  // Unified interface
  void getTextDimensions(const char* string, int* w, int* h, EpdFontStyle style = REGULAR) const;
  bool hasPrintableChars(const char* string, EpdFontStyle style = REGULAR) const;
  const EpdGlyph* getGlyph(uint32_t cp, EpdFontStyle style = REGULAR) const;

  // Get glyph bitmap (handles decompression for flash, SD cache for SD)
  const uint8_t* getGlyphBitmap(uint32_t cp, EpdFontStyle style = REGULAR) const;

  // Metadata
  uint8_t getAdvanceY(EpdFontStyle style = REGULAR) const;
  int8_t getAscender(EpdFontStyle style = REGULAR) const;
  int8_t getDescender(EpdFontStyle style = REGULAR) const;
  bool is2Bit(EpdFontStyle style = REGULAR) const;

  // Flash font specific (returns nullptr for SD fonts)
  const EpdFontData* getFlashData(EpdFontStyle style = REGULAR) const;
  FontDecompressor* getDecompressor() const { return fontDecompressor; }

  // Check if bold variant is available (for synthetic bold decision)
  bool hasBold() const;
};
