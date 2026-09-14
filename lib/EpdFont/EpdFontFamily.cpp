#include "EpdFontFamily.h"

const EpdFont* EpdFontFamily::getFont(const Style style) const {
  // Extract font style bits; render-time overlay bits do not affect font selection.
  const bool hasBold = (style & BOLD) != 0;
  const bool hasItalic = (style & ITALIC) != 0;

  if (hasBold && hasItalic) {
    if (boldItalic) return boldItalic;
    if (bold) return bold;
    if (italic) return italic;
  } else if (hasBold && bold) {
    return bold;
  } else if (hasItalic && italic) {
    return italic;
  }

  // Sparse SD families must agree with SdCardFont's advance-table fallback.
  if (!regular) {
    if ((hasBold || hasItalic) && boldItalic) return boldItalic;
    if (bold) return bold;
    return italic ? italic : boldItalic;
  }
  return regular;
}

bool EpdFontFamily::needsSyntheticBold(const Style style) const {
  if (!(style & BOLD)) return false;
  const auto* resolved = getFont(style);
  if (!resolved) return false;
  const bool isBoldFace = resolved == bold || resolved == boldItalic;
  const bool aliasesNormal = (regular && resolved->data == regular->data) || (italic && resolved->data == italic->data);
  return !isBoldFace || aliasesNormal;
}

void EpdFontFamily::getTextDimensions(const char* string, int* w, int* h, const Style style) const {
  getFont(style)->getTextDimensions(string, w, h, needsSyntheticBold(style), (style & (SUP | SUB)) != 0);
}

const EpdFontData* EpdFontFamily::getData(const Style style) const { return getFont(style)->data; }

const EpdGlyph* EpdFontFamily::getGlyph(const uint32_t cp, const Style style) const {
  return getFont(style)->getGlyph(cp);
}

bool EpdFontFamily::hasCodepoint(const uint32_t cp, const Style style) const {
  return getFont(style)->hasCodepoint(cp);
}

int8_t EpdFontFamily::getKerning(const uint32_t leftCp, const uint32_t rightCp, const Style style) const {
  return getFont(style)->getKerning(leftCp, rightCp);
}

uint32_t EpdFontFamily::applyLigatures(const uint32_t cp, const char*& text, const Style style) const {
  return getFont(style)->applyLigatures(cp, text);
}
