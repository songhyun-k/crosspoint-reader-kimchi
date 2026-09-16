#include "FontCacheManager.h"

#include <Arduino.h>
#include <FontDecompressor.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFont.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>

namespace {

bool scanScratchFits(const size_t bytes) {
  // Leave the reader's gray-plane headroom available to other scan-time work.
  return ESP.getFreeHeap() >= 60000 + bytes && ESP.getMaxAllocHeap() >= 16 * 1024 + bytes;
}

char* appendUtf8Codepoint(char* output, const uint32_t codepoint) {
  if (codepoint < 0x80) {
    *output++ = static_cast<char>(codepoint);
  } else if (codepoint < 0x800) {
    *output++ = static_cast<char>(0xC0 | (codepoint >> 6));
    *output++ = static_cast<char>(0x80 | (codepoint & 0x3F));
  } else if (codepoint < 0x10000) {
    *output++ = static_cast<char>(0xE0 | (codepoint >> 12));
    *output++ = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    *output++ = static_cast<char>(0x80 | (codepoint & 0x3F));
  } else {
    *output++ = static_cast<char>(0xF0 | (codepoint >> 18));
    *output++ = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
    *output++ = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    *output++ = static_cast<char>(0x80 | (codepoint & 0x3F));
  }
  return output;
}

}  // namespace

FontCacheManager::FontCacheManager(const std::map<int, EpdFontFamily>& fontMap,
                                   const std::map<int, SdCardFont*>& sdCardFonts)
    : fontMap_(fontMap), sdCardFonts_(sdCardFonts) {}

void FontCacheManager::setFontDecompressor(FontDecompressor* d) { fontDecompressor_ = d; }

void FontCacheManager::clearCache() {
  if (fontDecompressor_) fontDecompressor_->clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    font->clearCache();
  }
}

void FontCacheManager::releaseScopeCache() {
  if (fontDecompressor_) fontDecompressor_->releaseTransientCache();
  for (auto& [id, font] : sdCardFonts_) {
    font->clearCache();
  }
}

void FontCacheManager::retainScanFonts() {
  if (!fontDecompressor_) return;
  const EpdFontData* fonts[SCAN_GROUP_COUNT];
  uint8_t count = 0;
  for (uint8_t group = 0; group < SCAN_GROUP_COUNT; group++) {
    if (scanGroupCounts_[group] == 0) continue;
    const int fontId = scanFontIds_[group / 4];
    if (sdCardFonts_.count(fontId)) continue;
    const auto font = fontMap_.find(fontId);
    if (font == fontMap_.end()) continue;
    const auto* data = font->second.getData(static_cast<EpdFontFamily::Style>(group & 0x03));
    if (data && data->groups) fonts[count++] = data;
  }
  fontDecompressor_->retainFonts(fonts, count);
}

void FontCacheManager::releaseSdFontCaches() {
  if (fontDecompressor_) fontDecompressor_->clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    font->releaseResidentCaches();
  }
}

void FontCacheManager::prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask) {
  // SD card font prewarm path: prewarm all requested styles in one call
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    int missed = it->second->prewarm(utf8Text, styleMask);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache(SD): %d glyph(s) not found (styleMask=0x%02X)", missed, styleMask);
    }
    return;
  }

  // Standard compressed font prewarm path: loop over all requested styles
  if (!fontDecompressor_ || fontMap_.count(fontId) == 0) return;

  // A direct prewarm (settings preview / dictionary highlight) is its own batch.
  // Scan batches have already retained every requested font before this loop.
  if (scanMode_ == ScanMode::None) {
    const EpdFontData* fonts[4];
    uint8_t count = 0;
    for (uint8_t i = 0; i < 4; i++) {
      if (styleMask & (1 << i)) fonts[count++] = fontMap_.at(fontId).getData(static_cast<EpdFontFamily::Style>(i));
    }
    fontDecompressor_->retainFonts(fonts, count);
  }

  for (uint8_t i = 0; i < 4; i++) {
    if (!(styleMask & (1 << i))) continue;
    auto style = static_cast<EpdFontFamily::Style>(i);
    const EpdFontData* data = fontMap_.at(fontId).getData(style);
    if (!data || !data->groups) continue;
    int missed = fontDecompressor_->prewarmCache(data, utf8Text);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache: %d glyph(s) not cached for style %d", missed, i);
    }
  }
}

void FontCacheManager::logStats(const char* label) {
  if (fontDecompressor_) fontDecompressor_->logStats(label);
  for (auto& [id, font] : sdCardFonts_) {
    font->logStats(label);
  }
}

void FontCacheManager::resetStats() {
  if (fontDecompressor_) fontDecompressor_->resetStats();
  for (auto& [id, font] : sdCardFonts_) {
    font->resetStats();
  }
}

