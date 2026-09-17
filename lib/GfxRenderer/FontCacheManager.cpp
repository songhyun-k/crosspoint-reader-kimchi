#include "FontCacheManager.h"

#include <FontDecompressor.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>

namespace {

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

FontCacheManager::~FontCacheManager() { cancelPrewarm(); }

void FontCacheManager::setFontDecompressor(FontDecompressor* d) { fontDecompressor_ = d; }

void FontCacheManager::clearCache() {
  cancelPrewarm();
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
  cancelPrewarm();
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

void FontCacheManager::recordText(const char* text, int fontId, EpdFontFamily::Style style) {
  if (!text || *text == '\0') return;

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
  while (*cursor) {
    const uint32_t codepoint = utf8NextCodepoint(&cursor);
    if (codepoint == 0) break;

    const uint32_t packed = (static_cast<uint32_t>(fontSlot) << SCAN_FONT_SHIFT) |
                            (static_cast<uint32_t>(resolvedStyle) << SCAN_STYLE_SHIFT) | codepoint;
    bool found = false;
    for (uint16_t i = 0; i < scanCodepointCount_; i++) {
      if (scanCodepoints_[i] == packed) {
        found = true;
        break;
      }
    }
    if (found) continue;

    if (scanCodepointCount_ >= MAX_SCAN_CODEPOINTS) {
      if (!scanOverflowWarned_) {
        LOG_DBG("FCM", "Scan codepoint cap (%u) reached; excess glyphs will load on demand",
                static_cast<unsigned>(MAX_SCAN_CODEPOINTS));
        scanOverflowWarned_ = true;
      }
      continue;
    }

    scanCodepoints_[scanCodepointCount_++] = packed;
    scanGroupCounts_[group]++;
  }
}

// --- PrewarmScope implementation ---

FontCacheManager::PrewarmScope::PrewarmScope(FontCacheManager& manager) : manager_(&manager) {
  manager_->cancelPrewarm();
  manager_->scanMode_ = ScanMode::Scanning;
  manager_->releaseScopeCache();
  manager_->resetStats();
  manager_->scanOverflowWarned_ = false;
}

void FontCacheManager::beginPrewarm(const bool deferred) {
  if (scanMode_ != ScanMode::Scanning) return;
  scanMode_ = deferred ? ScanMode::Deferred : ScanMode::Prewarming;
  if (scanCodepointCount_ == 0) {
    finishPrewarm();
    return;
  }
  retainScanFonts();
  std::sort(scanCodepoints_, scanCodepoints_ + scanCodepointCount_);
  prewarmGroup_ = SCAN_GROUP_COUNT - 1;
}

bool FontCacheManager::isPrewarming() const {
  return scanMode_ == ScanMode::Prewarming || scanMode_ == ScanMode::Deferred;
}

void FontCacheManager::finishPrewarm() {
  const bool deferred = scanMode_ == ScanMode::Deferred;
  scanMode_ = ScanMode::None;
  prewarmGroup_ = -1;
  pendingSdFont_ = nullptr;
  scanCodepointCount_ = 0;
  scanFontCount_ = 0;
  memset(scanGroupCounts_, 0, sizeof(scanGroupCounts_));
  if (deferred) releaseScopeCache();
}

void FontCacheManager::cancelPrewarm() {
  if (pendingSdFont_) pendingSdFont_->cancelPrewarm();
  if (fontDecompressor_) fontDecompressor_->cancelPrewarm();
  finishPrewarm();
}

bool FontCacheManager::prewarmSome(const uint16_t maxUnits) {
  if (!isPrewarming()) return true;
  if (maxUnits == 0) return false;
  if (pendingSdFont_) {
    const int result = pendingSdFont_->prewarmSome(maxUnits);
    if (result == SdCardFont::PREWARM_PENDING) return false;
    if (result != 0) LOG_DBG("FCM", "Deferred SD prewarm result: %d", result);
    pendingSdFont_ = nullptr;
    --prewarmGroup_;
    return false;
  } else if (fontDecompressor_ && fontDecompressor_->isPrewarming()) {
    const int result = fontDecompressor_->prewarmSome(maxUnits);
    if (result == FontDecompressor::PREWARM_PENDING) return false;
    if (result != 0) LOG_DBG("FCM", "Deferred built-in prewarm result: %d", result);
    --prewarmGroup_;
    return false;
  }
  while (prewarmGroup_ >= 0 && scanGroupCounts_[prewarmGroup_] == 0) --prewarmGroup_;
  if (prewarmGroup_ < 0) {
    finishPrewarm();
    return true;
  }

  uint16_t start = 0;
  for (int group = 0; group < prewarmGroup_; ++group) start += scanGroupCounts_[group];
  const uint16_t count = scanGroupCounts_[prewarmGroup_];
  // Reverse group order keeps the in-place UTF-8 terminator out of unread entries.
  char* const text = reinterpret_cast<char*>(scanCodepoints_ + start);
  char* output = text;
  for (uint16_t i = 0; i < count; ++i) {
    output = appendUtf8Codepoint(output, scanCodepoints_[start + i] & SCAN_CODEPOINT_MASK);
  }
  *output = '\0';
  const int fontId = scanFontIds_[prewarmGroup_ / 4];
  const uint8_t styleMask = 1 << (prewarmGroup_ & 3);
  const auto sd = sdCardFonts_.find(fontId);
  if (sd != sdCardFonts_.end()) {
    if (sd->second->beginPrewarm(text, styleMask)) {
      pendingSdFont_ = sd->second;
    } else {
      LOG_ERR("FCM", "Cannot start SD prewarm for font %d", fontId);
      --prewarmGroup_;
    }
  } else {
    const auto font = fontMap_.find(fontId);
    int result = 0;
    if (fontDecompressor_ && font != fontMap_.end()) {
      const auto style = static_cast<EpdFontFamily::Style>(prewarmGroup_ & 3);
      result = fontDecompressor_->beginPrewarm(font->second.getData(style), text);
    }
    if (result != FontDecompressor::PREWARM_PENDING) --prewarmGroup_;
  }
  return false;
}

void FontCacheManager::PrewarmScope::endScanAndPrewarm() {
  manager_->beginPrewarm(false);
  while (!manager_->prewarmSome(UINT16_MAX)) {
  }
}

void FontCacheManager::PrewarmScope::deferPrewarm() {
  manager_->beginPrewarm(true);
  active_ = false;
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
