#include "SdCardFont.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>

#include "EpdFontFamily.h"

static_assert(sizeof(EpdGlyph) == 16, "EpdGlyph must be 16 bytes to match .cpfont file layout");
static_assert(sizeof(EpdUnicodeInterval) == 12, "EpdUnicodeInterval must be 12 bytes to match .cpfont file layout");
static_assert(sizeof(EpdKernClassEntry) == 3, "EpdKernClassEntry must be 3 bytes to match .cpfont file layout");
static_assert(sizeof(EpdLigaturePair) == 8, "EpdLigaturePair must be 8 bytes to match .cpfont file layout");

namespace {

// FNV-1a hash for content-based font ID generation
constexpr uint32_t FNV_OFFSET = 2166136261u;
constexpr uint32_t FNV_PRIME = 16777619u;

uint32_t fnv1a(const uint8_t* data, size_t len, uint32_t hash = FNV_OFFSET) {
  for (size_t i = 0; i < len; i++) {
    hash ^= data[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

// .cpfont magic bytes
constexpr char CPFONT_MAGIC[8] = {'C', 'P', 'F', 'O', 'N', 'T', '\0', '\0'};
// CPFONT_VERSION is defined as a #define in SdCardFont.h so it can be
// stringified into FONT_MANIFEST_URL.
constexpr uint32_t HEADER_SIZE = 32;
constexpr uint32_t STYLE_TOC_ENTRY_SIZE = 32;

// Helper to read little-endian values from byte buffer
inline uint16_t readU16(const uint8_t* p) { return p[0] | (p[1] << 8); }
inline int16_t readI16(const uint8_t* p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); }
inline uint32_t readU32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

// Walks a null-terminated UTF-8 string and appends each unique codepoint to
// codepoints[0..cpCount-1] via O(n²) dedup.  Returns true if the buffer
// reached maxCount (cap hit), false if all codepoints fit.
bool collectUniqueCodepoints(const char* text, uint32_t* codepoints, uint32_t& cpCount, uint32_t maxCount) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  while (*p) {
    uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;
    bool found = false;
    for (uint32_t i = 0; i < cpCount; i++) {
      if (codepoints[i] == cp) {
        found = true;
        break;
      }
    }
    if (!found) {
      if (cpCount >= maxCount) return true;
      codepoints[cpCount++] = cp;
    }
  }
  return false;
}

const char* asCStr(const std::string& s) { return s.c_str(); }
const char* asCStr(const char* s) { return s; }

// resetStyleMiniData retention bounds (see the PerStyle comment in the header).
constexpr size_t MINI_RETAIN_MIN_FREE_HEAP = 40 * 1024;
constexpr uint8_t MINI_UNDERUSE_RUNS_BEFORE_FREE = 3;
// Working headroom left outside the mini bitmap arena's single contiguous block.
constexpr uint32_t PREWARM_MAX_ALLOC_RESERVE = 4 * 1024;

// Keep-if-fits buffer reuse: only reallocate when the needed size exceeds the
// current capacity. Freeing + reallocating slightly different sizes every page
// turn punches non-coalescing holes in the heap (the freed block rarely fits the
// next page's need), eroding the largest contiguous block all session. With
// reuse, capacities converge on the book's max page after a few turns and page
// turns stop touching the allocator. Only three small instantiations exist
// (interval/glyph/byte arrays), so template bloat is negligible.
template <typename T, typename CapT>
bool ensureArrayCapacity(T*& buf, CapT& capacity, const uint32_t needed) {
  if (buf && capacity >= needed) return true;
  delete[] buf;
  buf = new (std::nothrow) T[needed > 0 ? needed : 1];
  capacity = buf ? static_cast<CapT>(needed) : 0;
  return buf != nullptr;
}

}  // namespace

struct SdCardFont::PrewarmState {
  enum class Phase { Ligatures, Style, Glyphs, BitmapSetup, Bitmaps, KernSetup, KernRows, Publish };
  struct Mapping {
    uint32_t codepoint;
    int32_t globalIndex;
  };
  struct KernRows {
    uint8_t leftScratch[256] = {};  // Class renumbering first, SD row bytes after preparation.
    uint8_t rightRenumber[256] = {};
    uint8_t newToOldLeft[256] = {};
    uint8_t newToOldRight[256] = {};
    uint16_t leftEntries = 0;
    uint16_t rightEntries = 0;
    uint8_t leftCount = 0;
    uint8_t rightCount = 0;
  };
  Phase phase = Phase::Ligatures;
  std::unique_ptr<uint32_t[]> codepoints;
  std::unique_ptr<uint32_t[]> unionCodepoints;
  std::unique_ptr<Mapping[]> mappings;
  std::unique_ptr<uint32_t[]> readOrder;
  std::unique_ptr<KernRows> kern;
  HalFile file;
  const uint32_t* styleCodepoints = nullptr;
  uint32_t codepointCount = 0;
  uint32_t styleLimit = 0;
  uint32_t styleCodepointCount = 0;
  uint32_t validCount = 0;
  uint32_t cursor = 0;
  uint32_t bitmapOffset = 0;
  uint32_t lastBitmapEnd = UINT32_MAX;
  int32_t lastReadIndex = INT32_MIN;
  unsigned long startedAt = 0;
  int totalMissed = 0;
  int styleMissed = 0;
  uint8_t style = 0;
  uint8_t styleMask = 0;
  bool retryingArena = false;
  bool metadataOnly = false;
  bool loadKernLig = true;
  bool styleMetadataOnly = false;
  bool rebuilding = false;
  bool kernOk = false;
};

SdCardFont::SdCardFont() = default;

SdCardFont::~SdCardFont() { freeAll(); }

// --- Per-style free/cleanup ---

void SdCardFont::freeStyleMiniData(PerStyle& s) {
  delete[] s.miniIntervals;
  s.miniIntervals = nullptr;
  delete[] s.miniGlyphs;
  s.miniGlyphs = nullptr;
  delete[] s.miniBitmap;
  s.miniBitmap = nullptr;
  s.miniIntervalCount = 0;
  s.miniGlyphCount = 0;
  s.miniIntervalCapacity = 0;
  s.miniGlyphCapacity = 0;
  s.miniBitmapCapacity = 0;
  s.miniBitmapUsed = 0;
  s.miniUnderuseRuns = 0;
  freeStyleMiniKern(s);
  memset(&s.miniData, 0, sizeof(s.miniData));
  s.epdFont.data = &s.stubData;
}

void SdCardFont::resetStyleMiniData(PerStyle& s) {
  // Retention is a bet that the next scope needs similar data. Don't hold it
  // when the heap is tight: the arenas are rebuildable for one page's worth of
  // allocations, and this floor keeps retained fonts out of the way of section
  // builds and the render path's own floors.
  if (ESP.getFreeHeap() < MINI_RETAIN_MIN_FREE_HEAP) {
    freeStyleMiniData(s);
    return;
  }
  // Underuse hysteresis, on the bitmap arena (the dominant allocation): an
  // outlier page (e.g. three styles cramped together) would otherwise pin its
  // high-water arena for the rest of the book. Keep while the page used at
  // least 3/4 of capacity; release only after several consecutive rebuilds
  // below that, so alternating dense/sparse pages never thrash. Evaluated at
  // most once per rebuild (a scope both constructs and destructs through here,
  // and subset hits load nothing new to judge).
  if (s.miniHysteresisPending && s.miniBitmapCapacity > 0 && s.miniBitmapUsed > 0) {
    s.miniHysteresisPending = false;
    if (s.miniBitmapUsed < s.miniBitmapCapacity - s.miniBitmapCapacity / 4) {
      if (++s.miniUnderuseRuns >= MINI_UNDERUSE_RUNS_BEFORE_FREE) {
        LOG_DBG("SDCF", "mini release (underuse): used=%u cap=%u", s.miniBitmapUsed, s.miniBitmapCapacity);
        freeStyleMiniData(s);
        return;
      }
    } else {
      s.miniUnderuseRuns = 0;
    }
  }
  // Data (intervals/glyphs/bitmaps/kern) deliberately survives the scope: the
  // next prewarm subset-checks against it, which is what lets the idle prewarm
  // of page N+1 serve the actual page turn with zero SD reads.
}

void SdCardFont::freeStyleKernLigatureData(PerStyle& s) {
  // Both views can outlive the resident table after a cache release. Clear
  // published references in the common teardown, including read/alloc failures.
  s.stubData.ligaturePairs = nullptr;
  s.stubData.ligaturePairCount = 0;
  s.miniData.ligaturePairs = nullptr;
  s.miniData.ligaturePairCount = 0;
  delete[] s.kernLeftClasses;
  s.kernLeftClasses = nullptr;
  delete[] s.kernRightClasses;
  s.kernRightClasses = nullptr;
  delete[] s.ligaturePairs;
  s.ligaturePairs = nullptr;
  s.kernLigLoaded = false;
}

void SdCardFont::freeStyleMiniKern(PerStyle& s) {
  delete[] s.miniKernLeftClasses;
  s.miniKernLeftClasses = nullptr;
  delete[] s.miniKernRightClasses;
  s.miniKernRightClasses = nullptr;
  delete[] s.miniKernMatrix;
  s.miniKernMatrix = nullptr;
  s.miniKernLeftEntryCount = 0;
  s.miniKernRightEntryCount = 0;
  s.miniKernLeftClassCount = 0;
  s.miniKernRightClassCount = 0;
  s.miniKernLeftCapacity = 0;
  s.miniKernRightCapacity = 0;
  s.miniKernMatrixCapacity = 0;
}

void SdCardFont::freeStyleAll(PerStyle& s) {
  freeStyleMiniData(s);
  delete[] s.fullIntervals;
  s.fullIntervals = nullptr;
  delete[] s.bmpIntervals;
  s.bmpIntervals = nullptr;
  s.intervalsAreBmp16 = false;
  freeStyleKernLigatureData(s);
  s.present = false;
}

// --- Global free/cleanup ---

void SdCardFont::releaseResidentCaches() {
  cancelPrewarm();
  clearOverflow();
  clearPersistentCache();
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    freeStyleMiniData(styles_[i]);  // also frees mini kern and restores the stub EpdFontData
    freeStyleKernLigatureData(styles_[i]);
    applyGlyphMissCallback(i);  // keep the on-demand miss path alive on the stub
  }
}

