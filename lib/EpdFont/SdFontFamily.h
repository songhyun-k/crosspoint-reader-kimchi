#pragma once

#include "EpdFontFamily.h"
#include "SdFont.h"

/**
 * SD Card font family - similar interface to EpdFontFamily but uses SdFont.
 * Supports regular, bold, italic, and bold-italic variants.
 */
class SdFontFamily {
 private:
  SdFont* regular;
  SdFont* bold;
  SdFont* italic;
  SdFont* boldItalic;
  bool ownsPointers;

  SdFont* getFont(EpdFontStyle style) const;

 public:
  explicit SdFontFamily(SdFont* regular, SdFont* bold = nullptr, SdFont* italic = nullptr,
                        SdFont* boldItalic = nullptr)
      : regular(regular), bold(bold), italic(italic), boldItalic(boldItalic), ownsPointers(false) {}

  explicit SdFontFamily(const char* regularPath, const char* boldPath = nullptr, const char* italicPath = nullptr,
                        const char* boldItalicPath = nullptr);

  ~SdFontFamily();

  SdFontFamily(const SdFontFamily&) = delete;
  SdFontFamily& operator=(const SdFontFamily&) = delete;

  SdFontFamily(SdFontFamily&& other) noexcept;
  SdFontFamily& operator=(SdFontFamily&& other) noexcept;

  bool load();
  bool isLoaded() const;

  void getTextDimensions(const char* string, int* w, int* h, EpdFontStyle style = EpdFontFamily::REGULAR) const;
  bool hasPrintableChars(const char* string, EpdFontStyle style = EpdFontFamily::REGULAR) const;
  const EpdGlyph* getGlyph(uint32_t cp, EpdFontStyle style = EpdFontFamily::REGULAR) const;
  const uint8_t* getGlyphBitmap(uint32_t cp, EpdFontStyle style = EpdFontFamily::REGULAR) const;

  uint8_t getAdvanceY(EpdFontStyle style = EpdFontFamily::REGULAR) const;
  int8_t getAscender(EpdFontStyle style = EpdFontFamily::REGULAR) const;
  int8_t getDescender(EpdFontStyle style = EpdFontFamily::REGULAR) const;
  bool is2Bit(EpdFontStyle style = EpdFontFamily::REGULAR) const;

  bool hasBold() const { return bold != nullptr; }
};
