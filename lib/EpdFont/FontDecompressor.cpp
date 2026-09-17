#include "FontDecompressor.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <bitset>
#include <cstdlib>

struct FontDecompressor::PrewarmState {
  enum class Phase : uint8_t { Align, Inflate, Extract };
  // Collection first uses all 512 words. After slot initialization, the first
  // 128 hold group IDs and the next 128 hold their running aligned offsets.
  uint32_t scratch[MAX_PAGE_GLYPHS] = {};
  std::unique_ptr<uint8_t[]> temp;
  Phase phase = Phase::Align;
  uint32_t sourceGlyph = 0;
  uint32_t sourceEnd = 0;
  uint32_t writeOffset = 0;
  int missed = 0;
  uint16_t groupCount = 0;
  uint16_t groupCursor = 0;
  uint16_t extractCursor = 0;
  uint8_t slotIndex = 0;
};

FontDecompressor::FontDecompressor() = default;

FontDecompressor::~FontDecompressor() { deinit(); }

bool FontDecompressor::init() {
  clearCache();
  return true;
}

void FontDecompressor::deinit() {
  freePageBuffer();
  freeHotGroup();
}

void FontDecompressor::clearCache() {
  freePageBuffer();
  freeHotGroup();
}

void FontDecompressor::releaseTransientCache() {
  cancelPrewarm();
  freeHotGroup();
  // Match the SD mini-cache retention floor; this is not a render admission gate.
  constexpr size_t RETAIN_MIN_FREE_HEAP = 40 * 1024;
  if (ESP.getFreeHeap() < RETAIN_MIN_FREE_HEAP) freePageBuffer();
}

void FontDecompressor::retainFonts(const EpdFontData* const* fonts, uint8_t count) {
  cancelPrewarm();
  for (uint8_t s = 0; s < pageSlotCount;) {
    bool needed = false;
    for (uint8_t i = 0; i < count; i++) {
      if (pageSlots[s].fontData == fonts[i]) {
        needed = true;
        break;
      }
    }
    if (needed)
      s++;
    else
      freePageSlot(s);
  }
}

void FontDecompressor::freePageSlot(uint8_t index) {
  free(pageSlots[index].buffer);
  free(pageSlots[index].glyphs);
  pageSlotCount--;
  if (index != pageSlotCount) pageSlots[index] = pageSlots[pageSlotCount];
  pageSlots[pageSlotCount] = {};
}

void FontDecompressor::freePageBuffer() {
  cancelPrewarm();
  while (pageSlotCount > 0) freePageSlot(pageSlotCount - 1);
}

void FontDecompressor::freeHotGroup() {
  free(hotGroup);
  hotGroup = nullptr;
  hotGroupCapacity = 0;
  hotGroupFont = nullptr;
  hotGroupIndex = UINT16_MAX;
  free(hotGlyphBuf);
  hotGlyphBuf = nullptr;
  hotGlyphBufCapacity = 0;
}

bool FontDecompressor::ensureCapacity(uint8_t*& buf, uint32_t& capacity, uint32_t needed) {
  if (capacity >= needed) return true;
  // Grow-only, free-then-malloc: every caller fully rewrites the buffer after a grow, so the
  // old contents are dead -- freeing first gives the allocator its best shot on a tight heap.
  free(buf);
  buf = static_cast<uint8_t*>(malloc(needed));  // owned by FontDecompressor, freed in freeHotGroup()
  capacity = buf ? needed : 0;
  return buf != nullptr;
}

uint16_t FontDecompressor::getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex) {
  // O(1) path for frequency-grouped fonts with glyphToGroup mapping
  if (fontData->glyphToGroup != nullptr) {
    return fontData->glyphToGroup[glyphIndex];
  }

  // Find the last contiguous group starting at or before this glyph.
  uint16_t left = 0, right = fontData->groupCount;
  while (left < right) {
    const uint16_t mid = left + (right - left) / 2;
    if (fontData->groups[mid].firstGlyphIndex <= glyphIndex) {
      left = mid + 1;
    } else {
      right = mid;
    }
  }
  if (left > 0) {
    const uint16_t groupIndex = left - 1;
    const EpdFontGroup& group = fontData->groups[groupIndex];
    if (glyphIndex - group.firstGlyphIndex < group.glyphCount) {
      return groupIndex;
    }
  }
  return fontData->groupCount;  // sentinel = not found
}

