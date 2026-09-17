#include "Section.h"

#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>
#include <ZipFile.h>

#include "Epub/css/CssParser.h"
#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
// v28: text decoration bits now include line-through in serialized wordStyles.
// v29: TextBlock word data stored as one flat arena (offset table + NUL-terminated
// text blob) instead of length-prefixed strings and per-field arrays.
// v30: Arabic shaping changed both drawing and measurement (getTextAdvanceX now
//      measures the shaped visual text); cached word positions from v29 no longer
//      match what drawText renders.
// v32: ImageBlock serializes the book-internal source href after the cache path
//      (lazy extraction: images are header-probed at build time and extracted on
//      first render).
// v33: Support <ruby> and <rt> tags. Skip <rp> tags
// v34: Word gaps are only suppressed for tokens glued in the source, so spaces between
//      Hangul words survive again; ruby element boundaries carry the continuation flag
//      instead. Invalidates v33 caches, whose word positions have the spaces collapsed.

// v34: <br> handling changed layout — a <br> after text is now a margin-stripped
//      line break (browser-like) and only a <br> whose block stays empty injects
//      the scene-break gap, so cached pages laid out by older versions no longer
//      match. Keeps <br>-per-paragraph books (common CJK formatting) from
//      re-adding container spacing at every paragraph.
// v35: Persist a uint32_t visible-text start offset for every page.
// v36: Ruby and CJK justification layout changes invalidate cached word positions.
// v37: Footnote href records grew from 96 to 256 bytes.
// v38: Focus Reading line breaking changed — a visible hyphen/dash inside a word is now a
//      break opportunity, and hyphenation of a focus-split word considers the whole word
//      instead of only its regular-weight suffix. Pages cached by older versions were laid
//      out with the previous, more restrictive break set and no longer match.
// v39: Image top margin is clamped so a full-viewport-height image cannot
//      overflow the page bottom; older caches can hold placements that panels
//      with no bottom inset refuse to draw.
// v40: Ruby groups remain intact when a large text block is soft-flushed.
// v41: Simple HTML table rows are laid out as positioned columns instead of
//      flattened paragraphs with synthetic row/cell labels.
// v42: Closing a block strips inherited vertical margins and padding.
// v43: Paragraph base direction excludes direction changes from inline elements.
// v44: Persist internal-link rectangles with each page for touch navigation.
// v45: Internal EPUB links preserve CSS superscript/subscript positioning.
// v46: Pattern-free builds no longer use language tries for oversized-word breaks.
// v47: Synthetic Bold and SD differential rounding change glyph advances.
// v48: Korean character-wrap layout and its persisted render-spec flag.
// v49: Independent paragraph indent and once-per-paragraph first-line state.
constexpr uint8_t SECTION_FILE_VERSION = 49;
// Written into the version field while a build is in progress; patched to
// SECTION_FILE_VERSION only when the build is finalized. An abandoned /
// crash-interrupted .bin therefore carries version 0, which loadSectionFile rejects
// as unknown and clears -- so an incomplete file is never mistaken for a valid one.
constexpr uint8_t SECTION_FILE_INCOMPLETE_VERSION = 0;
// Written when a build is suspended partway (reader exited or device slept mid-build).
// The file carries valid pages 0..pageCount-1, all LUTs, and a trailer with the parse
// watermark (bytesConsumed, totalBytes) appended after the li LUT. loadSectionFile
// accepts it so a resume shows those pages instantly; the reader extends it by
// rebuilding in the background. Uses the same header layout as SECTION_FILE_VERSION,
// so finalized files are untouched by this feature; older firmware treats the sentinel
// as an unknown version and rebuilds, which is a safe downgrade.
// MUST change in lockstep with SECTION_FILE_VERSION: the sentinel IS the partial's
// format version, so a stale-format partial otherwise passes the header check and
// only fails (noisily, via the block-decode error path) when a page is loaded.
// Derived so the pairing can't be forgotten: 0xFE for v28, 0xFD for v29, ...
constexpr uint8_t SECTION_FILE_PARTIAL_VERSION = 0xFE - (SECTION_FILE_VERSION - 28);
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) +
                                 sizeof(uint8_t) + sizeof(bool) + sizeof(bool) + sizeof(bool) + sizeof(uint32_t) +
                                 sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
}  // namespace

// Out-of-line so the unique_ptr<ChapterHtmlSlimParser> in BuildContext can be
// constructed/destroyed where the parser's full definition is visible.
Section::Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
    : epub(epub),
      spineIndex(spineIndex),
      renderer(renderer),
      filePath(epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".bin") {}

// Suspend any in-progress build so every section.reset() / navigation / sleep path
// persists the pages already laid out as a partial .bin instead of discarding them
// (no-op once a build has completed or never started).
Section::~Section() { suspendBuild(); }

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", builtPageCount_);
    return 0;
  }

  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", builtPageCount_);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed", builtPageCount_);

  builtPageCount_++;
  // pageCount is the pages available to read: a rebuild over a partial only raises it
  // once it has laid out more pages than the partial already covers.
  if (builtPageCount_ > pageCount) {
    pageCount = builtPageCount_;
  }
  return position;
}

