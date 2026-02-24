#include "UnifiedFontFamily.h"

UnifiedFontFamily::UnifiedFontFamily(const EpdFontFamily* font, FontDecompressor* decompressor)
    : type(Type::FLASH), flashFont(font), fontDecompressor(decompressor), sdFont(nullptr) {}

UnifiedFontFamily::UnifiedFontFamily(SdFontFamily* font)
    : type(Type::SD), flashFont(nullptr), fontDecompressor(nullptr), sdFont(font) {}

UnifiedFontFamily::~UnifiedFontFamily() {
  // flashFont and fontDecompressor are not owned
  delete sdFont;
}

UnifiedFontFamily::UnifiedFontFamily(UnifiedFontFamily&& other) noexcept
    : type(other.type), flashFont(other.flashFont), fontDecompressor(other.fontDecompressor), sdFont(other.sdFont) {
  other.flashFont = nullptr;
  other.fontDecompressor = nullptr;
  other.sdFont = nullptr;
}

UnifiedFontFamily& UnifiedFontFamily::operator=(UnifiedFontFamily&& other) noexcept {
  if (this != &other) {
    delete sdFont;

    type = other.type;
    flashFont = other.flashFont;
    fontDecompressor = other.fontDecompressor;
    sdFont = other.sdFont;

    other.flashFont = nullptr;
    other.fontDecompressor = nullptr;
    other.sdFont = nullptr;
  }
  return *this;
}

void UnifiedFontFamily::getTextDimensions(const char* string, int* w, int* h, EpdFontStyle style) const {
  if (type == Type::FLASH && flashFont) {
    flashFont->getTextDimensions(string, w, h, style);
  } else if (sdFont) {
    sdFont->getTextDimensions(string, w, h, style);
  } else {
    *w = 0;
    *h = 0;
  }
}

bool UnifiedFontFamily::hasPrintableChars(const char* string, EpdFontStyle style) const {
  if (type == Type::FLASH && flashFont) {
    return flashFont->hasPrintableChars(string, style);
  } else if (sdFont) {
    return sdFont->hasPrintableChars(string, style);
  }
  return false;
}

const EpdGlyph* UnifiedFontFamily::getGlyph(uint32_t cp, EpdFontStyle style) const {
  if (type == Type::FLASH && flashFont) {
    return flashFont->getGlyph(cp, style);
  } else if (sdFont) {
    return sdFont->getGlyph(cp, style);
  }
  return nullptr;
}

int8_t UnifiedFontFamily::getKerning(uint32_t leftCp, uint32_t rightCp, EpdFontStyle style) const {
  if (type == Type::FLASH && flashFont) {
    return flashFont->getKerning(leftCp, rightCp, style);
  }
  return 0;
}

uint32_t UnifiedFontFamily::applyLigatures(uint32_t cp, const char*& text, EpdFontStyle style) const {
  if (type == Type::FLASH && flashFont) {
    return flashFont->applyLigatures(cp, text, style);
  }
  return cp;
}

const uint8_t* UnifiedFontFamily::getGlyphBitmap(uint32_t cp, EpdFontStyle style) const {
  if (type == Type::FLASH && flashFont) {
    const EpdFontData* data = flashFont->getData(style);
    const EpdGlyph* glyph = flashFont->getGlyph(cp, style);
    if (!data || !glyph) {
      return nullptr;
    }
    if (fontDecompressor && data->groups && data->groupCount > 0) {
      // Compressed flash font: use FontDecompressor
      // Find glyph index for decompressor
      for (uint32_t i = 0; i < data->intervalCount; i++) {
        if (cp >= data->intervals[i].first && cp <= data->intervals[i].last) {
          uint16_t glyphIndex = static_cast<uint16_t>(data->intervals[i].offset + (cp - data->intervals[i].first));
          return fontDecompressor->getBitmap(data, glyph, glyphIndex);
        }
      }
      return nullptr;
    }
    // Uncompressed flash font: direct bitmap access
    return &data->bitmap[glyph->dataOffset];
  } else if (sdFont) {
    return sdFont->getGlyphBitmap(cp, style);
  }
  return nullptr;
}

uint8_t UnifiedFontFamily::getAdvanceY(EpdFontStyle style) const {
  if (type == Type::FLASH && flashFont) {
    const EpdFontData* data = flashFont->getData(style);
    return data ? data->advanceY : 0;
  } else if (sdFont) {
    return sdFont->getAdvanceY(style);
  }
  return 0;
}

int8_t UnifiedFontFamily::getAscender(EpdFontStyle style) const {
  if (type == Type::FLASH && flashFont) {
    const EpdFontData* data = flashFont->getData(style);
    return data ? data->ascender : 0;
  } else if (sdFont) {
    return sdFont->getAscender(style);
  }
  return 0;
}

int8_t UnifiedFontFamily::getDescender(EpdFontStyle style) const {
  if (type == Type::FLASH && flashFont) {
    const EpdFontData* data = flashFont->getData(style);
    return data ? data->descender : 0;
  } else if (sdFont) {
    return sdFont->getDescender(style);
  }
  return 0;
}

bool UnifiedFontFamily::is2Bit(EpdFontStyle style) const {
  if (type == Type::FLASH && flashFont) {
    const EpdFontData* data = flashFont->getData(style);
    return data ? data->is2Bit : false;
  } else if (sdFont) {
    return sdFont->is2Bit(style);
  }
  return false;
}

const EpdFontData* UnifiedFontFamily::getFlashData(EpdFontStyle style) const {
  if (type == Type::FLASH && flashFont) {
    return flashFont->getData(style);
  }
  return nullptr;
}

bool UnifiedFontFamily::hasBold() const {
  if (type == Type::FLASH && flashFont) {
    return flashFont->hasBold();
  } else if (sdFont) {
    return sdFont->hasBold();
  }
  return false;
}