bool FontDecompressor::decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, uint8_t* outBuf,
                                       uint32_t outSize) {
  const EpdFontGroup& group = fontData->groups[groupIndex];

  const uint32_t tDecomp = millis();
  inflateReader.init(false);
  inflateReader.setSource(&fontData->bitmap[group.compressedOffset], group.compressedSize);
  if (!inflateReader.read(outBuf, outSize)) {
    stats.decompressTimeMs += millis() - tDecomp;
    LOG_ERR("FDC", "Decompression failed for group %u", groupIndex);
    return false;
  }
  stats.decompressTimeMs += millis() - tDecomp;
  return true;
}

// --- Byte-aligned helpers ---

uint32_t FontDecompressor::getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex) {
  uint32_t offset = 0;

  auto accumGlyph = [&](const EpdGlyph& g) {
    if (g.width > 0 && g.height > 0) {
      offset += ((g.width + 3) / 4) * g.height;
    }
  };

  if (fontData->glyphToGroup) {
    // Frequency-grouped: scan glyphs before glyphIndex that belong to this group
    for (uint32_t i = 0; i < glyphIndex; i++) {
      if (fontData->glyphToGroup[i] == groupIndex) {
        accumGlyph(fontData->glyph[i]);
      }
    }
  } else {
    // Contiguous-group: sum aligned sizes of preceding glyphs in the group
    const EpdFontGroup& group = fontData->groups[groupIndex];
    for (uint32_t i = group.firstGlyphIndex; i < glyphIndex; i++) {
      accumGlyph(fontData->glyph[i]);
    }
  }

  return offset;
}

void FontDecompressor::compactSingleGlyph(const uint8_t* alignedSrc, uint8_t* packedDst, uint8_t width,
                                          uint8_t height) {
  if (width == 0 || height == 0) return;
  const uint32_t rowStride = (width + 3) / 4;
  if (width % 4 == 0) {
    memcpy(packedDst, alignedSrc, rowStride * height);
    return;
  }
  uint8_t outByte = 0, outBits = 0;
  uint32_t writeIdx = 0;
  for (uint8_t y = 0; y < height; y++) {
    for (uint8_t x = 0; x < width; x++) {
      outByte = (outByte << 2) | ((alignedSrc[y * rowStride + x / 4] >> ((3 - (x % 4)) * 2)) & 0x3);
      outBits += 2;
      if (outBits == 8) {
        packedDst[writeIdx++] = outByte;
        outByte = 0;
        outBits = 0;
      }
    }
  }
  if (outBits > 0) packedDst[writeIdx] = outByte << (8 - outBits);
}

// --- getBitmap: page buffer → hot group → decompress ---

const uint8_t* FontDecompressor::findPageBitmap(const PageSlot& slot, uint32_t glyphIndex) {
  int left = 0, right = slot.glyphCount - 1;
  while (left <= right) {
    const int mid = left + (right - left) / 2;
    const auto& entry = slot.glyphs[mid];
    if (entry.glyphIndex == glyphIndex) {
      return entry.bufferOffset != UINT32_MAX ? &slot.buffer[entry.bufferOffset] : nullptr;
    }
    if (entry.glyphIndex < glyphIndex)
      left = mid + 1;
    else
      right = mid - 1;
  }
  return nullptr;
}