void Section::writeSectionFileHeader(const ReaderRenderSpec& spec) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_FILE_VERSION) + sizeof(spec.fontId) + sizeof(spec.lineCompression) +
                                   sizeof(spec.extraParagraphSpacing) + sizeof(spec.paragraphAlignment) +
                                   sizeof(spec.viewportWidth) + sizeof(spec.viewportHeight) + sizeof(pageCount) +
                                   sizeof(spec.hyphenationEnabled) + sizeof(spec.embeddedStyle) +
                                   sizeof(spec.imageRendering) + sizeof(spec.focusReadingEnabled) +
                                   sizeof(spec.characterWrap) + sizeof(spec.paragraphIndent) + sizeof(uint32_t) +
                                   sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t),
                "Header size mismatch");
  // Written as the incomplete sentinel; finalizeBuild() patches it to
  // SECTION_FILE_VERSION as the last step, committing the file.
  serialization::writePod(file, SECTION_FILE_INCOMPLETE_VERSION);
  serialization::writePod(file, spec.fontId);
  serialization::writePod(file, spec.lineCompression);
  serialization::writePod(file, spec.extraParagraphSpacing);
  serialization::writePod(file, spec.paragraphAlignment);
  serialization::writePod(file, spec.viewportWidth);
  serialization::writePod(file, spec.viewportHeight);
  serialization::writePod(file, spec.hyphenationEnabled);
  serialization::writePod(file, spec.embeddedStyle);
  serialization::writePod(file, spec.imageRendering);
  serialization::writePod(file, spec.focusReadingEnabled);
  serialization::writePod(file, spec.characterWrap);
  serialization::writePod(file, spec.paragraphIndent);
  serialization::writePod(file, pageCount);  // Placeholder for page count (will be initially 0, patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for anchor map offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for paragraph LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for li LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for visible-offset LUT (patched later)
}