void SdCardFont::freeAll() {
  cancelPrewarm();
  clearOverflow();
  clearPersistentCache();
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    freeStyleAll(styles_[i]);
  }
  styleCount_ = 0;
  contentHash_ = 0;
  loaded_ = false;
}

void SdCardFont::clearOverflow() {
  for (uint32_t i = 0; i < overflowCount_; i++) {
    delete[] overflow_[i].bitmap;
    overflow_[i].bitmap = nullptr;
    overflow_[i].codepoint = 0;
  }
  overflowCount_ = 0;
  overflowNext_ = 0;
}

// --- Per-style kern/ligature ---

void SdCardFont::applyKernLigaturePointers(PerStyle& s, EpdFontData& data) const {
  // Kern data uses the per-page mini tables (renumbered class IDs). The full
  // kern matrix is never resident — see PerStyle::miniKernMatrix comment.
  data.kernLeftClasses = s.miniKernLeftClasses;
  data.kernRightClasses = s.miniKernRightClasses;
  // Packed class maps and dense matrix, as stored in the .cpfont and mapped in place; the split
  // and sparse forms are built-in only. Set explicitly rather than relying on the caller's
  // initialisation: getKerning() picks the representation by which pointer is non-null.
  data.kernLeftCodepoints = nullptr;
  data.kernLeftClassIds = nullptr;
  data.kernRightCodepoints = nullptr;
  data.kernRightClassIds = nullptr;
  data.kernRowOffsets = nullptr;
  data.kernSparseCols = nullptr;
  data.kernSparseValues = nullptr;
  data.kernMatrix = s.miniKernMatrix;
  data.kernLeftEntryCount = s.miniKernLeftEntryCount;
  data.kernRightEntryCount = s.miniKernRightEntryCount;
  data.kernLeftClassCount = s.miniKernLeftClassCount;
  data.kernRightClassCount = s.miniKernRightClassCount;
  // Ligatures are small (typically < 1KB) so they stay resident.
  data.ligaturePairs = s.ligaturePairs;
  data.ligaturePairCount = s.ligaturePairs ? s.header.ligaturePairCount : 0;
}

