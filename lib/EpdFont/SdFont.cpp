#include "SdFont.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>
#include <new>

// ============================================================================
// GlyphBitmapCache Implementation
// ============================================================================

GlyphBitmapCache::GlyphBitmapCache(size_t maxSize) : maxCacheSize(maxSize), currentSize(0) {}

GlyphBitmapCache::~GlyphBitmapCache() { clear(); }

void GlyphBitmapCache::evictOldest() {
  while (currentSize > maxCacheSize && !cacheList.empty()) {
    auto& oldest = cacheList.back();
    currentSize -= oldest.size;
    cacheMap.erase(oldest.key);
    free(oldest.bitmap);
    cacheList.pop_back();
  }
}

const uint8_t* GlyphBitmapCache::get(uint64_t key) {
  auto it = cacheMap.find(key);
  if (it == cacheMap.end()) {
    return nullptr;
  }

  // Move to front (most recently used)
  if (it->second != cacheList.begin()) {
    cacheList.splice(cacheList.begin(), cacheList, it->second);
  }

  return it->second->bitmap;
}

const uint8_t* GlyphBitmapCache::put(uint64_t key, const uint8_t* data, uint32_t size) {
  auto it = cacheMap.find(key);
  if (it != cacheMap.end()) {
    if (it->second != cacheList.begin()) {
      cacheList.splice(cacheList.begin(), cacheList, it->second);
    }
    return it->second->bitmap;
  }

  uint8_t* bitmapCopy = static_cast<uint8_t*>(malloc(size));
  if (!bitmapCopy) {
    LOG_ERR("SDF", "Failed to allocate %u bytes for glyph cache", size);
    return nullptr;
  }
  memcpy(bitmapCopy, data, size);

  CacheEntry entry = {key, bitmapCopy, size};
  cacheList.push_front(entry);
  cacheMap[key] = cacheList.begin();
  currentSize += size;

  evictOldest();

  return bitmapCopy;
}

void GlyphBitmapCache::clear() {
  for (auto& entry : cacheList) {
    free(entry.bitmap);
  }
  cacheList.clear();
  cacheMap.clear();
  currentSize = 0;
}

// ============================================================================
// GlyphMetadataCache Implementation (simple fixed-size circular buffer)
// ============================================================================

const EpdGlyph* GlyphMetadataCache::get(uint32_t codepoint) {
  for (size_t i = 0; i < MAX_ENTRIES; i++) {
    if (entries[i].valid && entries[i].codepoint == codepoint) {
      return &entries[i].glyph;
    }
  }
  return nullptr;
}

const EpdGlyph* GlyphMetadataCache::put(uint32_t codepoint, const EpdGlyph& glyph) {
  for (size_t i = 0; i < MAX_ENTRIES; i++) {
    if (entries[i].valid && entries[i].codepoint == codepoint) {
      return &entries[i].glyph;
    }
  }

  entries[nextSlot].codepoint = codepoint;
  entries[nextSlot].glyph = glyph;
  entries[nextSlot].valid = true;

  const EpdGlyph* result = &entries[nextSlot].glyph;
  nextSlot = (nextSlot + 1) % MAX_ENTRIES;
  return result;
}

void GlyphMetadataCache::clear() {
  for (size_t i = 0; i < MAX_ENTRIES; i++) {
    entries[i].valid = false;
  }
  nextSlot = 0;
}

// ============================================================================
// SdFontData Implementation
// ============================================================================

GlyphBitmapCache* SdFontData::sharedCache = nullptr;
int SdFontData::cacheRefCount = 0;
uint8_t SdFontData::nextFontTag = 0;

static constexpr uint32_t MAX_INTERVAL_COUNT = 10000;
static constexpr uint32_t MAX_GLYPH_COUNT = 150000;
static constexpr size_t MIN_FREE_HEAP_AFTER_LOAD = 16384;

SdFontData::SdFontData(const char* path) : filePath(path), loaded(false), intervals(nullptr) {
  memset(&header, 0, sizeof(header));
  fontTag = nextFontTag++;

  if (sharedCache == nullptr) {
    sharedCache = new GlyphBitmapCache(32768);  // 32KB cache
  }
  cacheRefCount++;
}

SdFontData::~SdFontData() {
  if (fontFile) {
    fontFile.close();
  }

  delete[] intervals;

  cacheRefCount--;
  if (cacheRefCount == 0 && sharedCache != nullptr) {
    delete sharedCache;
    sharedCache = nullptr;
  }
}

SdFontData::SdFontData(SdFontData&& other) noexcept
    : filePath(std::move(other.filePath)), loaded(other.loaded), header(other.header), intervals(other.intervals) {
  fontTag = other.fontTag;
  other.intervals = nullptr;
  other.loaded = false;
  cacheRefCount++;
}

SdFontData& SdFontData::operator=(SdFontData&& other) noexcept {
  if (this != &other) {
    if (fontFile) {
      fontFile.close();
    }
    delete[] intervals;

    filePath = std::move(other.filePath);
    loaded = other.loaded;
    header = other.header;
    intervals = other.intervals;
    fontTag = other.fontTag;

    other.intervals = nullptr;
    other.loaded = false;
  }
  return *this;
}