bool Section::loadSectionFile(const ReaderRenderSpec& spec) {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  // Match parameters
  bool filePartial = false;
  {
    uint8_t version;
    serialization::readPod(file, version);
    if (version != SECTION_FILE_VERSION && version != SECTION_FILE_PARTIAL_VERSION) {
      // Explicit close() required: member variable persists beyond function scope
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Unknown version %u", version);
      clearCache();
      return false;
    }
    filePartial = (version == SECTION_FILE_PARTIAL_VERSION);

    int fileFontId;
    uint16_t fileViewportWidth, fileViewportHeight;
    float fileLineCompression;
    bool fileExtraParagraphSpacing;
    uint8_t fileParagraphAlignment;
    bool fileHyphenationEnabled;
    bool fileEmbeddedStyle;
    uint8_t fileImageRendering;
    bool fileFocusReadingEnabled;
    bool fileCharacterWrap;
    bool fileParagraphIndent;
    serialization::readPod(file, fileFontId);
    serialization::readPod(file, fileLineCompression);
    serialization::readPod(file, fileExtraParagraphSpacing);
    serialization::readPod(file, fileParagraphAlignment);
    serialization::readPod(file, fileViewportWidth);
    serialization::readPod(file, fileViewportHeight);
    serialization::readPod(file, fileHyphenationEnabled);
    serialization::readPod(file, fileEmbeddedStyle);
    serialization::readPod(file, fileImageRendering);
    serialization::readPod(file, fileFocusReadingEnabled);
    serialization::readPod(file, fileCharacterWrap);
    serialization::readPod(file, fileParagraphIndent);

    if (spec.fontId != fileFontId || spec.lineCompression != fileLineCompression ||
        spec.extraParagraphSpacing != fileExtraParagraphSpacing || spec.paragraphAlignment != fileParagraphAlignment ||
        spec.viewportWidth != fileViewportWidth || spec.viewportHeight != fileViewportHeight ||
        spec.hyphenationEnabled != fileHyphenationEnabled || spec.embeddedStyle != fileEmbeddedStyle ||
        spec.imageRendering != fileImageRendering || spec.focusReadingEnabled != fileFocusReadingEnabled ||
        spec.characterWrap != fileCharacterWrap || spec.paragraphIndent != fileParagraphIndent) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
      clearCache();
      return false;
    }
  }

  serialization::readPod(file, pageCount);

  if (filePartial) {
    // A partial's pageCount is the watermark of a suspended build. Read the watermark
    // trailer (appended after the visible-offset LUT) so estimatedTotalPages can extrapolate.
    uint32_t liLutOffset = 0;
    file.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
    serialization::readPod(file, liLutOffset);
    uint32_t visibleLutOffset = 0;
    file.seek(HEADER_SIZE - sizeof(uint32_t));
    serialization::readPod(file, visibleLutOffset);
    const uint32_t trailerOffset = visibleLutOffset + static_cast<uint32_t>(pageCount) * sizeof(uint32_t);
    const bool trailerValid = pageCount > 0 && liLutOffset >= HEADER_SIZE && visibleLutOffset > liLutOffset &&
                              trailerOffset + 2 * sizeof(uint32_t) <= file.size();
    if (!trailerValid) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: malformed partial section");
      clearCache();
      pageCount = 0;
      return false;
    }
    file.seek(trailerOffset);
    serialization::readPod(file, partialBytesConsumed_);
    serialization::readPod(file, partialTotalBytes_);
    partial_ = true;
    partialPageCount_ = pageCount;
  }

  // Explicit close() required: member variable persists beyond function scope
  file.close();
  LOG_DBG("SCT", "Deserialization succeeded: %d pages%s", pageCount, filePartial ? " (partial)" : "");
  return true;
}

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() const {
  const std::string tmpBin = binTmpPath();
  if (Storage.exists(tmpBin.c_str())) {
    Storage.remove(tmpBin.c_str());
  }
  if (!Storage.exists(filePath.c_str())) {
    LOG_DBG("SCT", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("SCT", "Cache cleared successfully");
  return true;
}

bool Section::createSectionFile(const ReaderRenderSpec& spec, const std::function<void()>& popupFn) {
  // One-shot build: start, then lay out the whole section in a single pass.
  if (!startBuild(spec, popupFn)) {
    return false;
  }
  if (!buildSomeMore(0)) {  // 0 = build to completion
    return false;
  }
  return buildComplete_;
}

bool Section::startBuild(const ReaderRenderSpec& spec, const std::function<void()>& popupFn) {
  if (build_) {
    LOG_ERR("SCT", "startBuild called while a build is already active");
    return false;
  }
  if (auto* fcm = renderer.getFontCacheManager()) fcm->clearCache();
  buildComplete_ = false;
  builtPageCount_ = 0;
  // Pages from a loaded partial stay readable (from filePath) while this build writes
  // to the tmp .bin, so availability never drops below the partial's watermark.
  pageCount = partial_ ? partialPageCount_ : 0;

  // Remove a stale tmp .bin from a crash-interrupted build; this build recreates it.
  {
    const std::string staleTmp = binTmpPath();
    if (Storage.exists(staleTmp.c_str())) {
      Storage.remove(staleTmp.c_str());
    }
  }

  build_ = makeUniqueNoThrow<BuildContext>();
  if (!build_) {
    LOG_ERR("SCT", "OOM: BuildContext");
    return false;
  }
  auto& work = *build_;
  work.spec = spec;
  work.popupFn = popupFn;
  const auto localPath = epub->getSpineItem(spineIndex).href;
  work.sourcePath = FsHelpers::normalisePath(localPath);
  const auto htmlDir = epub->getCachePath() + "/html";
  work.htmlPath = htmlDir + "/" + std::to_string(spineIndex) + ".html";
  work.tmpHtmlPath = htmlDir + "/.tmp_" + std::to_string(spineIndex) + ".html";
  const auto sectionsDir = epub->getCachePath() + "/sections";
  Storage.mkdir(sectionsDir.c_str());
  work.reusedHtml = Storage.exists(work.htmlPath.c_str());
  work.parsePath = work.reusedHtml ? work.htmlPath : work.tmpHtmlPath;
  const size_t lastSlash = localPath.find_last_of('/');
  work.contentBase = lastSlash == std::string::npos ? "" : localPath.substr(0, lastSlash + 1);
  work.imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";
  if (work.reusedHtml) {
    LOG_DBG("SCT", "Reusing cached HTML %s", work.htmlPath.c_str());
    if (!beginLayout()) {
      suspendBuild();
      return false;
    }
  } else {
    Storage.mkdir(htmlDir.c_str());
    work.phase = BuildContext::Phase::ExtractBegin;
  }
  return true;
}

bool Section::extractHtmlSome(const bool drain) {
  auto& work = *build_;
  const auto retry = [&work]() {
    work.htmlStream.reset();
    work.htmlOutput = HalFile{};
    Storage.remove(work.tmpHtmlPath.c_str());
    if (work.extractAttempts == 3) {
      LOG_ERR("SCT", "Failed to extract HTML after retries");
      return false;
    }
    work.phase = BuildContext::Phase::ExtractBegin;
    return true;
  };
  ZipFile::ReadStatus status;
  if (work.phase == BuildContext::Phase::ExtractBegin) {
    if (work.extractAttempts > 0) delay(50);
    ++work.extractAttempts;
    work.htmlStream = makeUniqueNoThrow<ZipFile>(epub->getPath());
    if (!work.htmlStream || !Storage.openFileForWrite("SCT", work.tmpHtmlPath, work.htmlOutput)) {
      LOG_ERR("SCT", "Failed to start HTML extraction");
      return retry();
    }
    if (drain) {
      // The complete-build call retains its caller's framebuffer loan until this drain returns.
      status = work.htmlStream->readFileToStream(work.sourcePath.c_str(), work.htmlOutput, 8192)
                   ? ZipFile::ReadStatus::Done
                   : ZipFile::ReadStatus::Error;
    } else {
      if (!work.htmlStream->beginReadFileToStream(work.sourcePath.c_str(), 8192)) return retry();
      work.phase = BuildContext::Phase::Extract;
      return true;
    }
  } else {
    status = work.htmlStream->readSome(work.htmlOutput);
  }
  if (status == ZipFile::ReadStatus::More) return true;
  if (status == ZipFile::ReadStatus::Error) return retry();
  work.htmlStream.reset();
  work.htmlOutput = HalFile{};
  // Only a complete extracted file is visible at the persistent cache path.
  work.reusedHtml = Storage.rename(work.tmpHtmlPath.c_str(), work.htmlPath.c_str());
  work.parsePath = work.reusedHtml ? work.htmlPath : work.tmpHtmlPath;
  if (!work.reusedHtml) LOG_DBG("SCT", "Failed to promote HTML cache; parsing from temp");
  work.phase = BuildContext::Phase::StartParse;
  return true;
}

bool Section::beginLayout() {
  auto* ctx = build_.get();
  const auto& spec = ctx->spec;
  const auto& popupFn = ctx->popupFn;
  ctx->phase = BuildContext::Phase::Parse;
  std::string{}.swap(ctx->sourcePath);
  if (!Storage.openFileForWrite("SCT", binTmpPath(), file)) return false;
  writeSectionFileHeader(spec);
  if (spec.embeddedStyle) {
    ctx->cssParser = epub->getCssParser();
    if (ctx->cssParser) {
      const CssParser::CacheLoadResult cacheResult = ctx->cssParser->loadFromCache();
      if (cacheResult == CssParser::CacheLoadResult::LowMemory) {
        LOG_ERR("SCT", "Insufficient heap to hydrate CSS; section build deferred");
        ctx->cssParser->clear();
        return false;
      }
      if (cacheResult == CssParser::CacheLoadResult::Invalid) {
        LOG_ERR("SCT", "Failed to load CSS from cache");
      }
    }
  }

  // Collect TOC anchors for this spine so the parser can insert page breaks at chapter boundaries
  std::vector<std::string> tocAnchors;
  const int startTocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  if (startTocIndex >= 0) {
    for (int i = startTocIndex; i < epub->getTocItemsCount(); i++) {
      auto entry = epub->getTocItem(i);
      if (entry.spineIndex != spineIndex) break;
      if (!entry.anchor.empty()) {
        tocAnchors.push_back(std::move(entry.anchor));
      }
    }
  }

  // The parser stores the path/contentBase/imageBasePath by reference, so they must
  // live in the BuildContext (which outlives the parser). The page-complete callback
  // captures the BuildContext pointer to append to its in-RAM LUT; build_ owns the
  // context for the parser's whole lifetime.
  BuildContext* ctxPtr = ctx;
  ctx->parser = makeUniqueNoThrow<ChapterHtmlSlimParser>(
      epub, ctxPtr->parsePath, renderer, spec.fontId, spec.lineCompression, spec.extraParagraphSpacing,
      spec.paragraphAlignment, spec.viewportWidth, spec.viewportHeight, spec.hyphenationEnabled,
      spec.focusReadingEnabled,
      [this, ctxPtr](std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex,
                     const uint32_t visibleTextOffset) {
        ctxPtr->lut.push_back(
            {this->onPageComplete(std::move(page)), paragraphIndex, listItemIndex, visibleTextOffset});
      },
      spec.embeddedStyle, ctxPtr->contentBase, ctxPtr->imageBasePath, spec.imageRendering, std::move(tocAnchors),
      popupFn, ctxPtr->cssParser, spec.characterWrap, spec.paragraphIndent);
  if (!ctx->parser) {
    LOG_ERR("SCT", "OOM: ChapterHtmlSlimParser");
    return false;
  }

  ctx->popupFn = nullptr;
  Hyphenator::setPreferredLanguage(epub->getLanguage());
  if (!ctx->parser->beginParse()) {
    LOG_ERR("SCT", "Failed to begin parse");
    return false;
  }
  ctx->totalBytes = ctx->parser->parseTotalBytes();
  return true;
}

bool Section::hasRetainedBuildOperation() const {
  if (!build_) return false;
  switch (build_->phase) {
    case BuildContext::Phase::ExtractBegin:
    case BuildContext::Phase::StartParse:
      return false;
    case BuildContext::Phase::Parse:
      return build_->parser->hasPendingBlock();
    default:
      return true;
  }
}

bool Section::buildSomeMore(const int maxPages, const int maxParseSteps) {
  if (!build_) {
    LOG_ERR("SCT", "buildSomeMore with no active build");
    return false;
  }
  // Pace on pages laid out by THIS build, not pageCount: during a rebuild over a partial,
  // pageCount stays pinned at the partial's watermark until the build passes it, which
  // would otherwise turn one "small" chunk into a blocking rebuild of the whole watermark.
  const int startCount = builtPageCount_;
  for (int steps = 0;; ++steps) {
    if (build_->phase == BuildContext::Phase::ExtractBegin || build_->phase == BuildContext::Phase::Extract) {
      if (!extractHtmlSome(maxPages <= 0 && maxParseSteps <= 0)) {
        suspendBuild();
        return false;
      }
    } else if (build_->phase == BuildContext::Phase::StartParse) {
      if (!beginLayout()) {
        suspendBuild();
        return false;
      }
    } else if (build_->phase != BuildContext::Phase::Parse) {
      if (!finalizeBuild()) return false;
    } else {
      const auto status = build_->parser->parseStep();
      if (status == ChapterHtmlSlimParser::ParseStatus::Error) {
        LOG_ERR("SCT", "Parse error during incremental build");
        abandonBuild();
        return false;
      }
      if (status == ChapterHtmlSlimParser::ParseStatus::Done && !finalizeBuild()) return false;
    }
    if (!build_) return true;
    // ParseStatus::More: yield once we've laid out the requested number of pages.
    if ((maxPages > 0 && (builtPageCount_ - startCount) >= maxPages) ||
        (maxParseSteps > 0 && steps + 1 >= maxParseSteps)) {
      if (build_->parser) build_->bytesConsumed = build_->parser->parseBytesConsumed();
      return true;
    }
  }
}

bool Section::hasHtmlCache() const {
  const std::string htmlPath = epub->getCachePath() + "/html/" + std::to_string(spineIndex) + ".html";
  return Storage.exists(htmlPath.c_str());
}

std::optional<uint16_t> Section::findAnchorDuringBuild(const std::string& anchor) const {
  if (!build_ || !build_->parser) return std::nullopt;
  for (const auto& [key, page] : build_->parser->getAnchors()) {
    if (key == anchor) return page;
  }
  return std::nullopt;
}

std::optional<uint16_t> Section::findAnchor(const std::string& anchor) const {
  if (const auto page = findAnchorDuringBuild(anchor)) {
    return page;
  }
  // Fall back to the on-disk anchor map: a finalized section, or a partial whose map
  // covers everything up to its watermark (nullopt past it -- build further and retry).
  return getPageForAnchor(anchor);
}

uint16_t Section::estimatedTotalPages() const {
  // Extrapolation from a suspended session's watermark trailer. A static snapshot, so no EMA
  // damping is needed. Also the best guess while a rebuild is running but hasn't laid out
  // enough pages yet to extrapolate from its own progress.
  const auto partialEstimate = [this]() -> uint16_t {
    if (!partial_ || partialBytesConsumed_ == 0 || partialTotalBytes_ <= partialBytesConsumed_) {
      return pageCount;
    }
    const uint64_t est = static_cast<uint64_t>(partialPageCount_) * partialTotalBytes_ / partialBytesConsumed_;
    if (est <= pageCount) return pageCount;
    return est > 60000 ? 60000 : static_cast<uint16_t>(est);
  };

  if (!build_) {
    return partial_ ? partialEstimate() : pageCount;  // partial -> extrapolate, finalized -> exact
  }
  const uint32_t consumed = build_->bytesConsumed;
  const uint32_t total = build_->totalBytes;
  if (builtPageCount_ == 0 || consumed == 0 || total <= consumed) return partialEstimate();

  // Raw extrapolation: scale the pages built so far by the fraction of HTML still unparsed. This
  // re-derives from a growing, non-uniform sample, so it jitters up and down as the build crosses
  // dense vs sparse regions of the chapter.
  const uint64_t raw = static_cast<uint64_t>(builtPageCount_) * total / consumed;

  // Damp that jitter with an exponential moving average. Step it once per build advance (keyed on
  // bytesConsumed) rather than per status-bar redraw, so the smoothing rate doesn't depend on how
  // often we repaint. As the build nears the end, consumed -> total and raw -> the built count, so
  // the average settles onto the true count (and finalizeBuild then returns the exact pageCount).
  constexpr float ALPHA = 0.25f;  // weight of each new sample; lower = steadier but slower to settle
  if (build_->smoothedEstimate <= 0) {
    build_->smoothedEstimate = static_cast<float>(raw);  // seed on the first estimate
  } else if (consumed != build_->smoothedAtConsumed) {
    build_->smoothedEstimate += ALPHA * (static_cast<float>(raw) - build_->smoothedEstimate);
  }
  build_->smoothedAtConsumed = consumed;

  const uint64_t est = static_cast<uint64_t>(build_->smoothedEstimate + 0.5f);
  if (est <= pageCount) return pageCount;  // never fewer than the pages already available
  return est > 60000 ? 60000 : static_cast<uint16_t>(est);
}

// Write the LUTs and anchor map into the open tmp .bin, patch the header with the built
// page count and table offsets, stamp `version` as the commit point, then swap the tmp
// file over filePath. For SECTION_FILE_PARTIAL_VERSION a watermark trailer
// (bytesConsumed, totalBytes) is appended after the li LUT so a later open can estimate
// the total page count. The parser must still be alive (anchors are read from it).
// On failure the tmp is removed and any pre-existing file at filePath is left intact.
void Section::beginCommit(const uint8_t version) {
  build_->phase = BuildContext::Phase::PageLut;
  build_->commitCursor = 0;
  build_->anchorCount = 0;
  build_->commitVersion = version;
  build_->tableOffsets[0] = file.position();
}

Section::CommitStatus Section::commitSome(uint16_t maxUnits) {
  auto& work = *build_;
  using Phase = BuildContext::Phase;
  const bool asPartial = work.commitVersion == SECTION_FILE_PARTIAL_VERSION;
  const auto& anchors = work.parser->getAnchors();
  const auto failCommit = [this]() {
    LOG_ERR("SCT", "Failed to commit section tables");
    file = HalFile{};
    Storage.remove(binTmpPath().c_str());
    return CommitStatus::Error;
  };

  while (maxUnits-- != 0) {
    size_t expectedPosition = file.position();
    switch (work.phase) {
      case Phase::PageLut:
        if (work.commitCursor < work.lut.size()) {
          const auto offset = work.lut[work.commitCursor++].fileOffset;
          if (offset == 0) return failCommit();
          serialization::writePod(file, offset);
          expectedPosition += sizeof(offset);
        } else {
          work.tableOffsets[1] = file.position();
          work.commitCursor = 0;
          work.phase = Phase::CountAnchors;
        }
        break;
      case Phase::CountAnchors:
        if (work.commitCursor < anchors.size()) {
          if (!asPartial || anchors[work.commitCursor].second < builtPageCount_) ++work.anchorCount;
          ++work.commitCursor;
        } else {
          serialization::writePod(file, work.anchorCount);
          expectedPosition += sizeof(work.anchorCount);
          work.commitCursor = 0;
          work.phase = Phase::Anchors;
        }
        break;
      case Phase::Anchors:
        if (work.commitCursor < anchors.size()) {
          const auto& [anchor, page] = anchors[work.commitCursor++];
          if (!asPartial || page < builtPageCount_) {
            serialization::writeString(file, anchor);
            serialization::writePod(file, page);
            expectedPosition += sizeof(uint32_t) + anchor.size() + sizeof(page);
          }
        } else {
          work.tableOffsets[2] = file.position();
          serialization::writePod(file, static_cast<uint16_t>(work.lut.size()));
          expectedPosition += sizeof(uint16_t);
          work.commitCursor = 0;
          work.phase = Phase::ParagraphLut;
        }
        break;
      case Phase::ParagraphLut:
        if (work.commitCursor < work.lut.size()) {
          serialization::writePod(file, work.lut[work.commitCursor++].paragraphIndex);
          expectedPosition += sizeof(uint16_t);
        } else {
          work.tableOffsets[3] = file.position();
          work.commitCursor = 0;
          work.phase = Phase::ListLut;
        }
        break;
      case Phase::ListLut:
        if (work.commitCursor < work.lut.size()) {
          serialization::writePod(file, work.lut[work.commitCursor++].listItemIndex);
          expectedPosition += sizeof(uint16_t);
        } else {
          work.tableOffsets[4] = file.position();
          work.commitCursor = 0;
          work.phase = Phase::VisibleLut;
        }
        break;
      case Phase::VisibleLut:
        if (work.commitCursor < work.lut.size()) {
          serialization::writePod(file, work.lut[work.commitCursor++].visibleTextOffset);
          expectedPosition += sizeof(uint32_t);
        } else {
          work.phase = Phase::Trailer;
        }
        break;
      case Phase::Trailer:
        if (asPartial) {
          serialization::writePod(file, work.bytesConsumed);
          serialization::writePod(file, work.totalBytes);
          expectedPosition += sizeof(uint32_t) * 2;
        }
        work.phase = Phase::Header;
        break;
      case Phase::Header:
        expectedPosition = HEADER_SIZE - sizeof(work.tableOffsets) - sizeof(builtPageCount_);
        if (!file.seek(expectedPosition)) return failCommit();
        serialization::writePod(file, builtPageCount_);
        for (const auto offset : work.tableOffsets) serialization::writePod(file, offset);
        expectedPosition = HEADER_SIZE;
        work.phase = Phase::Version;
        break;
      case Phase::Version:
        if (!file.seek(0)) return failCommit();
        serialization::writePod(file, work.commitVersion);
        expectedPosition = sizeof(work.commitVersion);
        work.phase = Phase::Publish;
        break;
      case Phase::Publish:
        // No yield between closing the committed tmp and swapping it into place.
        file = HalFile{};
        if (Storage.exists(filePath.c_str()) && !Storage.remove(filePath.c_str())) return failCommit();
        if (!Storage.rename(binTmpPath().c_str(), filePath.c_str())) return failCommit();
        return CommitStatus::Done;
      case Phase::ExtractBegin:
      case Phase::Extract:
      case Phase::StartParse:
      case Phase::Parse:
        return failCommit();
    }
    // Serialization writes return no status; a short write must not publish a valid version.
    if (file.position() != expectedPosition) return failCommit();
  }
  return CommitStatus::More;
}

bool Section::commitBuildFile(const uint8_t version, const uint32_t bytesConsumed, const uint32_t totalBytes) {
  build_->bytesConsumed = bytesConsumed;
  build_->totalBytes = totalBytes;
  beginCommit(version);
  CommitStatus status;
  do {
    status = commitSome(UINT16_MAX);
  } while (status == CommitStatus::More);
  return status == CommitStatus::Done;
}

bool Section::finalizeBuild() {
  if (build_->phase == BuildContext::Phase::Parse) {
    if (!build_->parser->finishParse()) {
      abandonBuild();
      return false;
    }
    if (!build_->reusedHtml) {
      // Promote parsed HTML so later builds can reuse the extracted source.
      if (!Storage.rename(build_->tmpHtmlPath.c_str(), build_->htmlPath.c_str())) {
        LOG_DBG("SCT", "Failed to promote HTML cache, removing temp");
        Storage.remove(build_->tmpHtmlPath.c_str());
      }
    }
    beginCommit(SECTION_FILE_VERSION);
  }

  // A work-count bound, not a target time guarantee. Table entries retain their cursor.
  const CommitStatus status = commitSome(32);
  if (status == CommitStatus::More) return true;
  if (build_->cssParser) build_->cssParser->clear();
  build_.reset();
  if (status == CommitStatus::Error) {
    if (!Storage.exists(filePath.c_str())) {
      partial_ = false;
      partialPageCount_ = 0;
    }
    pageCount = partial_ ? partialPageCount_ : 0;
    builtPageCount_ = 0;
    return false;
  }
  buildComplete_ = true;
  partial_ = false;
  partialPageCount_ = 0;
  pageCount = builtPageCount_;
  return true;
}

void Section::suspendBuild() {
  if (!build_) return;
  // All pages are already complete once table commit starts. Finish that same
  // cursor before destroying its owner; do not append a second partial footer.
  if (build_->parser && build_->phase != BuildContext::Phase::Parse) {
    while (build_) {
      if (!finalizeBuild()) break;
    }
    return;
  }

  // Only worth persisting if this build produced pages a pre-existing partial doesn't
  // already cover; otherwise keep the older (bigger) partial and just drop the tmp.
  const bool worthKeeping = builtPageCount_ > 0 && (!partial_ || builtPageCount_ > partialPageCount_);

  bool committed = false;
  if (worthKeeping) {
    // Capture the parse watermark and commit before tearing the parser down (the anchor
    // map is read from it). The incomplete trailing page is intentionally not flushed:
    // only fully laid-out pages are persisted, and the rebuild re-derives the rest.
    const uint32_t consumed = static_cast<uint32_t>(build_->parser->parseBytesConsumed());
    committed = commitBuildFile(SECTION_FILE_PARTIAL_VERSION, consumed, build_->totalBytes);
    if (committed) {
      partial_ = true;
      partialPageCount_ = builtPageCount_;
      partialBytesConsumed_ = consumed;
      partialTotalBytes_ = build_->totalBytes;
      LOG_INF("SCT", "Suspended build: %u pages persisted", builtPageCount_);
    }
  }

  if (build_->parser) build_->parser->abortParse();
  if (build_->cssParser) build_->cssParser->clear();
  if (!committed && file) {
    // Explicit close() required before remove (member variable, O_RDWR handle).
    file.close();
    Storage.remove(binTmpPath().c_str());
  }
  build_->htmlStream.reset();
  build_->htmlOutput = HalFile{};
  if (!build_->reusedHtml && Storage.exists(build_->tmpHtmlPath.c_str())) {
    Storage.remove(build_->tmpHtmlPath.c_str());
  }
  build_.reset();
  buildComplete_ = false;
  pageCount = partial_ ? partialPageCount_ : 0;
  builtPageCount_ = 0;
}

void Section::abandonBuild() {
  if (!build_) return;
  if (build_->parser) build_->parser->abortParse();
  if (build_->cssParser) build_->cssParser->clear();
  if (file) {
    // Explicit close() required before remove (member variable, O_RDWR handle).
    file.close();
    Storage.remove(binTmpPath().c_str());
  }
  // A parse error would recur against the same HTML, so drop any partial too -- resuming
  // from it would just re-enter the failing build every open.
  if (Storage.exists(filePath.c_str())) {
    Storage.remove(filePath.c_str());
  }
  build_->htmlStream.reset();
  build_->htmlOutput = HalFile{};
  if (!build_->reusedHtml && Storage.exists(build_->tmpHtmlPath.c_str())) {
    Storage.remove(build_->tmpHtmlPath.c_str());
  }
  build_.reset();
  buildComplete_ = false;
  partial_ = false;
  partialPageCount_ = 0;
  pageCount = 0;
  builtPageCount_ = 0;
}

std::unique_ptr<Page> Section::loadPageDuringBuild(const int page) {
  if (!build_ || page < 0 || page >= static_cast<int>(build_->lut.size()) || !file) {
    return nullptr;
  }
  const uint32_t pos = build_->lut[page].fileOffset;
  if (pos == 0) {
    return nullptr;
  }
  // The .bin is open O_RDWR for the build. Read the already-written page, then restore
  // the write cursor so the next onPageComplete keeps appending where it left off.
  const uint32_t writePos = file.position();
  file.seek(pos);
  auto p = Page::deserialize(file);
  file.seek(writePos);
  if (p) {
    p->visibleTextOffset = build_->lut[page].visibleTextOffset;
  }
  return p;
}

// Read a page from the committed file at filePath (finalized section or partial from a
// previous session). Uses a local handle so it is safe while a build holds the member
// `file` open on the tmp .bin.
std::unique_ptr<Page> Section::loadPageAt(const int page) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return nullptr;
  }

  f.seek(HEADER_SIZE - sizeof(uint32_t) * 5);
  uint32_t lutOffset;
  serialization::readPod(f, lutOffset);
  f.seek(lutOffset + sizeof(uint32_t) * page);
  uint32_t pagePos;
  serialization::readPod(f, pagePos);

  // Read this page's visible-codepoint start offset from the visible-offset LUT (last header slot)
  // in the same open handle, so the reader can persist progress without reopening the section file
  // on every page turn (see Page::visibleTextOffset). A malformed/old file leaves it at 0.
  f.seek(HEADER_SIZE - sizeof(uint32_t));
  uint32_t visibleLutOffset;
  serialization::readPod(f, visibleLutOffset);
  uint32_t visibleTextOffset = 0;
  const uint32_t visibleEntry = visibleLutOffset + sizeof(uint32_t) * page;
  if (visibleLutOffset >= HEADER_SIZE && visibleEntry + sizeof(uint32_t) <= f.size()) {
    f.seek(visibleEntry);
    serialization::readPod(f, visibleTextOffset);
  }

  f.seek(pagePos);
  auto p = Page::deserialize(f);
  if (p) {
    p->visibleTextOffset = visibleTextOffset;
  }
  return p;
  // No f.close() needed -- DESTRUCTOR_CLOSES_FILE=1 handles it at scope exit
}