bool FontCacheManager::isScanning() const { return scanMode_ == ScanMode::Scanning; }

uint8_t FontCacheManager::resolveScanStyle(int fontId, EpdFontFamily::Style style) const {
  const uint8_t baseStyle = static_cast<uint8_t>(style) & 0x03;

  const auto sdFont = sdCardFonts_.find(fontId);
  if (sdFont != sdCardFonts_.end()) return sdFont->second->resolveStyle(baseStyle);

  const auto font = fontMap_.find(fontId);
  if (font == fontMap_.end()) return baseStyle;

  const EpdFontData* resolvedData = font->second.getData(static_cast<EpdFontFamily::Style>(baseStyle));
  for (uint8_t candidate = 0; candidate < 4; candidate++) {
    if (font->second.getData(static_cast<EpdFontFamily::Style>(candidate)) == resolvedData) return candidate;
  }
  return baseStyle;
}

uint16_t FontCacheManager::findScanLookupSlot(const uint32_t packed) const {
  uint16_t slot = (packed * 2654435761u) >> 22;
  for (uint16_t probes = 0; probes < SCAN_LOOKUP_SLOTS; ++probes) {
    const uint16_t entry = scanLookup_[slot];
    if (entry == 0 || scanCodepoints_[entry - 1] == packed) return slot;
    slot = (slot + 1) & (SCAN_LOOKUP_SLOTS - 1);
  }
  return SCAN_LOOKUP_SLOTS;
}

void FontCacheManager::buildScanLookup() {
  scanLookupAttempted_ = true;
  if (!scanScratchFits(SCAN_LOOKUP_SLOTS * sizeof(uint16_t))) return;
  scanLookup_ = makeUniqueNoThrow<uint16_t[]>(SCAN_LOOKUP_SLOTS);
  if (!scanLookup_) {
    LOG_ERR("FCM", "Scan index allocation failed; using linear lookup");
    return;
  }
  for (uint16_t i = 0; i < scanCodepointCount_; ++i) {
    const uint16_t slot = findScanLookupSlot(scanCodepoints_[i]);
    if (slot == SCAN_LOOKUP_SLOTS) {
      scanLookup_.reset();
      return;
    }
    scanLookup_[slot] = i + 1;
  }
}

// Keep hash probing out of the small-scan path's stack frame.
[[gnu::noinline]] const unsigned char* FontCacheManager::recordIndexed(const unsigned char* cursor,
                                                                       const uint32_t packedGroup,
                                                                       const uint8_t group) {
  while (*cursor) {
    const unsigned char* const start = cursor;
    const uint32_t codepoint = utf8NextCodepoint(&cursor);
    if (codepoint == 0) return nullptr;

    const uint32_t packed = packedGroup | codepoint;
    const uint16_t slot = findScanLookupSlot(packed);
    if (slot == SCAN_LOOKUP_SLOTS) {
      scanLookup_.reset();
      return start;
    }
    if (scanLookup_[slot]) continue;

    if (scanCodepointCount_ >= MAX_SCAN_CODEPOINTS) {
      if (!scanOverflowWarned_) {
        LOG_DBG("FCM", "Scan codepoint cap (%u) reached; excess glyphs will load on demand",
                static_cast<unsigned>(MAX_SCAN_CODEPOINTS));
        scanOverflowWarned_ = true;
      }
      continue;
    }

    scanLookup_[slot] = scanCodepointCount_ + 1;
    scanCodepoints_[scanCodepointCount_++] = packed;
    scanGroupCounts_[group]++;
  }
  return nullptr;
}