bool SdFontData::load() {
  if (loaded) {
    return true;
  }

  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_FREE_HEAP_AFTER_LOAD) {
    LOG_ERR("SDF", "Insufficient heap: %u bytes (need %u)", freeHeap, MIN_FREE_HEAP_AFTER_LOAD);
    return false;
  }

  if (!Storage.openFileForRead("SdFont", filePath.c_str(), fontFile)) {
    LOG_ERR("SDF", "Failed to open font file: %s", filePath.c_str());
    return false;
  }

  if (fontFile.read(&header, sizeof(EpdFontHeader)) != sizeof(EpdFontHeader)) {
    LOG_ERR("SDF", "Failed to read header from: %s", filePath.c_str());
    fontFile.close();
    return false;
  }

  if (header.magic != EPDFONT_MAGIC) {
    LOG_ERR("SDF", "Invalid magic: 0x%08X (expected 0x%08X)", header.magic, EPDFONT_MAGIC);
    fontFile.close();
    return false;
  }

  if (header.version != EPDFONT_VERSION) {
    LOG_ERR("SDF", "Bad version: %u (expected %u)", header.version, EPDFONT_VERSION);
    fontFile.close();
    return false;
  }

  if (header.intervalCount > MAX_INTERVAL_COUNT) {
    LOG_ERR("SDF", "Too many intervals: %u (max %u)", header.intervalCount, MAX_INTERVAL_COUNT);
    fontFile.close();
    return false;
  }

  if (header.glyphCount > MAX_GLYPH_COUNT) {
    LOG_ERR("SDF", "Too many glyphs: %u (max %u)", header.glyphCount, MAX_GLYPH_COUNT);
    fontFile.close();
    return false;
  }

  size_t intervalsMemory = header.intervalCount * sizeof(EpdFontInterval);

  if (intervalsMemory > freeHeap - MIN_FREE_HEAP_AFTER_LOAD) {
    LOG_ERR("SDF", "Not enough memory for intervals: need %u, have %u", intervalsMemory, freeHeap);
    fontFile.close();
    return false;
  }

  LOG_INF("SDF", "Loading %s: %u intervals, %u glyphs (on-demand)", filePath.c_str(), header.intervalCount,
          header.glyphCount);

  intervals = new (std::nothrow) EpdFontInterval[header.intervalCount];
  if (intervals == nullptr) {
    LOG_ERR("SDF", "Failed to allocate intervals (%u bytes)", intervalsMemory);
    fontFile.close();
    return false;
  }

  if (header.intervalsOffset != sizeof(EpdFontHeader)) {
    if (!fontFile.seekSet(header.intervalsOffset)) {
      LOG_ERR("SDF", "Failed to seek to intervals at %u", header.intervalsOffset);
      fontFile.close();
      delete[] intervals;
      intervals = nullptr;
      return false;
    }
  }

  if (fontFile.read(intervals, intervalsMemory) != static_cast<int>(intervalsMemory)) {
    LOG_ERR("SDF", "Failed to read intervals");
    fontFile.close();
    delete[] intervals;
    intervals = nullptr;
    return false;
  }

  fontFile.close();

  loaded = true;
  LOG_INF("SDF", "Loaded: %s (advanceY=%u, intervals=%uKB)", filePath.c_str(), header.advanceY, intervalsMemory / 1024);

  return true;
}

bool SdFontData::ensureFileOpen() const {
  if (fontFile && fontFile.isOpen()) {
    return true;
  }
  return Storage.openFileForRead("SdFont", filePath.c_str(), fontFile);
}

bool SdFontData::loadGlyphFromSD(int glyphIndex, EpdGlyph* outGlyph) const {
  if (!loaded || glyphIndex < 0 || glyphIndex >= static_cast<int>(header.glyphCount)) {
    return false;
  }

  if (!ensureFileOpen()) {
    return false;
  }

  uint32_t glyphFileOffset = header.glyphsOffset + (glyphIndex * sizeof(EpdFontGlyph));

  if (!fontFile.seekSet(glyphFileOffset)) {
    return false;
  }

  EpdFontGlyph fileGlyph;
  if (fontFile.read(&fileGlyph, sizeof(EpdFontGlyph)) != sizeof(EpdFontGlyph)) {
    return false;
  }

  outGlyph->width = fileGlyph.width;
  outGlyph->height = fileGlyph.height;
  outGlyph->advanceX = fileGlyph.advanceX;
  outGlyph->left = fileGlyph.left;
  outGlyph->top = fileGlyph.top;
  outGlyph->dataLength = static_cast<uint16_t>(fileGlyph.dataLength);
  outGlyph->dataOffset = fileGlyph.dataOffset;

  return true;
}

int SdFontData::findGlyphIndex(uint32_t codepoint) const {
  if (!loaded || intervals == nullptr) {
    return -1;
  }

  int left = 0;
  int right = static_cast<int>(header.intervalCount) - 1;

  while (left <= right) {
    int mid = left + (right - left) / 2;
    const EpdFontInterval* interval = &intervals[mid];

    if (codepoint < interval->first) {
      right = mid - 1;
    } else if (codepoint > interval->last) {
      left = mid + 1;
    } else {
      return static_cast<int>(interval->offset + (codepoint - interval->first));
    }
  }

  return -1;
}