std::unique_ptr<Page> Section::loadPage(const int page) {
  if (page < 0) {
    return nullptr;
  }
  if (build_ && page < static_cast<int>(build_->lut.size())) {
    return loadPageDuringBuild(page);
  }
  // Not (yet) in the active build: serve from the file on disk -- a finalized section,
  // or a partial from a previous session whose pages the rebuild hasn't reached again.
  const int onDisk = partial_ ? partialPageCount_ : (build_ ? 0 : pageCount);
  if (page >= onDisk) {
    return nullptr;
  }
  return loadPageAt(page);
}

std::string Section::getTextFromSectionFile() {
  std::string fullText;
  auto p = loadPage(currentPage);
  if (p) {
    for (const auto& el : p->elements) {
      if (el->getTag() == TAG_PageLine) {
        const auto& line = static_cast<const PageLine&>(*el);
        if (line.getBlock()) {
          const auto& block = *line.getBlock();
          for (uint16_t i = 0; i < block.wordCount(); i++) {
            if (!fullText.empty()) fullText += " ";
            fullText += block.wordText(i);
          }
        }
      }
    }
  }
  return fullText;
}

std::optional<uint16_t> Section::getCachedPageCount() const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (fileSize < HEADER_SIZE) {
    return std::nullopt;
  }

  // Only a finalized section's count is the chapter total; a partial's count is just the
  // suspended build's watermark, which would skew progress mapping. Callers fall back to
  // their own estimates.
  uint8_t version;
  serialization::readPod(f, version);
  if (version != SECTION_FILE_VERSION) {
    return std::nullopt;
  }

  f.seek(HEADER_SIZE - sizeof(uint32_t) * 5 - sizeof(uint16_t));
  uint16_t count;
  serialization::readPod(f, count);
  return count;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 4);
  uint32_t anchorMapOffset;
  serialization::readPod(f, anchorMapOffset);
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(anchorMapOffset);
  uint16_t count;
  serialization::readPod(f, count);
  for (uint16_t i = 0; i < count; i++) {
    std::string key;
    uint16_t page;
    serialization::readString(f, key);
    serialization::readPod(f, page);
    if (key == anchor) {
      return page;
    }
  }

  return std::nullopt;
}