const uint8_t* FontDecompressor::getBitmap(const EpdFontData* fontData, const EpdGlyph* glyph, uint32_t glyphIndex) {
  const uint32_t tStart = micros();
  stats.getBitmapCalls++;

  if (!fontData->groups || fontData->groupCount == 0) {
    stats.getBitmapTimeUs += micros() - tStart;
    return &fontData->bitmap[glyph->dataOffset];
  }

  // Check page buffer slots (populated by prewarmCache — one slot per font style)
  for (uint8_t s = 0; s < pageSlotCount; s++) {
    const auto& slot = pageSlots[s];
    if (slot.fontData != fontData || slot.glyphCount == 0) continue;

    if (const uint8_t* bitmap = findPageBitmap(slot, glyphIndex)) {
      stats.cacheHits++;
      stats.getBitmapTimeUs += micros() - tStart;
      return bitmap;
    }
    break;  // Found the right slot but glyph wasn't in it; don't check other slots
  }

  // Fallback: hot group slot
  uint16_t groupIndex = getGroupIndex(fontData, glyphIndex);
  if (groupIndex >= fontData->groupCount) {
    LOG_ERR("FDC", "Glyph %u not found in any group", glyphIndex);
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  // Check if hot group already has this group decompressed — if not, decompress it
  if (!(hotGroup != nullptr && hotGroupFont == fontData && hotGroupIndex == groupIndex)) {
    stats.cacheMisses++;
    const EpdFontGroup& group = fontData->groups[groupIndex];

    // ensureCapacity may free the buffer, so the cached-group identity dies with it either way.
    hotGroupFont = nullptr;
    hotGroupIndex = UINT16_MAX;
    if (!ensureCapacity(hotGroup, hotGroupCapacity, group.uncompressedSize)) {
      LOG_ERR("FDC", "Failed to allocate %u bytes for hot group %u", group.uncompressedSize, groupIndex);
      stats.getBitmapTimeUs += micros() - tStart;
      return nullptr;
    }

    if (!decompressGroup(fontData, groupIndex, hotGroup, group.uncompressedSize)) {
      stats.getBitmapTimeUs += micros() - tStart;
      return nullptr;
    }

    hotGroupFont = fontData;
    hotGroupIndex = groupIndex;
    stats.hotGroupBytes = group.uncompressedSize;
  } else {
    stats.cacheHits++;
  }

  // Compact just the requested glyph from byte-aligned data into scratch buffer
  if (!ensureCapacity(hotGlyphBuf, hotGlyphBufCapacity, glyph->dataLength)) {
    LOG_ERR("FDC", "Failed to allocate %u bytes for glyph scratch", (unsigned)glyph->dataLength);
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  uint32_t alignedOff = getAlignedOffset(fontData, groupIndex, glyphIndex);
  compactSingleGlyph(&hotGroup[alignedOff], hotGlyphBuf, glyph->width, glyph->height);
  stats.getBitmapTimeUs += micros() - tStart;
  return hotGlyphBuf;
}

// --- Prewarm: pre-decompress glyph bitmaps for a page of text ---

int32_t FontDecompressor::findGlyphIndex(const EpdFontData* fontData, uint32_t codepoint) {
  const EpdUnicodeInterval* intervals = fontData->intervals;
  const int count = fontData->intervalCount;

  if (count == 0) return -1;

  // Binary search
  int left = 0;
  int right = count - 1;

  while (left <= right) {
    const int mid = left + (right - left) / 2;
    const EpdUnicodeInterval* interval = &intervals[mid];

    if (codepoint < interval->first) {
      right = mid - 1;
    } else if (codepoint > interval->last) {
      left = mid + 1;
    } else {
      return static_cast<int32_t>(interval->offset + (codepoint - interval->first));
    }
  }

  return -1;
}

int FontDecompressor::prewarmCache(const EpdFontData* fontData, const char* utf8Text) {
  int result = beginPrewarm(fontData, utf8Text);
  while (result == PREWARM_PENDING) result = prewarmSome(UINT16_MAX);
  return result;
}

int FontDecompressor::beginPrewarm(const EpdFontData* fontData, const char* utf8Text) {
  cancelPrewarm();
  if (!fontData || !fontData->groups || !utf8Text) return 0;

  uint8_t resident = 0;
  while (resident < pageSlotCount && pageSlots[resident].fontData != fontData) ++resident;
  const PageSlot* cached = resident < pageSlotCount ? &pageSlots[resident] : nullptr;
  std::bitset<MAX_PAGE_GLYPHS> selected;
  std::unique_ptr<PrewarmState> work;
  uint16_t glyphCount = 0;
  bool glyphCapWarned = false;
  const auto cachedEntry = [cached](uint32_t index) -> const PageGlyphEntry* {
    if (!cached) return nullptr;
    auto* end = cached->glyphs + cached->glyphCount;
    const auto* entry = std::lower_bound(cached->glyphs, end, index, [](const PageGlyphEntry& glyph, uint32_t value) {
      return glyph.glyphIndex < value;
    });
    return entry != end && entry->glyphIndex == index ? entry : nullptr;
  };
  const auto containsGlyph = [&](uint32_t index) {
    if (work) return std::find(work->scratch, work->scratch + glyphCount, index) != work->scratch + glyphCount;
    const auto* entry = cachedEntry(index);
    return entry && selected[entry - cached->glyphs];
  };
  const auto addGlyph = [&](uint32_t index) {
    if (containsGlyph(index)) return true;
    if (glyphCount == MAX_PAGE_GLYPHS) {
      if (!glyphCapWarned) {
        LOG_DBG("FDC", "Glyph cap (%u) reached during prewarm; excess glyphs will use hot-group fallback",
                MAX_PAGE_GLYPHS);
        glyphCapWarned = true;
      }
      return true;
    }
    const auto* entry = cachedEntry(index);
    if (!work && entry && entry->bufferOffset != UINT32_MAX) {
      selected.set(entry - cached->glyphs);
    } else {
      if (!work) {
        // Covered text needs only a 64-byte membership set. Retain the larger
        // collection/alignment workspace only when decompression is needed.
        work = makeUniqueNoThrow<PrewarmState>();
        if (!work) {
          LOG_ERR("FDC", "Failed to allocate prewarm state");
          return false;
        }
        uint16_t next = 0;
        if (cached) {
          for (uint16_t i = 0; i < cached->glyphCount; ++i) {
            if (selected[i]) work->scratch[next++] = cached->glyphs[i].glyphIndex;
          }
        }
      }
      work->scratch[glyphCount] = index;
    }
    ++glyphCount;
    return true;
  };

  const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8Text);
  while (*p) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;
    const int32_t glyphIdx = findGlyphIndex(fontData, cp);
    if (glyphIdx >= 0 && !addGlyph(glyphIdx)) return -1;
  }

  // Ligature outputs use the same selected set, including preceding outputs.
  if (fontData->ligaturePairs) {
    for (uint32_t li = 0; li < fontData->ligaturePairCount && glyphCount < MAX_PAGE_GLYPHS; ++li) {
      const auto& pair = fontData->ligaturePairs[li];
      const int32_t left = findGlyphIndex(fontData, pair.pair >> 16);
      const int32_t right = findGlyphIndex(fontData, pair.pair & 0xFFFF);
      if (left < 0 || right < 0 || !containsGlyph(left) || !containsGlyph(right)) continue;
      const int32_t output = findGlyphIndex(fontData, pair.ligatureCp);
      if (output >= 0 && !addGlyph(output)) return -1;
    }
  }

  if (!work) return 0;
  auto* neededGlyphs = work->scratch;
  if (cached) freePageSlot(resident);
  if (pageSlotCount >= MAX_PAGE_SLOTS) {
    LOG_ERR("FDC", "All %u page buffer slots full, cannot prewarm fontData=%p", MAX_PAGE_SLOTS, (void*)fontData);
    return -1;
  }
  PageSlot& slot = pageSlots[pageSlotCount];

  uint32_t totalBytes = 0;
  for (uint16_t i = 0; i < glyphCount; ++i) totalBytes += fontData->glyph[neededGlyphs[i]].dataLength;
  // Allocate the existing per-font page buffer and lookup table.
  slot.buffer = static_cast<uint8_t*>(malloc(totalBytes));
  slot.glyphs = static_cast<PageGlyphEntry*>(malloc(glyphCount * sizeof(PageGlyphEntry)));
  if (!slot.buffer || !slot.glyphs) {
    LOG_ERR("FDC", "Failed to allocate page buffer (%u bytes, %u glyphs)", totalBytes, glyphCount);
    free(slot.buffer);
    free(slot.glyphs);
    slot = {};
    return glyphCount;
  }
  stats.pageBufferBytes += totalBytes;
  stats.pageGlyphsBytes += glyphCount * sizeof(PageGlyphEntry);

  slot.fontData = fontData;
  slot.glyphCount = glyphCount;
  work->slotIndex = pageSlotCount++;

  // Initialize lookup entries (bufferOffset = UINT32_MAX means not yet extracted)
  for (uint16_t i = 0; i < glyphCount; i++) {
    slot.glyphs[i] = {neededGlyphs[i], UINT32_MAX, 0};
  }

  // Step 2: Compute total buffer size and collect unique groups
  auto* neededGroups = work->scratch;
  auto& groupCount = work->groupCount;
  bool groupCapWarned = false;

  for (uint16_t i = 0; i < glyphCount; i++) {
    uint16_t gi = getGroupIndex(fontData, slot.glyphs[i].glyphIndex);
    bool found = false;
    for (uint8_t j = 0; j < groupCount; j++) {
      if (neededGroups[j] == gi) {
        found = true;
        break;
      }
    }
    if (!found) {
      if (groupCount < 128) {
        neededGroups[groupCount++] = gi;
      } else if (!groupCapWarned) {
        LOG_DBG("FDC", "Group cap (128) reached during prewarm; some groups will use hot-group fallback");
        groupCapWarned = true;
      }
    }
  }

  stats.uniqueGroupsAccessed = groupCount;

  // Sort by glyphIndex for binary search in getBitmap()
  for (uint16_t i = 1; i < glyphCount; i++) {
    PageGlyphEntry key = slot.glyphs[i];
    int j = i - 1;
    while (j >= 0 && slot.glyphs[j].glyphIndex > key.glyphIndex) {
      slot.glyphs[j + 1] = slot.glyphs[j];
      j--;
    }
    slot.glyphs[j + 1] = key;
  }

  uint32_t maxTempBytes = 0;
  for (uint8_t g = 0; g < groupCount; g++) {
    const uint32_t groupBytes = fontData->groups[neededGroups[g]].uncompressedSize;
    if (groupBytes > maxTempBytes) maxTempBytes = groupBytes;
  }
  work->temp = makeUniqueNoThrow<uint8_t[]>(maxTempBytes);
  if (!work->temp) {
    LOG_ERR("FDC", "Failed to allocate temp buffer (%u bytes)", maxTempBytes);
    freePageSlot(pageSlotCount - 1);
    return groupCount;  // Leave heap available for the hot-group fallback.
  }
  if (maxTempBytes > stats.peakTempBytes) stats.peakTempBytes = maxTempBytes;

  std::fill(work->scratch + 128, work->scratch + 256, 0);
  if (fontData->glyphToGroup) {
    const auto& last = fontData->intervals[fontData->intervalCount - 1];
    work->sourceEnd = last.offset + last.last - last.first + 1;
  }
  prewarm = std::move(work);
  return PREWARM_PENDING;
}