int SdCardFont::loadStyleKernLigatureSome(PrewarmState& work) {
  auto& s = styles_[work.style];
  if (s.kernLigLoaded) return 0;
  const bool hasKern = s.header.kernLeftEntryCount > 0;
  const bool hasLig = s.header.ligaturePairCount > 0;
  if ((hasKern || hasLig) && !work.file && !Storage.openFileForRead("SDCF", filePath_, work.file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont for kern/lig: %s", filePath_);
    return -1;
  }

  // Each retained pointer represents a complete table; it is reused on resume.
  if (hasKern && !s.kernLeftClasses) {
    auto table = makeUniqueNoThrow<EpdKernClassEntry[]>(s.header.kernLeftEntryCount);
    const size_t bytes = s.header.kernLeftEntryCount * sizeof(EpdKernClassEntry);
    if (!table || !work.file.seekSet(s.kernLeftFileOffset) ||
        work.file.read(table.get(), bytes) != static_cast<int>(bytes)) {
      LOG_ERR("SDCF", "Failed to prepare left kern classes");
      freeStyleKernLigatureData(s);
      return -1;
    }
    s.kernLeftClasses = table.release();
    return PREWARM_PENDING;
  }
  if (hasKern && s.header.kernRightEntryCount > 0 && !s.kernRightClasses) {
    auto table = makeUniqueNoThrow<EpdKernClassEntry[]>(s.header.kernRightEntryCount);
    const size_t bytes = s.header.kernRightEntryCount * sizeof(EpdKernClassEntry);
    if (!table || (work.file.position() != s.kernRightFileOffset && !work.file.seekSet(s.kernRightFileOffset)) ||
        work.file.read(table.get(), bytes) != static_cast<int>(bytes)) {
      LOG_ERR("SDCF", "Failed to prepare right kern classes");
      freeStyleKernLigatureData(s);
      return -1;
    }
    s.kernRightClasses = table.release();
    return PREWARM_PENDING;
  }
  if (hasLig && !s.ligaturePairs) {
    auto table = makeUniqueNoThrow<EpdLigaturePair[]>(s.header.ligaturePairCount);
    const size_t bytes = s.header.ligaturePairCount * sizeof(EpdLigaturePair);
    if (!table || !work.file.seekSet(s.ligatureFileOffset) ||
        work.file.read(table.get(), bytes) != static_cast<int>(bytes)) {
      LOG_ERR("SDCF", "Failed to prepare ligature pairs");
      freeStyleKernLigatureData(s);
      return -1;
    }
    s.ligaturePairs = table.release();
    return PREWARM_PENDING;
  }
  s.kernLigLoaded = true;
  s.stubData.ligaturePairs = s.ligaturePairs;
  s.stubData.ligaturePairCount = s.ligaturePairs ? s.header.ligaturePairCount : 0;
  return 0;
}

// --- Per-page mini kern matrix ---

// Local copy of EpdFont.cpp's lookupKernClass (that one is file-static there).
// Returns the 1-based class ID for `cp`, or 0 if the codepoint has no kerning class.
static uint8_t miniLookupKernClass(const EpdKernClassEntry* entries, uint16_t count, uint32_t cp) {
  if (!entries || count == 0 || cp > 0xFFFF) return 0;
  const auto target = static_cast<uint16_t>(cp);
  const auto* end = entries + count;
  const auto it =
      std::lower_bound(entries, end, target, [](const EpdKernClassEntry& e, uint16_t v) { return e.codepoint < v; });
  return (it != end && it->codepoint == target) ? it->classId : 0;
}

// Build a small per-page kern matrix containing ONLY the (leftClass, rightClass)
// pairs reachable from codepoints in the current text. Class IDs are renumbered
// to a dense 1..N range so the resulting matrix is usedLeft × usedRight (typical
// Latin page: ~25×25 bytes) instead of the font's full ~180×200 (~36KB).
//
// Correctness: EpdFont::getKerning only touches `kernLeftClasses` /
// `kernRightClasses` / `kernMatrix` / the count fields — we swap all of them to
// the mini versions together in applyKernLigaturePointers, so a codepoint not
// on this page simply returns class 0 (no kerning), which was the pre-existing
// behavior for any codepoint outside the kern classes.
bool SdCardFont::prepareMiniKern(PrewarmState& work) {
  auto& s = styles_[work.style];
  const uint32_t* codepoints = work.styleCodepoints;
  const uint32_t cpCount = work.styleCodepointCount;
  const auto resetMiniKernCounts = [&s]() {
    s.miniKernLeftEntryCount = 0;
    s.miniKernRightEntryCount = 0;
    s.miniKernLeftClassCount = 0;
    s.miniKernRightClassCount = 0;
  };
  if (!s.kernLeftClasses || !s.kernRightClasses || s.header.kernLeftEntryCount == 0 ||
      s.header.kernRightEntryCount == 0) {
    resetMiniKernCounts();
    return true;  // font has no kern classes — nothing to build
  }

  // Retained across row reads; allocate only after glyph/read-order scratch is released.
  work.kern = makeUniqueNoThrow<PrewarmState::KernRows>();
  if (!work.kern) {
    LOG_ERR("SDCF", "Failed to allocate mini kern work state");
    freeStyleMiniKern(s);
    return false;
  }
  auto& rows = *work.kern;
  auto& leftRenumber = rows.leftScratch;
  auto& rightRenumber = rows.rightRenumber;
  auto& newToOldLeft = rows.newToOldLeft;
  auto& newToOldRight = rows.newToOldRight;
  for (uint32_t i = 0; i < cpCount; i++) {
    uint8_t lc = miniLookupKernClass(s.kernLeftClasses, s.header.kernLeftEntryCount, codepoints[i]);
    if (lc) leftRenumber[lc] = 1;
    uint8_t rc = miniLookupKernClass(s.kernRightClasses, s.header.kernRightEntryCount, codepoints[i]);
    if (rc) rightRenumber[rc] = 1;
  }

  // Step 2: build renumber maps (oldClassId -> newClassId, 1-based) and
  // reverse maps (newClassId -> oldClassId) for the SD read step.
  auto& numLeft = rows.leftCount;
  auto& numRight = rows.rightCount;
  for (int i = 1; i < 256; i++) {
    if (leftRenumber[i]) {
      numLeft++;
      leftRenumber[i] = numLeft;
      newToOldLeft[numLeft] = static_cast<uint8_t>(i);
    }
    if (rightRenumber[i]) {
      numRight++;
      rightRenumber[i] = numRight;
      newToOldRight[numRight] = static_cast<uint8_t>(i);
    }
  }
  if (numLeft == 0 || numRight == 0) {
    resetMiniKernCounts();
    return true;  // no kern pairs applicable on this page
  }

  // Step 3: count how many codepoint→classId entries the mini class tables need.
  // Each resident class table has one entry per kerned codepoint in the page.
  uint16_t miniLeftCount = 0;
  uint16_t miniRightCount = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    if (miniLookupKernClass(s.kernLeftClasses, s.header.kernLeftEntryCount, codepoints[i]) != 0) miniLeftCount++;
    if (miniLookupKernClass(s.kernRightClasses, s.header.kernRightEntryCount, codepoints[i]) != 0) miniRightCount++;
  }

  // Step 4: size the three mini buffers (reused across pages when they fit; the
  // per-page sizes vary by a few entries, which as free+realloc churn was punching
  // non-coalescing holes in the heap every page turn).
  const uint32_t matrixBytes = static_cast<uint32_t>(numLeft) * numRight;
  if (!ensureArrayCapacity(s.miniKernLeftClasses, s.miniKernLeftCapacity, miniLeftCount) ||
      !ensureArrayCapacity(s.miniKernRightClasses, s.miniKernRightCapacity, miniRightCount) ||
      !ensureArrayCapacity(s.miniKernMatrix, s.miniKernMatrixCapacity, matrixBytes)) {
    LOG_ERR("SDCF", "Failed to allocate mini kern (%u+%u+%u bytes)", miniLeftCount * 3u, miniRightCount * 3u,
            matrixBytes);
    freeStyleMiniKern(s);
    return false;
  }

  // Step 5: populate mini class tables. `codepoints` is already sorted (see
  // prewarm()) so the output is sorted by codepoint — required for binary
  // search in lookupKernClass during render.
  auto& lIdx = rows.leftEntries;
  auto& rIdx = rows.rightEntries;
  for (uint32_t i = 0; i < cpCount; i++) {
    uint32_t cp = codepoints[i];
    if (cp > 0xFFFF) continue;  // kern class entries are uint16_t
    uint8_t lc = miniLookupKernClass(s.kernLeftClasses, s.header.kernLeftEntryCount, cp);
    if (lc) {
      s.miniKernLeftClasses[lIdx].codepoint = static_cast<uint16_t>(cp);
      s.miniKernLeftClasses[lIdx].classId = leftRenumber[lc];
      lIdx++;
    }
    uint8_t rc = miniLookupKernClass(s.kernRightClasses, s.header.kernRightEntryCount, cp);
    if (rc) {
      s.miniKernRightClasses[rIdx].codepoint = static_cast<uint16_t>(cp);
      s.miniKernRightClasses[rIdx].classId = rightRenumber[rc];
      rIdx++;
    }
  }

  if (!work.file && !Storage.openFileForRead("SDCF", filePath_, work.file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont for mini kern: %s", filePath_);
    freeStyleMiniKern(s);
    return false;
  }
  work.cursor = 1;
  work.phase = PrewarmState::Phase::KernRows;
  return true;
}

// --- Glyph miss callback ---

void SdCardFont::applyGlyphMissCallback(uint8_t styleIdx) {
  overflowCtx_[styleIdx].self = this;
  overflowCtx_[styleIdx].styleIdx = styleIdx;

  auto& s = styles_[styleIdx];
  s.stubData.glyphMissHandler = &SdCardFont::onGlyphMiss;
  s.stubData.glyphMissCtx = &overflowCtx_[styleIdx];
  s.stubData.coverageHandler = &SdCardFont::onCoverageQuery;
}

bool SdCardFont::onCoverageQuery(void* ctx, const uint32_t codepoint) {
  const auto* octx = static_cast<OverflowContext*>(ctx);
  const PerStyle& s = octx->self->styles_[octx->styleIdx];
  if (!s.fullIntervals && !s.bmpIntervals) return false;  // coverage index freed/never loaded
  return octx->self->findGlobalGlyphIndex(s, codepoint) >= 0;
}

// --- Compute per-style file offsets from a base data offset ---

void SdCardFont::computeStyleFileOffsets(PerStyle& s, uint32_t baseOffset) {
  s.intervalsFileOffset = baseOffset;
  s.glyphsFileOffset = s.intervalsFileOffset + s.header.intervalCount * sizeof(EpdUnicodeInterval);
  s.kernLeftFileOffset = s.glyphsFileOffset + s.header.glyphCount * sizeof(EpdGlyph);
  s.kernRightFileOffset = s.kernLeftFileOffset + s.header.kernLeftEntryCount * sizeof(EpdKernClassEntry);
  s.kernMatrixFileOffset = s.kernRightFileOffset + s.header.kernRightEntryCount * sizeof(EpdKernClassEntry);
  s.ligatureFileOffset =
      s.kernMatrixFileOffset + static_cast<uint32_t>(s.header.kernLeftClassCount) * s.header.kernRightClassCount;
  s.bitmapFileOffset = s.ligatureFileOffset + s.header.ligaturePairCount * sizeof(EpdLigaturePair);
}

// --- Load ---

bool SdCardFont::load(const char* path) {
  freeAll();
  if (strlen(path) >= sizeof(filePath_)) {
    LOG_ERR("SDCF", "Path too long (%zu bytes, max %zu)", strlen(path), sizeof(filePath_) - 1);
    return false;
  }
  strncpy(filePath_, path, sizeof(filePath_) - 1);
  filePath_[sizeof(filePath_) - 1] = '\0';

  HalFile file;
  if (!Storage.openFileForRead("SDCF", path, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont: %s", path);
    return false;
  }

  // Read and validate global header
  uint8_t headerBuf[HEADER_SIZE];
  if (file.read(headerBuf, HEADER_SIZE) != HEADER_SIZE) {
    LOG_ERR("SDCF", "Failed to read header");
    return false;
  }

  if (memcmp(headerBuf, CPFONT_MAGIC, 8) != 0) {
    LOG_ERR("SDCF", "Invalid magic bytes");
    return false;
  }

  uint16_t fileVersion = readU16(headerBuf + 8);
  if (fileVersion != CPFONT_VERSION) {
    LOG_ERR("SDCF", "Unsupported version: %u (expected %u)", fileVersion, CPFONT_VERSION);
    return false;
  }

  // Begin content hash: accumulate global header
  uint32_t hash = fnv1a(headerBuf, HEADER_SIZE);

  bool is2Bit = (readU16(headerBuf + 10) & 1) != 0;

  uint8_t styleCount = headerBuf[12];
  if (styleCount == 0 || styleCount > MAX_STYLES) {
    LOG_ERR("SDCF", "Invalid style count: %u", styleCount);
    return false;
  }

  // Read style TOC
  for (uint8_t i = 0; i < styleCount; i++) {
    uint8_t tocBuf[STYLE_TOC_ENTRY_SIZE];
    if (file.read(tocBuf, STYLE_TOC_ENTRY_SIZE) != STYLE_TOC_ENTRY_SIZE) {
      LOG_ERR("SDCF", "Failed to read style TOC entry %u", i);
      freeAll();
      return false;
    }

    // Accumulate TOC entry into content hash
    hash = fnv1a(tocBuf, STYLE_TOC_ENTRY_SIZE, hash);

    uint8_t styleId = tocBuf[0];
    if (styleId >= MAX_STYLES) {
      LOG_ERR("SDCF", "Invalid styleId %u in TOC", styleId);
      file.close();
      freeAll();
      return false;
    }

    auto& s = styles_[styleId];
    s.present = true;
    s.header.intervalCount = readU32(tocBuf + 4);
    s.header.glyphCount = readU32(tocBuf + 8);
    s.header.advanceY = tocBuf[12];
    s.header.ascender = readI16(tocBuf + 13);
    s.header.descender = readI16(tocBuf + 15);
    s.header.kernLeftEntryCount = readU16(tocBuf + 17);
    s.header.kernRightEntryCount = readU16(tocBuf + 19);
    s.header.kernLeftClassCount = tocBuf[21];
    s.header.kernRightClassCount = tocBuf[22];
    s.header.ligaturePairCount = tocBuf[23];
    s.header.is2Bit = is2Bit;

    // Sanity-check counts to reject malformed files before allocating.
    // Kern class counts are uint8 (bounded by type). Entry counts are uint16
    // but in practice a sane font has far fewer than 4096 per-side kern entries.
    static constexpr uint32_t MAX_INTERVALS = 4096;
    static constexpr uint32_t MAX_GLYPHS = 65536;
    static constexpr uint32_t MAX_KERN_ENTRIES = 4096;
    if (s.header.intervalCount > MAX_INTERVALS || s.header.glyphCount > MAX_GLYPHS ||
        s.header.kernLeftEntryCount > MAX_KERN_ENTRIES || s.header.kernRightEntryCount > MAX_KERN_ENTRIES) {
      LOG_ERR("SDCF", "Style %u: unreasonable counts (iv=%u, gl=%u, kL=%u, kR=%u)", styleId, s.header.intervalCount,
              s.header.glyphCount, s.header.kernLeftEntryCount, s.header.kernRightEntryCount);
      file.close();
      freeAll();
      return false;
    }

    uint32_t dataOffset = readU32(tocBuf + 24);
    computeStyleFileOffsets(s, dataOffset);
  }

  styleCount_ = styleCount;
  contentHash_ = hash;

  // Load full intervals into RAM for each present style. BMP-only fonts with
  // fewer than 65536 glyphs use a compact 6-byte interval table instead of the
  // on-disk 12-byte table; large sparse CJK subsets otherwise keep tens of KB
  // of always-resident heap just for lookup metadata.
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    auto& s = styles_[i];
    if (!s.present) continue;

    if (!file.seekSet(s.intervalsFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek to intervals for style %u", i);
      freeAll();
      return false;
    }

    // Validate interval contents before any later code (findGlobalGlyphIndex,
    // glyph reads) trusts them. A malformed file could otherwise drive
    // out-of-range glyph indices into bogus on-disk reads.
    bool canUseBmp16 = s.header.glyphCount <= UINT16_MAX;
    uint32_t expectedOffset = 0;
    uint32_t prevLast = 0;
    EpdUnicodeInterval iv{};
    for (uint32_t j = 0; j < s.header.intervalCount; ++j) {
      if (file.read(reinterpret_cast<uint8_t*>(&iv), sizeof(iv)) != sizeof(iv)) {
        LOG_ERR("SDCF", "Failed to read interval %u for style %u", j, i);
        freeAll();
        return false;
      }
      if (iv.first > iv.last) {
        LOG_ERR("SDCF", "Style %u: invalid interval %u (first 0x%lX > last 0x%lX)", i, j,
                static_cast<unsigned long>(iv.first), static_cast<unsigned long>(iv.last));
        file.close();
        freeAll();
        return false;
      }
      const uint32_t span = iv.last - iv.first + 1;
      const bool overlapsPrev = (j > 0 && iv.first <= prevLast);
      const bool spanTooBig = (span > s.header.glyphCount);
      const bool offsetMismatch = (iv.offset != expectedOffset);
      const bool offsetOverruns = (iv.offset > s.header.glyphCount - span);
      if (overlapsPrev || spanTooBig || offsetMismatch || offsetOverruns) {
        LOG_ERR("SDCF", "Style %u: invalid interval layout at %u (overlap=%d span=%u offMis=%d offOver=%d)", i, j,
                overlapsPrev, span, offsetMismatch, offsetOverruns);
        file.close();
        freeAll();
        return false;
      }
      if (iv.first > UINT16_MAX || iv.last > UINT16_MAX || iv.offset > UINT16_MAX) {
        canUseBmp16 = false;
      }
      expectedOffset += span;
      prevLast = iv.last;
    }

    if (!file.seekSet(s.intervalsFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek back to intervals for style %u", i);
      freeAll();
      return false;
    }

    if (canUseBmp16) {
      s.bmpIntervals = new (std::nothrow) PerStyle::BmpInterval16[s.header.intervalCount];
      if (!s.bmpIntervals) {
        LOG_ERR("SDCF", "Failed to allocate compact intervals for style %u", i);
        freeAll();
        return false;
      }
      for (uint32_t j = 0; j < s.header.intervalCount; ++j) {
        if (file.read(reinterpret_cast<uint8_t*>(&iv), sizeof(iv)) != sizeof(iv)) {
          LOG_ERR("SDCF", "Failed to read compact interval %u for style %u", j, i);
          freeAll();
          return false;
        }
        s.bmpIntervals[j] = {static_cast<uint16_t>(iv.first), static_cast<uint16_t>(iv.last),
                             static_cast<uint16_t>(iv.offset)};
      }
      s.intervalsAreBmp16 = true;
    } else {
      s.fullIntervals = new (std::nothrow) EpdUnicodeInterval[s.header.intervalCount];
      if (!s.fullIntervals) {
        LOG_ERR("SDCF", "Failed to allocate %u intervals for style %u", s.header.intervalCount, i);
        freeAll();
        return false;
      }
      size_t intervalsBytes = s.header.intervalCount * sizeof(EpdUnicodeInterval);
      if (file.read(reinterpret_cast<uint8_t*>(s.fullIntervals), intervalsBytes) != static_cast<int>(intervalsBytes)) {
        LOG_ERR("SDCF", "Failed to read intervals for style %u", i);
        freeAll();
        return false;
      }
    }

    // Initialize stub data
    memset(&s.stubData, 0, sizeof(s.stubData));
    s.stubData.advanceY = s.header.advanceY;
    s.stubData.ascender = s.header.ascender;
    s.stubData.descender = s.header.descender;
    s.stubData.is2Bit = s.header.is2Bit;

    s.epdFont.data = &s.stubData;
    applyGlyphMissCallback(i);
  }

  loaded_ = true;

  LOG_DBG("SDCF", "Loaded: %s (v%u, %u styles)", path, CPFONT_VERSION, styleCount_);
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    const auto& h = styles_[i].header;
    LOG_DBG("SDCF", "  style[%u]: %u intervals, %u glyphs, advY=%u, asc=%d, desc=%d, kernL=%u, kernR=%u, ligs=%u", i,
            h.intervalCount, h.glyphCount, h.advanceY, h.ascender, h.descender, h.kernLeftEntryCount,
            h.kernRightEntryCount, h.ligaturePairCount);
  }
  return true;
}

// --- Codepoint lookup ---

int32_t SdCardFont::findGlobalGlyphIndex(const PerStyle& s, uint32_t codepoint) const {
  int left = 0;
  int right = static_cast<int>(s.header.intervalCount) - 1;
  while (left <= right) {
    int mid = left + (right - left) / 2;
    const uint32_t first = s.intervalsAreBmp16 ? s.bmpIntervals[mid].first : s.fullIntervals[mid].first;
    const uint32_t last = s.intervalsAreBmp16 ? s.bmpIntervals[mid].last : s.fullIntervals[mid].last;
    if (codepoint < first) {
      right = mid - 1;
    } else if (codepoint > last) {
      left = mid + 1;
    } else {
      const uint32_t offset = s.intervalsAreBmp16 ? s.bmpIntervals[mid].offset : s.fullIntervals[mid].offset;
      return static_cast<int32_t>(offset + (codepoint - first));
    }
  }
  return -1;
}

// --- Prewarm ---

namespace {
const char* singleTextGetter(const void* ctx, uint32_t) { return static_cast<const char*>(ctx); }
}  // namespace

int SdCardFont::prewarm(const char* utf8Text, uint8_t styleMask, bool metadataOnly, bool loadKernLig) {
  return prewarm(&singleTextGetter, utf8Text, 1, styleMask, metadataOnly, loadKernLig);
}

int SdCardFont::prewarm(TextGetter getter, const void* ctx, uint32_t textCount, uint8_t styleMask, bool metadataOnly,
                        bool loadKernLig) {
  if (!beginPrewarm(getter, ctx, textCount, styleMask, metadataOnly, loadKernLig)) return -1;
  int result;
  do {
    result = prewarmSome(UINT16_MAX);
  } while (result == PREWARM_PENDING);
  return result;
}

bool SdCardFont::beginPrewarm(const char* text, const uint8_t styleMask) {
  return beginPrewarm(&singleTextGetter, text, 1, styleMask, false, true);
}

bool SdCardFont::beginPrewarm(TextGetter getter, const void* ctx, uint32_t textCount, uint8_t styleMask,
                              bool metadataOnly, bool loadKernLig) {
  cancelPrewarm();
  if (!loaded_ || getter == nullptr) return false;
  styleMask = resolveStyleMask(styleMask);
  if (styleMask == 0) return true;
  // The operation and its existing scratch buffers outlive a resume call.
  auto work = makeUniqueNoThrow<PrewarmState>();
  if (!work) {
    LOG_ERR("SDCF", "Failed to allocate prewarm state");
    return false;
  }
  work->startedAt = millis();
  // Cap the unique-codepoint budget by what the heap can actually hold as a
  // full mini arena (glyph structs + bitmaps, working headroom left over).
  // Multi-string batches only: a several-hundred-chapter CJK table of
  // contents would otherwise extract up to MAX_PAGE_GLYPHS and fail the whole
  // arena allocation — better to load the first screens' worth and let
  // scrolling union-in the rest page by page. Per-string requests are small
  // and already bounded by the union gate in prewarmStyle (running the check
  // there would also log per draw call); metadata-only prewarms load no
  // bitmaps. Bytes/glyph prefers the measured average from the resident mini:
  // Hangul ink boxes run well under the advanceY-squared em estimate, which
  // otherwise roughly halves the usable budget.
  uint32_t cpBudget = MAX_PAGE_GLYPHS;
  if (!metadataOnly && textCount > 1) {
    uint8_t refStyle = MAX_STYLES;
    for (uint8_t si = 0; si < MAX_STYLES && refStyle == MAX_STYLES; si++) {
      if ((styleMask & (1 << si)) && styles_[si].present) refStyle = si;
    }
    if (refStyle < MAX_STYLES) {
      const auto& s = styles_[refStyle];
      const uint32_t bpp = s.header.is2Bit ? 2 : 1;
      uint32_t bitmapPerGlyph = (static_cast<uint32_t>(s.header.advanceY) * s.header.advanceY * bpp) / 8 + 4;
      if (s.miniGlyphCount > 0 && s.miniBitmapUsed > 0) {
        bitmapPerGlyph = s.miniBitmapUsed / s.miniGlyphCount;
      }
      const uint32_t perGlyph = bitmapPerGlyph + sizeof(EpdGlyph);
      constexpr uint32_t PREWARM_HEAP_HEADROOM = 16 * 1024;
      const uint32_t freeHeap = ESP.getFreeHeap();
      const uint32_t budgetBytes = freeHeap > PREWARM_HEAP_HEADROOM ? freeHeap - PREWARM_HEAP_HEADROOM : 0;
      const uint32_t budgetGlyphs = budgetBytes / (perGlyph > 0 ? perGlyph : 1);
      if (budgetGlyphs < cpBudget) {
        cpBudget = budgetGlyphs;
      }
    }
  }
  if (cpBudget == 0) return false;

  // Step 1: Extract unique codepoints from the UTF-8 texts (shared across all styles).
  // Dedup uses O(n^2) linear scan — worst case is MAX_PAGE_GLYPHS (512) unique codepoints
  // = ~131K comparisons, but in practice pages contain far fewer unique codepoints so the
  // actual cost is much lower. This is dwarfed by SD I/O that follows. Alternatives (hash
  // set, bitmap) exceed the 256-byte stack limit or add template bloat.
  // Heap-allocated: MAX_PAGE_GLYPHS * 4 = 2048 bytes, too large for stack (limit < 256 bytes)
  auto codepoints = makeUniqueNoThrow<uint32_t[]>(MAX_PAGE_GLYPHS);
  if (!codepoints) {
    LOG_ERR("SDCF", "Failed to allocate codepoint buffer (%u bytes)", MAX_PAGE_GLYPHS * 4);
    return false;
  }
  uint32_t cpCount = 0;

  for (uint32_t ti = 0; ti < textCount && cpCount < cpBudget; ti++) {
    const char* text = getter(ctx, ti);
    if (text == nullptr) continue;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
    while (*p && cpCount < cpBudget) {
      uint32_t cp = utf8NextCodepoint(&p);
      if (cp == 0) break;

      bool found = false;
      for (uint32_t i = 0; i < cpCount; i++) {
        if (codepoints[i] == cp) {
          found = true;
          break;
        }
      }
      if (!found) {
        codepoints[cpCount++] = cp;
      }
    }
  }

  // Always include the replacement character
  {
    bool hasReplacement = false;
    for (uint32_t i = 0; i < cpCount; i++) {
      if (codepoints[i] == REPLACEMENT_GLYPH) {
        hasReplacement = true;
        break;
      }
    }
    if (!hasReplacement && cpCount < MAX_PAGE_GLYPHS) {
      codepoints[cpCount++] = REPLACEMENT_GLYPH;
    }
  }

  work->codepoints = std::move(codepoints);
  work->codepointCount = cpCount;
  work->styleMask = styleMask;
  work->metadataOnly = metadataOnly;
  work->loadKernLig = loadKernLig;
  if (metadataOnly || !loadKernLig) {
    std::sort(work->codepoints.get(), work->codepoints.get() + cpCount);
    work->phase = PrewarmState::Phase::Style;
  }
  prewarm_ = std::move(work);
  return true;
}

int SdCardFont::preparePrewarmStyle(PrewarmState& work) {
  const uint8_t styleIdx = work.style;
  auto& s = styles_[styleIdx];
  const uint32_t* codepoints = work.codepoints.get();
  uint32_t cpCount = work.styleLimit;
  bool metadataOnly = work.metadataOnly;
  const bool loadKernLig = work.loadKernLig;
  // Idle-prewarm hit: mini data persists across PrewarmScopes (resetStyleMiniData
  // keeps it), so when the previous scope -- typically the idle prewarm of this
  // exact page -- already loaded every requested codepoint the font covers, this
  // page needs zero SD reads. A mini built metadata-only cannot serve a full
  // request (no bitmaps). Any uncovered codepoint falls through to the rebuild.
  if (s.miniGlyphCount > 0 && !(s.miniMetadataOnly && !metadataOnly)) {
    bool covered = true;
    int missedInMini = 0;
    for (uint32_t i = 0; i < cpCount && covered; i++) {
      const uint32_t cp = codepoints[i];
      bool inMini = false;
      for (uint32_t iv = 0; iv < s.miniIntervalCount; iv++) {
        if (cp < s.miniIntervals[iv].first) break;  // intervals sorted ascending
        if (cp <= s.miniIntervals[iv].last) {
          inMini = true;
          break;
        }
      }
      if (inMini) continue;
      if (findGlobalGlyphIndex(s, cp) < 0) {
        missedInMini++;  // not in font coverage: the rebuild couldn't load it either
      } else {
        covered = false;
      }
    }
    if (covered) {
      // A kern-wanting request (reader path) can subset-hit a mini that a
      // kern-free UI prewarm built: top up the kern matrix for the requested
      // codepoints without re-reading any glyphs.
      if (!metadataOnly && loadKernLig && s.miniKernLeftClassCount == 0 && s.header.kernLeftEntryCount > 0) {
        work.styleCodepoints = codepoints;
        work.styleCodepointCount = cpCount;
        work.styleMissed = missedInMini;
        work.phase = PrewarmState::Phase::KernSetup;
        return PREWARM_PENDING;
      }
      return missedInMini;
    }
  }

  // Merge the resident mini's codepoints into the request so the rebuild below
  // accumulates instead of replacing. Screens draw several distinct fallback
  // strings per refresh (file browser rows, chapter lists, the reader status
  // bar after the page scope); replacing meant every string evicted every
  // other string's glyphs, so each measure/draw re-hit the SD forever. With
  // the union, residency converges after one pass and redraws are RAM-only.
  // Over MAX_PAGE_GLYPHS the union is abandoned (request-only rebuild), which
  // bounds mini RAM to the same worst case as a single dense page.
  auto& unionCps = work.unionCodepoints;
  if (s.miniGlyphCount > 0 && s.miniIntervalCount > 0 && ESP.getFreeHeap() < MINI_RETAIN_MIN_FREE_HEAP) {
    // Heap-tight (e.g. a chapter list stacked over an open book). Size-aware:
    // a small union (a UI screen's worth of titles, a few KB) is exactly what
    // stops per-string eviction from re-reading the SD on every repaint, so
    // allow it as long as the estimated arena leaves headroom. Only when the
    // union would crowd the remaining heap (page-scale arenas) drop the
    // retained data and rebuild request-only below: bounded like the
    // pre-merge behavior, and the freed arena gives the small alloc room.
    const uint32_t unionMaxCount = s.miniGlyphCount + cpCount;  // pre-dedup upper bound
    const uint32_t avgBitmapBytes =
        (s.miniBitmapUsed > 0 && s.miniGlyphCount > 0) ? s.miniBitmapUsed / s.miniGlyphCount : 64;
    const uint32_t estArenaBytes = unionMaxCount * (static_cast<uint32_t>(sizeof(EpdGlyph)) + avgBitmapBytes);
    constexpr uint32_t UNION_PRESSURE_HEADROOM = 12 * 1024;
    if (estArenaBytes + UNION_PRESSURE_HEADROOM > ESP.getFreeHeap()) {
      freeStyleMiniData(s);
    }
  }
  if (s.miniGlyphCount > 0 && s.miniIntervalCount > 0) {
    const uint32_t unionMax = s.miniGlyphCount + cpCount;
    unionCps = makeUniqueNoThrow<uint32_t[]>(unionMax);
    if (unionCps) {
      // Two-way sorted merge: resident stream walks the mini intervals
      // (ascending), request stream is the caller's sorted codepoint array.
      uint32_t n = 0;
      uint32_t ivIdx = 0;
      uint32_t ivCp = s.miniIntervals[0].first;
      bool ivActive = true;
      uint32_t ri = 0;
      while ((ivActive || ri < cpCount) && n < unionMax) {
        uint32_t next;
        if (ivActive && (ri >= cpCount || ivCp <= codepoints[ri])) {
          next = ivCp;
          if (ri < cpCount && codepoints[ri] == ivCp) ri++;
          if (ivCp < s.miniIntervals[ivIdx].last) {
            ivCp++;
          } else if (++ivIdx < s.miniIntervalCount) {
            ivCp = s.miniIntervals[ivIdx].first;
          } else {
            ivActive = false;
          }
        } else {
          next = codepoints[ri++];
        }
        unionCps[n++] = next;
      }
      if (!ivActive && ri >= cpCount && n <= MAX_PAGE_GLYPHS) {
        // A full mini must stay full: a metadata-only request may not drop
        // bitmaps other strings are still rendering from.
        metadataOnly = metadataOnly && s.miniMetadataOnly;
        codepoints = unionCps.get();
        cpCount = n;
      } else {
        unionCps.reset();
      }
    }
  }

  // Map codepoints to global glyph indices for this style
  work.mappings = makeUniqueNoThrow<PrewarmState::Mapping[]>(cpCount);
  auto* mappings = work.mappings.get();
  if (!mappings) {
    LOG_ERR("SDCF", "Failed to allocate mapping array for style %u", styleIdx);
    return static_cast<int>(cpCount);
  }

  uint32_t validCount = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    int32_t idx = findGlobalGlyphIndex(s, codepoints[i]);
    if (idx >= 0) {
      mappings[validCount].codepoint = codepoints[i];
      mappings[validCount].globalIndex = idx;
      validCount++;
    }
  }
  int missed = static_cast<int>(cpCount - validCount);

  if (validCount == 0) {
    freeStyleMiniData(s);
    s.epdFont.data = &s.stubData;
    return missed;
  }

  // Build mini intervals from sorted codepoints. Reset counts and fall back to the
  // stub until the rebuild completes, but KEEP the existing buffers (keep-if-fits
  // reuse) — the free-and-realloc-per-page pattern here was a primary fragmenter.
  work.rebuilding = true;
  s.miniIntervalCount = 0;
  s.miniGlyphCount = 0;
  s.miniKernLeftEntryCount = 0;
  s.miniKernRightEntryCount = 0;
  s.miniKernLeftClassCount = 0;
  s.miniKernRightClassCount = 0;
  memset(&s.miniData, 0, sizeof(s.miniData));
  s.epdFont.data = &s.stubData;

  if (!ensureArrayCapacity(s.miniIntervals, s.miniIntervalCapacity, validCount)) {
    LOG_ERR("SDCF", "Failed to allocate mini intervals for style %u", styleIdx);
    return static_cast<int>(cpCount);
  }

  s.miniIntervalCount = 0;
  uint32_t rangeStart = 0;
  for (uint32_t i = 1; i <= validCount; i++) {
    if (i == validCount || mappings[i].codepoint != mappings[i - 1].codepoint + 1) {
      s.miniIntervals[s.miniIntervalCount].first = mappings[rangeStart].codepoint;
      s.miniIntervals[s.miniIntervalCount].last = mappings[i - 1].codepoint;
      s.miniIntervals[s.miniIntervalCount].offset = rangeStart;
      s.miniIntervalCount++;
      rangeStart = i;
    }
  }

  // Mini glyph array (reused across pages when it fits)
  if (!ensureArrayCapacity(s.miniGlyphs, s.miniGlyphCapacity, validCount)) {
    LOG_ERR("SDCF", "Failed to allocate mini glyphs for style %u", styleIdx);
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }

  // Build sorted read order for sequential I/O
  work.readOrder = makeUniqueNoThrow<uint32_t[]>(validCount);
  auto* readOrder = work.readOrder.get();
  if (!readOrder) {
    LOG_ERR("SDCF", "Failed to allocate read order for style %u", styleIdx);
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }
  for (uint32_t i = 0; i < validCount; i++) readOrder[i] = i;
  std::sort(readOrder, readOrder + validCount,
            [&](uint32_t a, uint32_t b) { return mappings[a].globalIndex < mappings[b].globalIndex; });

  if (!work.file && !Storage.openFileForRead("SDCF", filePath_, work.file)) {
    LOG_ERR("SDCF", "Failed to reopen .cpfont for prewarm (style %u)", styleIdx);
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }

  work.styleCodepoints = codepoints;
  work.styleCodepointCount = cpCount;
  work.validCount = validCount;
  work.styleMissed = missed;
  work.styleMetadataOnly = metadataOnly;
  work.cursor = 0;
  work.lastReadIndex = INT32_MIN;
  work.phase = PrewarmState::Phase::Glyphs;
  return PREWARM_PENDING;
}

void SdCardFont::finishPrewarmStyle(PrewarmState& work, const int missed) {
  work.totalMissed += missed + static_cast<int>(work.codepointCount - work.styleLimit);
  work.unionCodepoints.reset();
  work.mappings.reset();
  work.readOrder.reset();
  work.kern.reset();
  work.styleCodepoints = nullptr;
  work.rebuilding = false;
  work.kernOk = false;
  work.styleLimit = 0;
  work.retryingArena = false;
  ++work.style;
  work.phase = PrewarmState::Phase::Style;
}

void SdCardFont::publishPrewarmStyle(PrewarmState& work) {
  auto& s = styles_[work.style];
  if (work.rebuilding) {
    s.miniGlyphCount = work.validCount;
    s.miniMetadataOnly = work.styleMetadataOnly;
    s.miniHysteresisPending = !work.styleMetadataOnly;
    s.miniData = {};
    s.miniData.bitmap = s.miniBitmap;
    s.miniData.glyph = s.miniGlyphs;
    s.miniData.intervals = s.miniIntervals;
    s.miniData.intervalCount = s.miniIntervalCount;
    s.miniData.advanceY = s.header.advanceY;
    s.miniData.ascender = s.header.ascender;
    s.miniData.descender = s.header.descender;
    s.miniData.is2Bit = s.header.is2Bit;
    s.miniData.glyphMissHandler = &SdCardFont::onGlyphMiss;
    s.miniData.glyphMissCtx = &overflowCtx_[work.style];
    s.miniData.coverageHandler = &SdCardFont::onCoverageQuery;
    stats_.uniqueGlyphs += work.validCount;
    if (!work.styleMetadataOnly) stats_.bitmapBytes += s.miniBitmapUsed;
  }
  if (work.kernOk) applyKernLigaturePointers(s, s.miniData);
  s.epdFont.data = &s.miniData;
  finishPrewarmStyle(work, work.styleMissed);
}

void SdCardFont::cancelPrewarm() {
  if (!prewarm_) return;
  auto& work = *prewarm_;
  if (work.rebuilding) {
    freeStyleMiniData(styles_[work.style]);
  } else if (work.kern) {
    auto& s = styles_[work.style];
    freeStyleMiniKern(s);
    applyKernLigaturePointers(s, s.miniData);
  }
  prewarm_.reset();
}

int SdCardFont::prewarmSome(uint16_t maxUnits) {
  if (!prewarm_) return 0;
  auto& work = *prewarm_;
  using Phase = PrewarmState::Phase;
  while (maxUnits-- != 0) {
    if (work.style == MAX_STYLES) {
      if (work.phase == Phase::Ligatures) {
        std::sort(work.codepoints.get(), work.codepoints.get() + work.codepointCount);
        work.style = 0;
        work.phase = Phase::Style;
      } else {
        const int result = work.totalMissed;
        stats_.prewarmTotalMs = millis() - work.startedAt;
        prewarm_.reset();
        return result;
      }
    }
    if (!(work.styleMask & (1 << work.style)) || !styles_[work.style].present) {
      ++work.style;
      continue;
    }
    auto& s = styles_[work.style];
    switch (work.phase) {
      case Phase::Ligatures: {
        if (loadStyleKernLigatureSome(work) == PREWARM_PENDING) break;
        // A table load failure uses the existing glyph-miss fallback.
        for (uint8_t li = 0;
             s.ligaturePairs && li < s.header.ligaturePairCount && work.codepointCount < MAX_PAGE_GLYPHS; ++li) {
          const uint32_t left = s.ligaturePairs[li].pair >> 16;
          const uint32_t right = s.ligaturePairs[li].pair & 0xFFFF;
          const uint32_t output = s.ligaturePairs[li].ligatureCp;
          bool hasLeft = false, hasRight = false, hasOutput = false;
          for (uint32_t i = 0; i < work.codepointCount; ++i) {
            hasLeft |= work.codepoints[i] == left;
            hasRight |= work.codepoints[i] == right;
            hasOutput |= work.codepoints[i] == output;
          }
          if (hasLeft && hasRight && !hasOutput) work.codepoints[work.codepointCount++] = output;
        }
        ++work.style;
        break;
      }
      case Phase::Style: {
        if (work.styleLimit == 0) work.styleLimit = work.codepointCount;
        const int result = preparePrewarmStyle(work);
        if (result != PREWARM_PENDING) finishPrewarmStyle(work, result);
        break;
      }
      case Phase::Glyphs: {
        const uint32_t mapIndex = work.readOrder[work.cursor];
        const int32_t index = work.mappings[mapIndex].globalIndex;
        const unsigned long start = millis();
        bool ok = true;
        if (index != work.lastReadIndex + 1) {
          ok = work.file.seekSet(s.glyphsFileOffset + static_cast<uint32_t>(index) * sizeof(EpdGlyph));
          ++stats_.seekCount;
        }
        if (ok) ok = work.file.read(&s.miniGlyphs[mapIndex], sizeof(EpdGlyph)) == sizeof(EpdGlyph);
        stats_.sdReadTimeMs += millis() - start;
        if (!ok) {
          LOG_ERR("SDCF", "Prewarm glyph read failed (style %u, glyph %ld)", work.style, static_cast<long>(index));
          freeStyleMiniData(s);
          finishPrewarmStyle(work, work.styleCodepointCount);
          break;
        }
        work.lastReadIndex = index;
        if (++work.cursor == work.validCount) work.phase = Phase::BitmapSetup;
        break;
      }
      case Phase::BitmapSetup: {
        uint32_t bitmapBytes = 0;
        if (work.styleMetadataOnly) {
          work.phase = Phase::Publish;
          break;
        }
        for (uint32_t i = 0; i < work.validCount; ++i) bitmapBytes += s.miniGlyphs[i].dataLength;
        s.measuredBytesPerGlyph = (bitmapBytes + work.validCount - 1) / work.validCount;
        if (!ensureArrayCapacity(s.miniBitmap, s.miniBitmapCapacity, bitmapBytes)) {
          LOG_ERR("SDCF", "Failed to allocate mini bitmap (%lu bytes)", static_cast<unsigned long>(bitmapBytes));
          freeStyleMiniData(s);
          work.rebuilding = false;
          work.unionCodepoints.reset();
          work.mappings.reset();
          work.readOrder.reset();
          uint32_t fit = work.styleLimit / 2;
          if (!work.retryingArena) {
            work.retryingArena = true;
            const uint32_t perGlyph = std::max<uint32_t>(1, s.measuredBytesPerGlyph);
            const uint32_t maxAlloc = ESP.getMaxAllocHeap();
            fit = maxAlloc > PREWARM_MAX_ALLOC_RESERVE ? (maxAlloc - PREWARM_MAX_ALLOC_RESERVE) / perGlyph : 0;
            fit = std::min(fit, work.codepointCount);
          }
          if (fit == 0) {
            finishPrewarmStyle(work, work.styleLimit);
          } else {
            work.styleLimit = fit;
            work.phase = Phase::Style;
          }
          break;
        }
        s.miniBitmapUsed = bitmapBytes;
        std::sort(work.readOrder.get(), work.readOrder.get() + work.validCount,
                  [&s](uint32_t a, uint32_t b) { return s.miniGlyphs[a].dataOffset < s.miniGlyphs[b].dataOffset; });
        work.cursor = 0;
        work.bitmapOffset = 0;
        work.lastBitmapEnd = UINT32_MAX;
        work.phase = Phase::Bitmaps;
        break;
      }
      case Phase::Bitmaps: {
        auto& glyph = s.miniGlyphs[work.readOrder[work.cursor]];
        if (glyph.dataLength != 0) {
          const uint32_t offset = s.bitmapFileOffset + glyph.dataOffset;
          const unsigned long start = millis();
          bool ok = true;
          if (offset != work.lastBitmapEnd) {
            ok = work.file.seekSet(offset);
            ++stats_.seekCount;
          }
          if (ok)
            ok = work.file.read(s.miniBitmap + work.bitmapOffset, glyph.dataLength) ==
                 static_cast<int>(glyph.dataLength);
          stats_.sdReadTimeMs += millis() - start;
          if (!ok) {
            LOG_ERR("SDCF", "Prewarm bitmap read failed (style %u)", work.style);
            freeStyleMiniData(s);
            finishPrewarmStyle(work, work.styleCodepointCount);
            break;
          }
          work.lastBitmapEnd = offset + glyph.dataLength;
        }
        glyph.dataOffset = work.bitmapOffset;
        work.bitmapOffset += glyph.dataLength;
        if (++work.cursor == work.validCount) work.phase = Phase::KernSetup;
        break;
      }
      case Phase::KernSetup: {
        // Glyph I/O is finished; kernel maps reuse its temporary memory headroom.
        work.mappings.reset();
        work.readOrder.reset();
        if (work.loadKernLig) {
          const int loaded = loadStyleKernLigatureSome(work);
          if (loaded == PREWARM_PENDING) break;
          work.phase = Phase::Publish;
          if (loaded == 0) work.kernOk = prepareMiniKern(work);
        } else {
          work.phase = Phase::Publish;
        }
        break;
      }
      case Phase::KernRows: {
        const auto& rows = *work.kern;
        const uint8_t oldLeft = rows.newToOldLeft[work.cursor];
        const uint32_t offset = s.kernMatrixFileOffset + (oldLeft - 1u) * s.header.kernRightClassCount;
        if (!work.file.seekSet(offset) ||
            work.file.read(work.kern->leftScratch, s.header.kernRightClassCount) != s.header.kernRightClassCount) {
          LOG_ERR("SDCF", "Failed to read kern row %u", oldLeft);
          freeStyleMiniKern(s);
          work.kernOk = false;
          work.phase = Phase::Publish;
          break;
        }
        int8_t* row = s.miniKernMatrix + (work.cursor - 1u) * rows.rightCount;
        for (uint16_t r = 1; r <= rows.rightCount; ++r)
          row[r - 1] = static_cast<int8_t>(rows.leftScratch[rows.newToOldRight[r] - 1]);
        if (++work.cursor > rows.leftCount) {
          s.miniKernLeftEntryCount = rows.leftEntries;
          s.miniKernRightEntryCount = rows.rightEntries;
          s.miniKernLeftClassCount = rows.leftCount;
          s.miniKernRightClassCount = rows.rightCount;
          work.phase = Phase::Publish;
        }
        break;
      }
      case Phase::Publish:
        publishPrewarmStyle(work);
        break;
    }
  }
  return PREWARM_PENDING;
}

// --- Cache management ---

void SdCardFont::clearCache() {
  cancelPrewarm();
  clearOverflow();
  // Note: advance table is intentionally preserved here. It persists across
  // layout passes so repeated section indexing amortizes SD reads. Use
  // clearPersistentCache() to wipe it.
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    resetStyleMiniData(styles_[i]);
    applyGlyphMissCallback(i);
  }
}