std::optional<uint16_t> Section::getPageForParagraphIndex(const uint16_t pIndex) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 3);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = paragraphLutOffset + sizeof(uint16_t) + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pagePIdx;
    serialization::readPod(f, pagePIdx);
    if (pagePIdx >= pIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint16_t> Section::getParagraphIndexForPage(const uint16_t page) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 3);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0 || page >= count) {
    return std::nullopt;
  }

  const uint32_t entryEnd = paragraphLutOffset + sizeof(uint16_t) + (page + 1) * sizeof(uint16_t);
  if (entryEnd > fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset + sizeof(uint16_t) + page * sizeof(uint16_t));
  uint16_t pIdx;
  serialization::readPod(f, pIdx);
  return pIdx;
}

std::optional<uint16_t> Section::getPageForListItemIndex(const uint16_t liIndex) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
  uint32_t liLutOffset;
  serialization::readPod(f, liLutOffset);
  if (liLutOffset == 0 || liLutOffset >= fileSize) {
    return std::nullopt;
  }

  // The li LUT shares count with the paragraph LUT; read count from paragraphLutOffset
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 3);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = liLutOffset + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  f.seek(liLutOffset);
  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pageLiIdx;
    serialization::readPod(f, pageLiIdx);
    if (pageLiIdx >= liIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint32_t> Section::getVisibleTextOffsetForPage(const uint16_t page) const {
  if (build_ && page < build_->lut.size()) {
    return build_->lut[page].visibleTextOffset;
  }

  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f) || f.size() < HEADER_SIZE) {
    return std::nullopt;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != SECTION_FILE_VERSION && version != SECTION_FILE_PARTIAL_VERSION) {
    return std::nullopt;
  }

  f.seek(HEADER_SIZE - sizeof(uint32_t) * 5 - sizeof(uint16_t));
  uint16_t count;
  serialization::readPod(f, count);
  if (page >= count) {
    return std::nullopt;
  }

  f.seek(HEADER_SIZE - sizeof(uint32_t));
  uint32_t visibleLutOffset;
  serialization::readPod(f, visibleLutOffset);
  const uint32_t entryOffset = visibleLutOffset + static_cast<uint32_t>(page) * sizeof(uint32_t);
  if (visibleLutOffset < HEADER_SIZE || entryOffset + sizeof(uint32_t) > f.size()) {
    return std::nullopt;
  }

  f.seek(entryOffset);
  uint32_t result;
  serialization::readPod(f, result);
  return result;
}

