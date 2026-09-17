#pragma once
#include "EpdFontData.h"

class EpdFont {
  const EpdFontData* immutableData = nullptr;
  const EpdUnicodeInterval* hangulInterval = nullptr;
  const EpdUnicodeInterval* spaceInterval = nullptr;
  void initImmutableIntervals();
  const EpdGlyph* getGlyphFallback(uint32_t cp) const;
  void getTextBounds(const char* string, int startX, int startY, int* minX, int* minY, int* maxX, int* maxY,
                     bool syntheticBold, bool halfSize) const;

 public:
  const EpdFontData* data;
  // Opt in only for immutable metadata and arrays whose lifetime covers this font.
  explicit EpdFont(const EpdFontData* data, bool immutable = false) : data(data) {
    if (immutable) initImmutableIntervals();
  }
  ~EpdFont() = default;
  void getTextDimensions(const char* string, int* w, int* h, bool syntheticBold = false, bool halfSize = false) const;

  // Emboldening is one output pixel, including at SUP/SUB scale. Zero-advance
  // marks stay overlays; all advancing glyphs use the same spacing contract.
  static constexpr int32_t advanceForRender(int32_t advance, bool syntheticBold, bool halfSize = false) {
    if (halfSize) advance = (advance + 1) / 2;
    return syntheticBold && advance > 0 ? advance + fp4::fromPixel(1) : advance;
  }

  const EpdGlyph* getGlyph(uint32_t cp) const {
    // Public data replacement disables the cached intervals without touching them.
    if (immutableData && data == immutableData) {
      // Initialization guarantees each interval contains its probe codepoint.
      if (cp == 0x20 && spaceInterval) {
        return &data->glyph[spaceInterval->offset + (cp - spaceInterval->first)];
      }
      if (cp >= 0xAC00 && hangulInterval && cp <= hangulInterval->last) {
        return &data->glyph[hangulInterval->offset + (cp - hangulInterval->first)];
      }
    }
    return getGlyphFallback(cp);
  }

  /// Returns true if this font covers `cp`: either via its in-RAM interval
  /// table or, for SD card fonts, via the coverageHandler that consults the
  /// full RAM-resident coverage index. Unlike getGlyph(), it never performs
  /// storage I/O and never falls back to the replacement glyph — it reports
  /// only what this font can render. Used by the CJK UI font fallback to
  /// decide whether a string needs to be routed to another font.
  bool hasCodepoint(uint32_t cp) const;

  /// Returns the kerning adjustment (4.4 fixed-point in pixels) between two codepoints.
  /// Returns 0 if no kerning data exists for the pair.
  int8_t getKerning(uint32_t leftCp, uint32_t rightCp) const;

  /// Returns the ligature codepoint for a pair, or 0 if no ligature exists.
  uint32_t getLigature(uint32_t leftCp, uint32_t rightCp) const;

  /// Greedily applies ligature substitutions starting from cp, consuming
  /// as many following codepoints from text as possible. Returns the
  /// (possibly substituted) codepoint; advances text past consumed chars.
  uint32_t applyLigatures(uint32_t cp, const char*& text) const;
};