void FontDecompressor::cancelPrewarm() {
  if (!prewarm) return;
  const uint8_t slot = prewarm->slotIndex;
  prewarm.reset();
  freePageSlot(slot);
}

int FontDecompressor::prewarmSome(uint16_t maxUnits) {
  if (!prewarm) return 0;
  auto& work = *prewarm;
  auto& slot = pageSlots[work.slotIndex];
  const auto* font = slot.fontData;
  const auto* groups = work.scratch;
  auto* aligned = work.scratch + 128;
  using Phase = PrewarmState::Phase;
  while (maxUnits-- != 0) {
    if (work.phase == Phase::Align) {
      constexpr uint16_t ALIGN_RECORDS_PER_UNIT = 64;
      for (uint16_t n = 0; n < ALIGN_RECORDS_PER_UNIT; ++n) {
        uint32_t glyphIndex = work.sourceGlyph;
        uint16_t groupPosition = work.groupCursor;
        if (font->glyphToGroup) {
          if (work.sourceGlyph == work.sourceEnd) {
            work.phase = Phase::Inflate;
            work.groupCursor = 0;
            break;
          }
          const uint16_t group = font->glyphToGroup[work.sourceGlyph++];
          groupPosition = work.groupCount;
          for (uint16_t g = 0; g < work.groupCount; ++g) {
            if (groups[g] == group) {
              groupPosition = g;
              break;
            }
          }
          if (groupPosition == work.groupCount) continue;
        } else {
          if (work.groupCursor == work.groupCount) {
            work.phase = Phase::Inflate;
            work.groupCursor = 0;
            break;
          }
          const auto& group = font->groups[groups[work.groupCursor]];
          if (work.sourceGlyph == group.glyphCount) {
            ++work.groupCursor;
            work.sourceGlyph = 0;
            continue;
          }
          glyphIndex = group.firstGlyphIndex + work.sourceGlyph++;
        }
        uint16_t left = 0, end = slot.glyphCount;
        while (left < end) {
          const uint16_t mid = left + (end - left) / 2;
          if (slot.glyphs[mid].glyphIndex < glyphIndex)
            left = mid + 1;
          else
            end = mid;
        }
        if (left < slot.glyphCount && slot.glyphs[left].glyphIndex == glyphIndex) {
          slot.glyphs[left].alignedOffset = aligned[groupPosition];
        }
        const auto& glyph = font->glyph[glyphIndex];
        if (glyph.width > 0 && glyph.height > 0) aligned[groupPosition] += ((glyph.width + 3) / 4) * glyph.height;
      }
    } else if (work.phase == Phase::Inflate) {
      if (work.groupCursor == work.groupCount) {
        const int result = work.missed;
        prewarm.reset();
        return result;
      }
      const uint16_t groupIndex = groups[work.groupCursor];
      const auto& group = font->groups[groupIndex];
      if (!decompressGroup(font, groupIndex, work.temp.get(), group.uncompressedSize)) {
        ++work.missed;
        ++work.groupCursor;
        continue;
      }
      work.extractCursor = 0;
      if (!font->glyphToGroup) {
        uint16_t end = slot.glyphCount;
        while (work.extractCursor < end) {
          const uint16_t mid = work.extractCursor + (end - work.extractCursor) / 2;
          if (slot.glyphs[mid].glyphIndex < group.firstGlyphIndex)
            work.extractCursor = mid + 1;
          else
            end = mid;
        }
      }
      work.phase = Phase::Extract;
    } else {
      const uint16_t groupIndex = groups[work.groupCursor];
      const auto& group = font->groups[groupIndex];
      if (work.extractCursor == slot.glyphCount ||
          (!font->glyphToGroup &&
           slot.glyphs[work.extractCursor].glyphIndex >= group.firstGlyphIndex + group.glyphCount)) {
        ++work.groupCursor;
        work.phase = Phase::Inflate;
        continue;
      }
      auto& entry = slot.glyphs[work.extractCursor++];
      if (font->glyphToGroup &&
          (entry.bufferOffset != UINT32_MAX || getGroupIndex(font, entry.glyphIndex) != groupIndex))
        continue;
      const auto& glyph = font->glyph[entry.glyphIndex];
      compactSingleGlyph(work.temp.get() + entry.alignedOffset, slot.buffer + work.writeOffset, glyph.width,
                         glyph.height);
      entry.bufferOffset = work.writeOffset;
      work.writeOffset += glyph.dataLength;
    }
  }
  return PREWARM_PENDING;
}

// --- Stats ---

void FontDecompressor::resetStats() { stats = Stats{}; }

void FontDecompressor::logStats(const char* label) {
  const uint32_t total = stats.cacheHits + stats.cacheMisses;
  LOG_DBG("FDC", "[%s] hits=%lu misses=%lu (%.1f%% hit rate)", label, stats.cacheHits, stats.cacheMisses,
          total > 0 ? 100.0f * stats.cacheHits / total : 0.0f);
  LOG_DBG("FDC", "[%s] decompress=%lums groups_accessed=%u", label, stats.decompressTimeMs, stats.uniqueGroupsAccessed);
  LOG_DBG("FDC", "[%s] mem: pageBuf=%lu pageGlyphs=%lu hotGroup=%lu peakTemp=%lu", label, stats.pageBufferBytes,
          stats.pageGlyphsBytes, stats.hotGroupBytes, stats.peakTempBytes);
  if (stats.getBitmapCalls > 0) {
    LOG_DBG("FDC", "[%s] getBitmap: %lu calls, %luus total, %luus/call avg", label, stats.getBitmapCalls,
            stats.getBitmapTimeUs, stats.getBitmapTimeUs / stats.getBitmapCalls);
  }
  resetStats();
}