std::optional<uint16_t> Section::getPageForVisibleTextOffset(const uint32_t offset,
                                                             const bool preferFirstAtOffset) const {
  const auto findInEntries = [offset, preferFirstAtOffset](const auto& entries) -> std::optional<uint16_t> {
    if (entries.empty()) return std::nullopt;
    uint16_t result = 0;
    for (size_t i = 0; i < entries.size(); i++) {
      const uint32_t pageStart = entries[i].visibleTextOffset;
      if (preferFirstAtOffset && pageStart == offset) {
        return static_cast<uint16_t>(i);
      }
      if (pageStart > offset) break;
      result = static_cast<uint16_t>(i);
    }
    return result;
  };

  if (build_ && !build_->lut.empty()) {
    // Resolve within the active build's known range. Later offsets may still be
    // covered by an on-disk partial that the resumed build has not reached yet.
    if (offset <= build_->lut.back().visibleTextOffset) {
      return findInEntries(build_->lut);
    }
  }

  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f) || f.size() < HEADER_SIZE) {
    return std::nullopt;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != SECTION_FILE_VERSION && version != SECTION_FILE_PARTIAL_VERSION) {
    return std::nullopt;
  }
  const bool partial = version == SECTION_FILE_PARTIAL_VERSION;

  f.seek(HEADER_SIZE - sizeof(uint32_t) * 5 - sizeof(uint16_t));
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0) {
    return std::nullopt;
  }

  f.seek(HEADER_SIZE - sizeof(uint32_t));
  uint32_t visibleLutOffset;
  serialization::readPod(f, visibleLutOffset);
  if (visibleLutOffset < HEADER_SIZE || visibleLutOffset + static_cast<uint32_t>(count) * sizeof(uint32_t) > f.size()) {
    return std::nullopt;
  }

  f.seek(visibleLutOffset);
  uint16_t result = 0;
  uint32_t lastPageStart = 0;
  for (uint16_t page = 0; page < count; page++) {
    uint32_t pageStart;
    serialization::readPod(f, pageStart);
    lastPageStart = pageStart;
    if (preferFirstAtOffset && pageStart == offset) {
      return page;
    }
    if (pageStart > offset) break;
    result = page;
  }
  if (partial && offset > lastPageStart) {
    return std::nullopt;
  }
  return result;
}
