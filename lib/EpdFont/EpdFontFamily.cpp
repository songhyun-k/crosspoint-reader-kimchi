#include "EpdFontFamily.h"

const EpdFont* EpdFontFamily::getFont(const Style style) const {
  const EpdFont* fonts[] = {regular, bold, italic, boldItalic};
  const uint8_t mask = (regular ? 1 : 0) | (bold ? 2 : 0) | (italic ? 4 : 0) | (boldItalic ? 8 : 0);
  return fonts[resolveStyle(static_cast<uint8_t>(style), mask)];
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