// --- Advance table ---

void SdCardFont::clearPersistentCache() {
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    delete[] advanceTable_[i];
    advanceTable_[i] = nullptr;
    advanceTableSize_[i] = 0;
  }
}

bool SdCardFont::advanceTableLookup(uint8_t styleIdx, uint32_t codepoint, uint16_t* outAdvance) const {
  const AdvanceEntry* table = advanceTable_[styleIdx];
  const uint32_t size = advanceTableSize_[styleIdx];
  if (!table || size == 0) return false;
  uint32_t lo = 0, hi = size;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (table[mid].codepoint < codepoint) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < size && table[lo].codepoint == codepoint) {
    if (outAdvance) *outAdvance = table[lo].advanceX;
    return true;
  }
  return false;
}

void SdCardFont::mergeIntoAdvanceTable(uint8_t styleIdx, const AdvanceEntry* sortedNew, uint32_t newCount) {
  if (newCount == 0) return;
  const uint32_t oldSize = advanceTableSize_[styleIdx];
  if (oldSize >= ADVANCE_CACHE_LIMIT) return;  // already full

  // Cap the merged size at ADVANCE_CACHE_LIMIT. Anything past the cap is
  // dropped from the tail of the sorted merge. Oversized requests use the
  // metric-only fallback for the remainder; later requests may replace the set.
  uint32_t mergedCap = oldSize + newCount;
  if (mergedCap > ADVANCE_CACHE_LIMIT) mergedCap = ADVANCE_CACHE_LIMIT;

  AdvanceEntry* merged = new (std::nothrow) AdvanceEntry[mergedCap];
  if (!merged) {
    LOG_ERR("SDCF", "mergeIntoAdvanceTable: alloc failed (%u entries) style %u", mergedCap, styleIdx);
    return;
  }

  const AdvanceEntry* a = advanceTable_[styleIdx];
  const AdvanceEntry* b = sortedNew;
  uint32_t i = 0, j = 0, k = 0;
  while (k < mergedCap && (i < oldSize || j < newCount)) {
    if (i < oldSize && (j >= newCount || a[i].codepoint <= b[j].codepoint)) {
      merged[k++] = a[i++];
    } else {
      merged[k++] = b[j++];
    }
  }

  delete[] advanceTable_[styleIdx];
  advanceTable_[styleIdx] = merged;
  advanceTableSize_[styleIdx] = k;
}

