#include "SdFontFamily.h"

#include <Logging.h>

#include <new>

// ============================================================================
// SdFontFamily Implementation
// ============================================================================

SdFontFamily::SdFontFamily(const char* regularPath, const char* boldPath, const char* italicPath,
                           const char* boldItalicPath)
    : regular(nullptr), bold(nullptr), italic(nullptr), boldItalic(nullptr), ownsPointers(true) {
  if (regularPath) {
    regular = new (std::nothrow) SdFont(regularPath);
  }
  if (boldPath) {
    bold = new (std::nothrow) SdFont(boldPath);
  }
  if (italicPath) {
    italic = new (std::nothrow) SdFont(italicPath);
  }
  if (boldItalicPath) {
    boldItalic = new (std::nothrow) SdFont(boldItalicPath);
  }
}

SdFontFamily::~SdFontFamily() {
  if (ownsPointers) {
    delete regular;
    delete bold;
    delete italic;
    delete boldItalic;
  }
}

SdFontFamily::SdFontFamily(SdFontFamily&& other) noexcept
    : regular(other.regular),
      bold(other.bold),
      italic(other.italic),
      boldItalic(other.boldItalic),
      ownsPointers(other.ownsPointers) {
  other.regular = nullptr;
  other.bold = nullptr;
  other.italic = nullptr;
  other.boldItalic = nullptr;
  other.ownsPointers = false;
}

SdFontFamily& SdFontFamily::operator=(SdFontFamily&& other) noexcept {
  if (this != &other) {
    if (ownsPointers) {
      delete regular;
      delete bold;
      delete italic;
      delete boldItalic;
    }

    regular = other.regular;
    bold = other.bold;
    italic = other.italic;
    boldItalic = other.boldItalic;
    ownsPointers = other.ownsPointers;

    other.regular = nullptr;
    other.bold = nullptr;
    other.italic = nullptr;
    other.boldItalic = nullptr;
    other.ownsPointers = false;
  }
  return *this;
}

bool SdFontFamily::load() {
  bool success = true;

  if (regular && !regular->load()) {
    LOG_ERR("SDF", "Failed to load regular font");
    success = false;
  }
  if (bold && !bold->load()) {
    LOG_DBG("SDF", "Failed to load bold font (optional)");
  }
  if (italic && !italic->load()) {
    LOG_DBG("SDF", "Failed to load italic font (optional)");
  }
  if (boldItalic && !boldItalic->load()) {
    LOG_DBG("SDF", "Failed to load bold-italic font (optional)");
  }

  return success;
}

bool SdFontFamily::isLoaded() const { return regular && regular->isLoaded(); }

SdFont* SdFontFamily::getFont(EpdFontStyle style) const {
  if (style == EpdFontFamily::BOLD && bold && bold->isLoaded()) {
    return bold;
  }
  if (style == EpdFontFamily::ITALIC && italic && italic->isLoaded()) {
    return italic;
  }
  if (style == EpdFontFamily::BOLD_ITALIC) {
    if (boldItalic && boldItalic->isLoaded()) {
      return boldItalic;
    }
    if (bold && bold->isLoaded()) {
      return bold;
    }
    if (italic && italic->isLoaded()) {
      return italic;
    }
  }

  return regular;
}

void SdFontFamily::getTextDimensions(const char* string, int* w, int* h, EpdFontStyle style) const {
  SdFont* font = getFont(style);
  if (font) {
    font->getTextDimensions(string, w, h);
  } else {
    *w = 0;
    *h = 0;
  }
}

bool SdFontFamily::hasPrintableChars(const char* string, EpdFontStyle style) const {
  SdFont* font = getFont(style);
  return font ? font->hasPrintableChars(string) : false;
}

const EpdGlyph* SdFontFamily::getGlyph(uint32_t cp, EpdFontStyle style) const {
  SdFont* font = getFont(style);
  return font ? font->getGlyph(cp) : nullptr;
}

const uint8_t* SdFontFamily::getGlyphBitmap(uint32_t cp, EpdFontStyle style) const {
  SdFont* font = getFont(style);
  return font ? font->getGlyphBitmap(cp) : nullptr;
}

uint8_t SdFontFamily::getAdvanceY(EpdFontStyle style) const {
  SdFont* font = getFont(style);
  return font ? font->getAdvanceY() : 0;
}

int8_t SdFontFamily::getAscender(EpdFontStyle style) const {
  SdFont* font = getFont(style);
  return font ? font->getAscender() : 0;
}

int8_t SdFontFamily::getDescender(EpdFontStyle style) const {
  SdFont* font = getFont(style);
  return font ? font->getDescender() : 0;
}

bool SdFontFamily::is2Bit(EpdFontStyle style) const {
  SdFont* font = getFont(style);
  return font ? font->is2Bit() : false;
}