const EpdGlyph* SdFontData::getGlyph(uint32_t codepoint) const {
  if (!loaded) {
    return nullptr;
  }

  const EpdGlyph* cached = glyphCache.get(codepoint);
  if (cached != nullptr) {
    return cached;
  }

  int index = findGlyphIndex(codepoint);
  if (index < 0 || index >= static_cast<int>(header.glyphCount)) {
    return nullptr;
  }

  EpdGlyph glyph;
  if (!loadGlyphFromSD(index, &glyph)) {
    return nullptr;
  }

  return glyphCache.put(codepoint, glyph);
}

const uint8_t* SdFontData::getGlyphBitmap(uint32_t codepoint) const {
  if (!loaded || sharedCache == nullptr) {
    return nullptr;
  }

  // Check bitmap cache first (fast path)
  const uint64_t key = makeCacheKey(fontTag, codepoint);
  const uint8_t* cached = sharedCache->get(key);
  if (cached != nullptr) {
    return cached;
  }

  // Get glyph metadata (hits glyphCache or single SD read)
  const EpdGlyph* glyph = getGlyph(codepoint);
  if (!glyph || glyph->dataLength == 0) {
    return nullptr;
  }

  // Read only bitmap data from SD (metadata re-read eliminated)
  if (!ensureFileOpen()) {
    return nullptr;
  }

  if (!fontFile.seekSet(header.bitmapOffset + glyph->dataOffset)) {
    return nullptr;
  }

  uint8_t* tempBuffer = static_cast<uint8_t*>(malloc(glyph->dataLength));
  if (!tempBuffer) {
    return nullptr;
  }

  if (fontFile.read(tempBuffer, glyph->dataLength) != static_cast<int>(glyph->dataLength)) {
    free(tempBuffer);
    return nullptr;
  }

  const uint8_t* result = sharedCache->put(key, tempBuffer, glyph->dataLength);
  free(tempBuffer);

  return result;
}

void SdFontData::setCacheSize(size_t maxBytes) {
  if (sharedCache != nullptr) {
    delete sharedCache;
  }
  sharedCache = new GlyphBitmapCache(maxBytes);
}

void SdFontData::clearCache() {
  if (sharedCache != nullptr) {
    sharedCache->clear();
  }
}

size_t SdFontData::getCacheUsedSize() {
  if (sharedCache != nullptr) {
    return sharedCache->getUsedSize();
  }
  return 0;
}

// ============================================================================
// SdFont Implementation
// ============================================================================

SdFont::SdFont(SdFontData* fontData, bool takeOwnership) : data(fontData), ownsData(takeOwnership) {}

SdFont::SdFont(const char* filePath) : data(new (std::nothrow) SdFontData(filePath)), ownsData(true) {}

SdFont::~SdFont() {
  if (ownsData) {
    delete data;
  }
}

SdFont::SdFont(SdFont&& other) noexcept : data(other.data), ownsData(other.ownsData) {
  other.data = nullptr;
  other.ownsData = false;
}

SdFont& SdFont::operator=(SdFont&& other) noexcept {
  if (this != &other) {
    if (ownsData) {
      delete data;
    }
    data = other.data;
    ownsData = other.ownsData;
    other.data = nullptr;
    other.ownsData = false;
  }
  return *this;
}

bool SdFont::load() {
  if (data == nullptr) {
    return false;
  }
  return data->load();
}

void SdFont::getTextDimensions(const char* string, int* w, int* h) const {
  *w = 0;
  *h = 0;

  if (data == nullptr || !data->isLoaded() || string == nullptr || *string == '\0') {
    return;
  }

  int minX = 0, minY = 0, maxX = 0, maxY = 0;
  int cursorX = 0;
  const int cursorY = 0;

  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&string)))) {
    const EpdGlyph* glyph = data->getGlyph(cp);
    if (!glyph) {
      glyph = data->getGlyph('?');
    }
    if (!glyph) {
      continue;
    }

    minX = std::min(minX, cursorX + glyph->left);
    maxX = std::max(maxX, cursorX + glyph->left + glyph->width);
    minY = std::min(minY, cursorY + glyph->top - glyph->height);
    maxY = std::max(maxY, cursorY + glyph->top);
    cursorX += glyph->advanceX;
  }

  *w = maxX - minX;
  *h = maxY - minY;
}

bool SdFont::hasPrintableChars(const char* string) const {
  int w = 0, h = 0;
  getTextDimensions(string, &w, &h);
  return w > 0 || h > 0;
}

const EpdGlyph* SdFont::getGlyph(uint32_t cp) const {
  if (data == nullptr) {
    return nullptr;
  }
  return data->getGlyph(cp);
}

const uint8_t* SdFont::getGlyphBitmap(uint32_t cp) const {
  if (data == nullptr) {
    return nullptr;
  }
  return data->getGlyphBitmap(cp);
}