bool SdCardFont::hasAdvanceTable() const {
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (advanceTable_[i]) return true;
  }
  return false;
}

uint16_t SdCardFont::getAdvance(uint32_t codepoint, uint8_t style) const {
  style &= (MAX_STYLES - 1);
  if (!advanceTable_[style]) return 0;
  const AdvanceEntry* table = advanceTable_[style];
  const uint32_t size = advanceTableSize_[style];
  // Binary search sorted by codepoint
  uint32_t lo = 0, hi = size;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (table[mid].codepoint < codepoint) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < size && table[lo].codepoint == codepoint) {
    return table[lo].advanceX;
  }
  return 0;
}

uint16_t SdCardFont::getAdvanceOrLoad(uint32_t codepoint, uint8_t style) const {
  if (!loaded_) return 0;
  style = resolveStyle(style);
  uint16_t advance = 0;
  if (advanceTableLookup(style, codepoint, &advance)) return advance;  // includes valid zero advances
  const auto& s = styles_[style];
  if (!s.present) return 0;
  int32_t index = findGlobalGlyphIndex(s, codepoint);
  if (index < 0) index = findGlobalGlyphIndex(s, REPLACEMENT_GLYPH);
  if (index < 0) return 0;

  // Unlike getGlyph()/onGlyphMiss(), measuring text needs no bitmap allocation
  // or bitmap SD read. The 16-byte record lives on the caller's stack.
  HalFile file;
  EpdGlyph glyph{};
  if (!Storage.openFileForRead("SDCF", filePath_, file) ||
      !file.seekSet(s.glyphsFileOffset + static_cast<uint32_t>(index) * sizeof(EpdGlyph)) ||
      file.read(reinterpret_cast<uint8_t*>(&glyph), sizeof(glyph)) != sizeof(glyph)) {
    LOG_ERR("SDCF", "Failed to read advance for U+%04X style %u", codepoint, style);
    return 0;
  }
  return glyph.advanceX;
}