void FontCacheManager::recordNonEmptyText(const char* text, int fontId, EpdFontFamily::Style style) {
  uint8_t fontSlot = scanFontCount_;
  for (uint8_t i = 0; i < scanFontCount_; i++) {
    if (scanFontIds_[i] == fontId) {
      fontSlot = i;
      break;
    }
  }
  if (fontSlot == scanFontCount_) {
    if (scanFontCount_ >= MAX_SCAN_FONTS) return;
    scanFontIds_[scanFontCount_++] = fontId;
  }

  const uint8_t resolvedStyle = resolveScanStyle(fontId, style);
  const uint8_t group = fontSlot * 4 + resolvedStyle;
  const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text);
  const uint32_t packedGroup =
      (static_cast<uint32_t>(fontSlot) << SCAN_FONT_SHIFT) | (static_cast<uint32_t>(resolvedStyle) << SCAN_STYLE_SHIFT);
  if (scanCodepointCount_ >= SCAN_LOOKUP_THRESHOLD) {
    if (scanLookup_ && !scanScratchFits(0)) scanLookup_.reset();
    if (!scanLookupAttempted_ && scanMode_ == ScanMode::Scanning) buildScanLookup();
    if (scanLookup_) {
      cursor = recordIndexed(cursor, packedGroup, group);
      if (!cursor) return;
    }
  }

  uint32_t previousCodepoint = 0;
  while (*cursor) {
    const uint32_t codepoint = utf8NextCodepoint(&cursor);
    if (codepoint == 0) return;
    if (codepoint == previousCodepoint) continue;
    previousCodepoint = codepoint;
    const uint32_t packed = packedGroup | codepoint;
    auto* const end = scanCodepoints_ + scanCodepointCount_;
    if (std::find(scanCodepoints_, end, packed) != end) continue;
    if (scanCodepointCount_ >= MAX_SCAN_CODEPOINTS) {
      if (!scanOverflowWarned_) {
        LOG_DBG("FCM", "Scan codepoint cap (%u) reached; excess glyphs will load on demand",
                static_cast<unsigned>(MAX_SCAN_CODEPOINTS));
        scanOverflowWarned_ = true;
      }
      continue;
    }
    const uint16_t index = scanCodepointCount_++;
    scanCodepoints_[index] = packed;
    scanGroupCounts_[group]++;
    if (index == SCAN_LOOKUP_THRESHOLD - 1 && !scanLookupAttempted_ && scanMode_ == ScanMode::Scanning && *cursor) {
      buildScanLookup();
      if (scanLookup_) {
        cursor = recordIndexed(cursor, packedGroup, group);
        if (!cursor) return;
      }
    }
  }
}

// --- PrewarmScope implementation ---

FontCacheManager::PrewarmScope::PrewarmScope(FontCacheManager& manager) : manager_(&manager) {
  manager_->scanLookup_.reset();
  manager_->scanLookupAttempted_ = false;
  manager_->scanMode_ = ScanMode::Scanning;
  manager_->releaseScopeCache();
  manager_->resetStats();
  manager_->scanCodepointCount_ = 0;
  manager_->scanFontCount_ = 0;
  manager_->scanOverflowWarned_ = false;
  memset(manager_->scanGroupCounts_, 0, sizeof(manager_->scanGroupCounts_));
}

void FontCacheManager::PrewarmScope::endScanAndPrewarm() {
  if (!active_) return;
  manager_->scanLookup_.reset();
  manager_->scanMode_ = ScanMode::None;
  if (manager_->scanCodepointCount_ == 0) return;

  manager_->retainScanFonts();
  manager_->scanMode_ = ScanMode::Prewarming;
  std::sort(manager_->scanCodepoints_, manager_->scanCodepoints_ + manager_->scanCodepointCount_);

  uint16_t groupStarts[SCAN_GROUP_COUNT] = {};
  for (uint8_t group = 1; group < SCAN_GROUP_COUNT; group++) {
    groupStarts[group] = groupStarts[group - 1] + manager_->scanGroupCounts_[group - 1];
  }

  // Each packed entry provides four bytes, enough for one UTF-8 codepoint.
  // Encoding high groups first means a terminator can overwrite only a group
  // that has already been prewarmed; unread lower groups remain intact.
  for (int group = SCAN_GROUP_COUNT - 1; group >= 0; group--) {
    const uint16_t groupCount = manager_->scanGroupCounts_[group];
    if (groupCount == 0) continue;

    const uint16_t groupStart = groupStarts[group];
    char* const utf8Text = reinterpret_cast<char*>(manager_->scanCodepoints_ + groupStart);
    char* output = utf8Text;
    for (uint16_t i = 0; i < groupCount; i++) {
      const uint32_t codepoint = manager_->scanCodepoints_[groupStart + i] & SCAN_CODEPOINT_MASK;
      output = appendUtf8Codepoint(output, codepoint);
    }
    *output = '\0';

    const uint8_t fontSlot = static_cast<uint8_t>(group) / 4;
    const uint8_t style = static_cast<uint8_t>(group) & 0x03;
    manager_->prewarmCache(manager_->scanFontIds_[fontSlot], utf8Text, 1 << style);
  }

  manager_->scanMode_ = ScanMode::None;
  manager_->scanCodepointCount_ = 0;
  manager_->scanFontCount_ = 0;
  memset(manager_->scanGroupCounts_, 0, sizeof(manager_->scanGroupCounts_));
}

FontCacheManager::PrewarmScope::~PrewarmScope() {
  if (active_) {
    endScanAndPrewarm();  // no-op if already called
    manager_->releaseScopeCache();
  }
}

FontCacheManager::PrewarmScope::PrewarmScope(PrewarmScope&& other) noexcept
    : manager_(other.manager_), active_(other.active_) {
  other.active_ = false;
}

FontCacheManager::PrewarmScope FontCacheManager::createPrewarmScope() { return PrewarmScope(*this); }
