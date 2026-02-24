#pragma once

#include <SdFat.h>

#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>

#include "EpdFontData.h"
#include "SdFontFormat.h"

/**
 * LRU Cache for glyph bitmap data loaded from SD card.
 * Automatically evicts least recently used entries when memory limit is reached.
 */
class GlyphBitmapCache {
 public:
  struct CacheEntry {
    uint64_t key;
    uint8_t* bitmap;
    uint32_t size;
  };

 private:
  size_t maxCacheSize;
  size_t currentSize;
  std::list<CacheEntry> cacheList;  // Most recent at front
  std::unordered_map<uint64_t, std::list<CacheEntry>::iterator> cacheMap;

  void evictOldest();

 public:
  explicit GlyphBitmapCache(size_t maxSize = 32768);  // 32KB default
  ~GlyphBitmapCache();

  const uint8_t* get(uint64_t key);
  const uint8_t* put(uint64_t key, const uint8_t* data, uint32_t size);

  void clear();
  size_t getUsedSize() const { return currentSize; }
  size_t getMaxSize() const { return maxCacheSize; }
};

/**
 * Simple fixed-size cache for glyph metadata (EpdGlyph) loaded on-demand.
 * Uses a circular buffer to avoid STL container overhead on ESP32.
 */
class GlyphMetadataCache {
 public:
  static constexpr size_t MAX_ENTRIES = 128;

  struct CacheEntry {
    uint32_t codepoint;
    EpdGlyph glyph;
    bool valid;
  };

 private:
  CacheEntry entries[MAX_ENTRIES];
  size_t nextSlot;

 public:
  GlyphMetadataCache() : nextSlot(0) {
    for (size_t i = 0; i < MAX_ENTRIES; i++) {
      entries[i].valid = false;
    }
  }

  const EpdGlyph* get(uint32_t codepoint);
  const EpdGlyph* put(uint32_t codepoint, const EpdGlyph& glyph);
  void clear();
};

/**
 * SD Card font data structure.
 * Mimics EpdFontData interface but loads data on-demand from SD card.
 */
class SdFontData {
 private:
  std::string filePath;
  bool loaded;

  // Font metadata (loaded once, kept in RAM)
  EpdFontHeader header;
  EpdFontInterval* intervals;  // Dynamically allocated

  // Glyph metadata cache (per-font, small circular buffer)
  mutable GlyphMetadataCache glyphCache;

  // Bitmap cache (shared across all SdFontData instances)
  static GlyphBitmapCache* sharedCache;
  static int cacheRefCount;
  static uint8_t nextFontTag;
  uint8_t fontTag;

  // File handle for reading (opened on demand)
  mutable FsFile fontFile;

  int findGlyphIndex(uint32_t codepoint) const;
  bool loadGlyphFromSD(int glyphIndex, EpdGlyph* outGlyph) const;
  bool ensureFileOpen() const;

 public:
  explicit SdFontData(const char* path);
  ~SdFontData();

  SdFontData(const SdFontData&) = delete;
  SdFontData& operator=(const SdFontData&) = delete;

  SdFontData(SdFontData&& other) noexcept;
  SdFontData& operator=(SdFontData&& other) noexcept;

  bool load();
  bool isLoaded() const { return loaded; }

  uint8_t getAdvanceY() const { return header.advanceY; }
  int8_t getAscender() const { return header.ascender; }
  int8_t getDescender() const { return header.descender; }
  bool is2Bit() const { return header.is2Bit != 0; }
  uint32_t getIntervalCount() const { return header.intervalCount; }
  uint32_t getGlyphCount() const { return header.glyphCount; }

  const EpdGlyph* getGlyph(uint32_t codepoint) const;
  const uint8_t* getGlyphBitmap(uint32_t codepoint) const;

  static void setCacheSize(size_t maxBytes);
  static void clearCache();
  static size_t getCacheUsedSize();
  static uint64_t makeCacheKey(uint8_t tag, uint32_t cp) { return (static_cast<uint64_t>(tag) << 32) | cp; }
};

/**
 * SD Card font class - similar interface to EpdFont but loads from SD card.
 */
class SdFont {
 private:
  SdFontData* data;
  bool ownsData;

 public:
  explicit SdFont(SdFontData* fontData, bool takeOwnership = false);
  explicit SdFont(const char* filePath);
  ~SdFont();

  SdFont(const SdFont&) = delete;
  SdFont& operator=(const SdFont&) = delete;

  SdFont(SdFont&& other) noexcept;
  SdFont& operator=(SdFont&& other) noexcept;

  bool load();
  bool isLoaded() const { return data && data->isLoaded(); }

  void getTextDimensions(const char* string, int* w, int* h) const;
  bool hasPrintableChars(const char* string) const;
  const EpdGlyph* getGlyph(uint32_t cp) const;
  const uint8_t* getGlyphBitmap(uint32_t cp) const;

  uint8_t getAdvanceY() const { return data ? data->getAdvanceY() : 0; }
  int8_t getAscender() const { return data ? data->getAscender() : 0; }
  int8_t getDescender() const { return data ? data->getDescender() : 0; }
  bool is2Bit() const { return data ? data->is2Bit() : false; }

  SdFontData* getData() const { return data; }
};