// Given a sorted array of unique codepoints, resolve glyph indices per style,
// batch-read advanceX from SD, and merge into the persistent advance table.
// Caller owns the codepoints buffer.
int SdCardFont::fetchAdvancesForCodepoints(uint32_t* codepoints, uint32_t cpCount, uint8_t styleMask) {
  int totalMissed = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
    const auto& s = styles_[si];

    // Keep repeated/subset requests warm, but never freeze the cache at the
    // first 768 characters in a book. Replace with the current paragraph when
    // its missing entries no longer fit alongside the old set.
    uint32_t missing = 0;
    for (uint32_t i = 0; i < cpCount; ++i) {
      if (!advanceTableLookup(si, codepoints[i], nullptr)) ++missing;
    }
    if (missing == 0) continue;
    const bool replaceCache = advanceTableSize_[si] + missing > ADVANCE_CACHE_LIMIT;

    // For each codepoint in `codepoints`, skip those already cached, then
    // resolve to a glyph index. Build a parallel array sorted by glyph index
    // for sequential SD reads.
    struct CpIdx {
      uint32_t codepoint;
      int32_t glyphIndex;
    };
    std::unique_ptr<CpIdx[]> mappings(new (std::nothrow) CpIdx[cpCount]);
    if (!mappings) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to allocate mappings for style %u", si);
      totalMissed += cpCount;
      continue;
    }

    uint32_t needCount = 0;
    uint32_t missedThisStyle = 0;
    const int32_t replacementIdx = findGlobalGlyphIndex(s, REPLACEMENT_GLYPH);
    for (uint32_t i = 0; i < cpCount; i++) {
      const uint32_t cp = codepoints[i];
      if (!replaceCache && advanceTableLookup(si, cp, nullptr)) continue;
      int32_t idx = findGlobalGlyphIndex(s, cp);
      if (idx < 0) {
        if (replacementIdx < 0) {
          missedThisStyle++;
          continue;
        }
        idx = replacementIdx;
      }
      mappings[needCount].codepoint = cp;
      mappings[needCount].glyphIndex = idx;
      needCount++;
    }
    totalMissed += static_cast<int>(missedThisStyle);

    if (needCount == 0) continue;

    // Sort by glyph index so SD reads are mostly sequential.
    std::sort(mappings.get(), mappings.get() + needCount,
              [](const CpIdx& a, const CpIdx& b) { return a.glyphIndex < b.glyphIndex; });

    // Open file once and read advanceX for each needed glyph.
    HalFile file;
    if (!Storage.openFileForRead("SDCF", filePath_, file)) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to open .cpfont for style %u", si);
      continue;
    }

    std::unique_ptr<AdvanceEntry[]> staged(new (std::nothrow) AdvanceEntry[needCount]);
    if (!staged) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to allocate staging for style %u", si);
      file.close();
      continue;
    }

    uint32_t fetched = 0;
    EpdGlyph tempGlyph;
    int32_t lastReadIndex = INT32_MIN;
    for (uint32_t i = 0; i < needCount; i++) {
      int32_t gIdx = mappings[i].glyphIndex;
      uint32_t fileOff = s.glyphsFileOffset + static_cast<uint32_t>(gIdx) * sizeof(EpdGlyph);
      if (gIdx != lastReadIndex + 1) {
        if (!file.seekSet(fileOff)) {
          LOG_ERR("SDCF", "buildAdvanceTable: failed to seek to glyph %d (style %u)", gIdx, si);
          break;
        }
      }
      if (file.read(reinterpret_cast<uint8_t*>(&tempGlyph), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
        LOG_ERR("SDCF", "buildAdvanceTable: short glyph read (style %u, glyph %d)", si, gIdx);
        break;
      }
      lastReadIndex = gIdx;
      staged[fetched].codepoint = mappings[i].codepoint;
      staged[fetched].advanceX = tempGlyph.advanceX;
      fetched++;
    }
    file.close();

    if (fetched > 0) {
      // Sort staged by codepoint, then merge into the persistent table.
      std::sort(staged.get(), staged.get() + fetched,
                [](const AdvanceEntry& a, const AdvanceEntry& b) { return a.codepoint < b.codepoint; });
      if (replaceCache) {
        // Do not evict usable entries until the replacement reads succeeded.
        delete[] advanceTable_[si];
        advanceTable_[si] = nullptr;
        advanceTableSize_[si] = 0;
      }
      mergeIntoAdvanceTable(si, staged.get(), fetched);
    }

    LOG_DBG("SDCF", "Advance table style %u: +%u from SD, total=%u/%u", si, fetched, advanceTableSize_[si],
            ADVANCE_CACHE_LIMIT);
  }

  return totalMissed;
}

