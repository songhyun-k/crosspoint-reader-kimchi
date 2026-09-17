#pragma once

#include <Epub.h>
#include <Epub/PageLink.h>
#include <Epub/Section.h>
#include <Logging.h>

#include <algorithm>
#include <optional>
#include <vector>

#include "ProgressFile.h"

namespace EpubReaderUtils {

constexpr int PARTIAL_REBUILD_START_MARGIN = 15;

enum class IdleAction : uint8_t { None, Font, StartSection, Section, ScanNextPage };
struct IdleWork {
  IdleAction action = IdleAction::None;
  uint32_t delayMs = 0;
};
// Worker-local observations, never published across the activity boundary.
struct IdleFacts {
  const Section* section = nullptr;
  bool inputActive = false;
  bool fontPending = false;
  bool scanNextPage = false;
  bool canStartBuild = false;
  bool hasRendered = false;
  uint32_t idleMs = 0;
  size_t freeHeap = 0;
  size_t maxBlock = 0;
};

// The dispatcher and its next-wake snapshot use the same eligibility decision.
inline IdleWork nextIdleWork(const IdleFacts& state) {
  if (state.inputActive || !state.section || !state.hasRendered) return {};
  if (state.fontPending) return {IdleAction::Font, 0};
  constexpr uint32_t FINISH_DELAY_MS = 1000;
  constexpr uint32_t FONT_DELAY_MS = 400;
  // ponytail: bounded optional-memory polling; tune with target recovery/energy measurements.
  constexpr uint32_t HEAP_RETRY_MS = 250;
  constexpr size_t MIN_BLOCK = 16 * 1024;
  constexpr size_t MIN_BUILD_HEAP = 32 * 1024;
  constexpr size_t MIN_FONT_HEAP = 24 * 1024;
  constexpr int BUILD_WINDOW_AHEAD = 5;
  const auto& section = *state.section;
  const bool retained = section.hasRetainedBuildOperation();
  IdleWork work;
  if (state.scanNextPage && !retained && section.currentPage + 1 < section.pageCount) {
    const uint32_t delay = state.idleMs < FONT_DELAY_MS ? FONT_DELAY_MS - state.idleMs : 0;
    work = {IdleAction::ScanNextPage,
            std::max(delay, state.freeHeap > MIN_FONT_HEAP && state.maxBlock > MIN_BLOCK ? 0u : HEAP_RETRY_MS)};
  }
  IdleWork build;
  if (section.isBuilding()) {
    const bool needed = retained || section.isPartial() || section.pageCount < section.currentPage + BUILD_WINDOW_AHEAD;
    build = {IdleAction::Section, needed || state.idleMs >= FINISH_DELAY_MS ? 0 : FINISH_DELAY_MS - state.idleMs};
  } else if (section.isPartial() && state.canStartBuild &&
             section.currentPage + PARTIAL_REBUILD_START_MARGIN >= section.pageCount) {
    build = {IdleAction::StartSection, 0};
  }
  if (build.action != IdleAction::None) {
    if (!retained && (state.freeHeap < MIN_BUILD_HEAP || state.maxBlock < MIN_BLOCK)) {
      build.delayMs = std::max(build.delayMs, HEAP_RETRY_MS);
    }
    if (work.action == IdleAction::None || build.delayMs < work.delayMs) work = build;
  }
  return work;
}

// Persists reader progress for an EPUB to its cache directory. Returns true on success.
inline bool saveProgress(const Epub& epub, int spineIndex, int pageNumber, int pageCount,
                         std::optional<uint32_t> visibleTextOffset = std::nullopt) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  uint8_t data[10];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = pageNumber & 0xFF;
  data[3] = (pageNumber >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  size_t dataSize = 6;
  if (visibleTextOffset.has_value()) {
    data[6] = *visibleTextOffset & 0xFF;
    data[7] = (*visibleTextOffset >> 8) & 0xFF;
    data[8] = (*visibleTextOffset >> 16) & 0xFF;
    data[9] = (*visibleTextOffset >> 24) & 0xFF;
    dataSize = sizeof(data);
  }
  if (!ProgressFile::writeAtomic(epub.getCachePath(), data, dataSize)) {
    return false;
  }
  LOG_DBG("ERS", "Progress saved: spine=%d offset=%u page=%d", spineIndex, visibleTextOffset.value_or(0), pageNumber);
  return true;
}

inline const PageLink* linkAtPoint(const std::vector<PageLink>& links, const int x, const int y, const int marginLeft,
                                   const int marginTop) {
  // Finger slop, plus a floor on the target width: a note marker is often a single superscript
  // digit only a few pixels wide. The box is never grown vertically beyond its own line, so
  // taps on the lines above and below still reach the page-turn zones.
  constexpr int TOUCH_SLOP = 6;
  constexpr int MIN_TOUCH_WIDTH = 28;
  const int pageX = x - marginLeft;
  const int pageY = y - marginTop;
  const auto link = std::find_if(links.begin(), links.end(), [pageX, pageY](const PageLink& candidate) {
    return candidate.contains(pageX, pageY, TOUCH_SLOP, MIN_TOUCH_WIDTH);
  });
  return link == links.end() ? nullptr : &*link;
}

}  // namespace EpubReaderUtils