template <typename Iter>
int SdCardFont::buildAdvanceTableRange(Iter begin, Iter end, bool includeSpace, bool includeHyphen, uint8_t styleMask,
                                       const char* extraText) {
  if (!loaded_) return -1;
  styleMask = resolveStyleMask(styleMask);
  if (styleMask == 0) return 0;

  unsigned long startMs = millis();

  // +2 reserved slots for space and hyphen injected after the main scan.
  static constexpr uint32_t MAX_UNIQUE_CODEPOINTS = 4096;
  uint32_t* codepoints = new (std::nothrow) uint32_t[MAX_UNIQUE_CODEPOINTS + 2];
  if (!codepoints) {
    LOG_ERR("SDCF", "buildAdvanceTable: failed to allocate codepoint buffer (%u bytes)", MAX_UNIQUE_CODEPOINTS * 4);
    return -1;
  }
  uint32_t cpCount = 0;
  bool hitCap = false;

  for (auto it = begin; it != end && !hitCap; ++it) {
    hitCap = collectUniqueCodepoints(asCStr(*it), codepoints, cpCount, MAX_UNIQUE_CODEPOINTS);
  }
  if (extraText && !hitCap) {
    hitCap = collectUniqueCodepoints(extraText, codepoints, cpCount, MAX_UNIQUE_CODEPOINTS);
  }

  if (includeSpace && std::none_of(codepoints, codepoints + cpCount, [](uint32_t c) { return c == ' '; }))
    codepoints[cpCount++] = ' ';
  if (includeHyphen && std::none_of(codepoints, codepoints + cpCount, [](uint32_t c) { return c == '-'; }))
    codepoints[cpCount++] = '-';

  if (hitCap) {
    LOG_ERR("SDCF", "buildAdvanceTable: unique codepoint cap (%u) hit, layout may be approximate",
            MAX_UNIQUE_CODEPOINTS);
  }
  std::sort(codepoints, codepoints + cpCount);
  int totalMissed = fetchAdvancesForCodepoints(codepoints, cpCount, styleMask);
  delete[] codepoints;
  stats_.prewarmTotalMs = millis() - startMs;
  return totalMissed;
}

int SdCardFont::buildAdvanceTable(const char* utf8Text, uint8_t styleMask, const char* extraText) {
  return buildAdvanceTableRange(&utf8Text, &utf8Text + 1, false, false, styleMask, extraText);
}

int SdCardFont::buildAdvanceTable(const std::deque<std::string>& words, bool includeHyphen, uint8_t styleMask,
                                  const char* extraText) {
  return buildAdvanceTableRange(words.begin(), words.end(), words.size() > 1, includeHyphen, styleMask, extraText);
}

// --- Stats ---

void SdCardFont::logStats(const char* label) {
  LOG_DBG("SDCF", "[%s] total=%ums sd_read=%ums seeks=%u glyphs=%u bitmap=%u bytes", label, stats_.prewarmTotalMs,
          stats_.sdReadTimeMs, stats_.seekCount, stats_.uniqueGlyphs, stats_.bitmapBytes);
}

void SdCardFont::resetStats() { stats_ = Stats{}; }

// --- Public accessors ---

EpdFont* SdCardFont::getEpdFont(uint8_t style) {
  style &= (MAX_STYLES - 1);
  if (!styles_[style].present) return nullptr;
  return &styles_[style].epdFont;
}

bool SdCardFont::hasStyle(uint8_t style) const { return styles_[style & (MAX_STYLES - 1)].present; }

uint8_t SdCardFont::resolveStyle(uint8_t style) const {
  static const uint8_t kFallbacks[MAX_STYLES][MAX_STYLES] = {
      // REGULAR: REGULAR -> BOLD -> ITALIC -> BOLD_ITALIC
      {EpdFontFamily::REGULAR, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::BOLD_ITALIC},
      // BOLD: BOLD -> REGULAR -> BOLD_ITALIC -> ITALIC
      {EpdFontFamily::BOLD, EpdFontFamily::REGULAR, EpdFontFamily::BOLD_ITALIC, EpdFontFamily::ITALIC},
      // ITALIC: ITALIC -> REGULAR -> BOLD_ITALIC -> BOLD
      {EpdFontFamily::ITALIC, EpdFontFamily::REGULAR, EpdFontFamily::BOLD_ITALIC, EpdFontFamily::BOLD},
      // BOLD_ITALIC: BOLD_ITALIC -> BOLD -> ITALIC -> REGULAR
      {EpdFontFamily::BOLD_ITALIC, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::REGULAR},
  };

  const uint8_t styleBits = style & (MAX_STYLES - 1);
  for (uint8_t candidate : kFallbacks[styleBits]) {
    if (styles_[candidate].present) return candidate;
  }
  return EpdFontFamily::REGULAR;
}

uint8_t SdCardFont::resolveStyleMask(uint8_t styleMask) const {
  uint8_t resolvedMask = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (styleMask & (1 << si)) {
      resolvedMask |= static_cast<uint8_t>(1u << resolveStyle(si));
    }
  }
  return resolvedMask;
}

// --- On-demand glyph loading (overflow buffer) ---

const EpdGlyph* SdCardFont::onGlyphMiss(void* ctx, uint32_t codepoint) {
  auto* oc = static_cast<OverflowContext*>(ctx);
  auto* self = oc->self;
  uint8_t styleIdx = oc->styleIdx;

  if (!self->loaded_ || styleIdx >= MAX_STYLES || !self->styles_[styleIdx].present) return nullptr;
  const auto& s = self->styles_[styleIdx];
  if (!s.fullIntervals && !s.bmpIntervals) return nullptr;

  // Check overflow cache first (matching both codepoint and style)
  for (uint32_t i = 0; i < self->overflowCount_; i++) {
    if (self->overflow_[i].codepoint == codepoint && self->overflow_[i].styleIdx == styleIdx) {
      return &self->overflow_[i].glyph;
    }
  }

  // Look up global glyph index via full intervals
  int32_t globalIdx = self->findGlobalGlyphIndex(s, codepoint);
  if (globalIdx < 0) return nullptr;

  // Pick overflow slot (ring buffer). Read into temporaries first so the
  // existing slot stays valid if SD I/O fails. Bookkeeping (count/next)
  // is deferred until after all I/O succeeds to avoid inconsistent state.
  uint32_t slot = self->overflowNext_;
  bool wasAtCapacity = (self->overflowCount_ == OVERFLOW_CAPACITY);

  // Read glyph metadata into temporary
  HalFile file;
  if (!Storage.openFileForRead("SDCF", self->filePath_, file)) {
    LOG_ERR("SDCF", "Overflow: failed to open .cpfont");
    return nullptr;
  }

  EpdGlyph tempGlyph = {};
  uint32_t glyphFileOff = s.glyphsFileOffset + static_cast<uint32_t>(globalIdx) * sizeof(EpdGlyph);
  if (!file.seekSet(glyphFileOff)) {
    LOG_ERR("SDCF", "Overflow: failed to seek to glyph for U+%04X style %u", codepoint, styleIdx);
    file.close();
    return nullptr;
  }
  if (file.read(reinterpret_cast<uint8_t*>(&tempGlyph), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
    LOG_ERR("SDCF", "Overflow: failed to read glyph metadata for U+%04X style %u", codepoint, styleIdx);
    return nullptr;
  }

  // Read bitmap data into temporary (if any)
  uint8_t* tempBitmap = nullptr;
  if (tempGlyph.dataLength > 0) {
    tempBitmap = new (std::nothrow) uint8_t[tempGlyph.dataLength];
    if (!tempBitmap) {
      LOG_ERR("SDCF", "Overflow: failed to allocate %u bytes for U+%04X bitmap", tempGlyph.dataLength, codepoint);
      return nullptr;
    }
    if (!file.seekSet(s.bitmapFileOffset + tempGlyph.dataOffset)) {
      LOG_ERR("SDCF", "Overflow: failed to seek to bitmap for U+%04X", codepoint);
      delete[] tempBitmap;
      file.close();
      return nullptr;
    }
    if (file.read(tempBitmap, tempGlyph.dataLength) != static_cast<int>(tempGlyph.dataLength)) {
      LOG_ERR("SDCF", "Overflow: failed to read bitmap for U+%04X", codepoint);
      delete[] tempBitmap;
      return nullptr;
    }
  }

  // All reads succeeded — commit to slot and advance ring buffer
  if (wasAtCapacity) {
    delete[] self->overflow_[slot].bitmap;
  } else {
    self->overflowCount_++;
  }
  self->overflowNext_ = (slot + 1) % OVERFLOW_CAPACITY;
  self->overflow_[slot].glyph = tempGlyph;
  self->overflow_[slot].bitmap = tempBitmap;
  self->overflow_[slot].codepoint = codepoint;
  self->overflow_[slot].styleIdx = styleIdx;

  LOG_DBG("SDCF", "Overflow: loaded U+%04X style %u on demand (slot %u/%u)", codepoint, styleIdx, slot,
          OVERFLOW_CAPACITY);

  return &self->overflow_[slot].glyph;
}

bool SdCardFont::isOverflowGlyph(const EpdGlyph* glyph) const {
  for (uint32_t i = 0; i < overflowCount_; i++) {
    if (&overflow_[i].glyph == glyph) return true;
  }
  return false;
}

const uint8_t* SdCardFont::getOverflowBitmap(const EpdGlyph* glyph) const {
  for (uint32_t i = 0; i < overflowCount_; i++) {
    if (&overflow_[i].glyph == glyph) {
      return overflow_[i].bitmap;
    }
  }
  return nullptr;
}

SdCardFont* SdCardFont::fromMissCtx(void* ctx) { return static_cast<OverflowContext*>(ctx)->self; }
