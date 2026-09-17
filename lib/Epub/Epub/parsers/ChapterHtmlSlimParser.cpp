#include "ChapterHtmlSlimParser.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <ZipFile.h>
#include <expat.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <new>

#include "../../../../src/fontIds.h"
#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/VisibleTextUtils.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/ImageDimsProbe.h"
#include "Epub/converters/ImageToFramebufferDecoder.h"
#include "Epub/htmlEntities.h"

struct ChapterHtmlSlimParser::ImageState {
  enum class Phase : uint8_t { Prepare, Probe, ExtractBegin, Extract, Dimensions, Size, Place, Fallback, Alt };
  Phase phase = Phase::Prepare;
  std::unique_ptr<char[]> src;
  std::unique_ptr<char[]> alt;
  size_t altLength = 0;
  std::string resolvedPath;
  std::string cachedImagePath;
  CssLength cssWidth;
  CssLength cssHeight;
  bool widthDefined = false;
  bool heightDefined = false;
  int displayWidth = 0;
  int displayHeight = 0;
  ImageDimsProbe probe;
  ImageDimensions dimensions = {0, 0};
  std::unique_ptr<ZipFile> stream;
  HalFile output;
  std::unique_ptr<char[]> tempPath;
  uint8_t dimensionAttempts = 0;

  ~ImageState() { discardTemp(); }
  void discardTemp() {
    stream.reset();
    output = HalFile{};
    if (tempPath) {
      Storage.remove(tempPath.get());
      tempPath.reset();
    }
  }
};

ChapterHtmlSlimParser::ChapterHtmlSlimParser(
    std::shared_ptr<Epub> epub, const std::string& filepath, GfxRenderer& renderer, const int fontId,
    const float lineCompression, const bool extraParagraphSpacing, const uint8_t paragraphAlignment,
    const uint16_t viewportWidth, const uint16_t viewportHeight, const bool hyphenationEnabled,
    const bool focusReadingEnabled,
    const std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t)>& completePageFn,
    const bool embeddedStyle, const std::string& contentBase, const std::string& imageBasePath,
    const uint8_t imageRendering, std::vector<std::string> tocAnchors, const std::function<void()>& popupFn,
    const CssParser* cssParser, const bool characterWrap, const bool paragraphIndent)
    : epub(epub),
      filepath(filepath),
      renderer(renderer),
      completePageFn(completePageFn),
      popupFn(popupFn),
      fontId(fontId),
      lineCompression(lineCompression),
      extraParagraphSpacing(extraParagraphSpacing),
      paragraphAlignment(paragraphAlignment),
      viewportWidth(viewportWidth),
      viewportHeight(viewportHeight),
      hyphenationEnabled(hyphenationEnabled),
      focusReadingEnabled(focusReadingEnabled),
      characterWrap(characterWrap),
      paragraphIndent(paragraphIndent),
      cssParser(cssParser),
      embeddedStyle(embeddedStyle),
      imageRendering(imageRendering),
      contentBase(contentBase),
      imageBasePath(imageBasePath),
      tocAnchors(std::move(tocAnchors)) {}

// Minimum file size (in bytes) to show indexing popup - smaller chapters don't benefit from it
constexpr size_t MIN_SIZE_FOR_POPUP = 10 * 1024;  // 10KB
constexpr size_t PARSE_BUFFER_SIZE = 1024;

// This number comes from PR #73
// If we have > 750 words buffered up, perform the layout and consume out all but the last line
// There should be enough here to build out 1-2 full pages and doing this will free up a lot of
// memory.
// Spotted when reading Intermezzo, there are some really long text blocks in there.
constexpr size_t TEXT_BLOCK_SOFT_FLUSH_WORDS = 750;

// When CSS is enabled, flush earlier to save RAM. 320 is still more than enough to build a CJK
// page at font size 14
constexpr size_t TEXT_BLOCK_SOFT_FLUSH_WORDS_WITH_CSS = 320;

// Hard cap on the number of anchor IDs recorded per chapter. Legitimate navigation
// anchors (TOC entries, footnotes, cross-references) rarely exceed a few hundred per
// chapter. A runaway count usually means a converter injected machine-generated IDs on
// every text fragment (e.g. Kobo KePub spans). The cap prevents unbounded heap growth
// on resource-constrained devices (~380KB heap). TOC anchors bypass this cap.
constexpr size_t MAX_ANCHORS_PER_CHAPTER = 1024;

// Reuse serializable PageLine/PageHorizontalRule elements for a small grid.
constexpr int16_t TABLE_CELL_HORIZONTAL_PADDING = 4;
constexpr int16_t TABLE_ROW_SEPARATOR_GAP = 4;
constexpr uint8_t TABLE_ROW_SEPARATOR_THICKNESS = 1;
constexpr int16_t TABLE_MIN_CELL_WIDTH_LINE_HEIGHTS = 3;

constexpr const char* HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
constexpr const char* BLOCK_TAGS[] = {"p", "li", "div", "br", "blockquote"};
constexpr const char* BOLD_TAGS[] = {"b", "strong"};
constexpr const char* ITALIC_TAGS[] = {"i", "em"};
constexpr const char* UNDERLINE_TAGS[] = {"u", "ins"};
constexpr const char* LINETHROUGH_TAGS[] = {"del", "s", "strike"};
constexpr const char* IMAGE_TAGS[] = {"img", "image"};
bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

std::string trimAndNormalize(const std::string& str) {
  if (str.empty()) return "";
  size_t start = 0;
  while (start < str.size() && isWhitespace(str[start])) {
    start++;
  }
  if (start == str.size()) return "";
  size_t end = str.size() - 1;
  while (end > start && isWhitespace(str[end])) {
    end--;
  }
  std::string result;
  result.reserve(end - start + 1);
  bool inSpace = false;
  for (size_t i = start; i <= end; i++) {
    if (isWhitespace(str[i])) {
      if (!inSpace) {
        result.push_back(' ');
        inSpace = true;
      }
    } else {
      result.push_back(str[i]);
      inSpace = false;
    }
  }
  return result;
}

bool matches(const char* tag_name, const char* const* possible_tags, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(tag_name, possible_tags[i]) == 0) {
      return true;
    }
  }
  return false;
}

bool isNonVisibleTextTag(const char* name) { return VisibleTextUtils::isNonVisibleElement(name); }

const char* getAttribute(const XML_Char** atts, const char* attrName) {
  if (!atts) return nullptr;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], attrName) == 0) return atts[i + 1];
  }
  return nullptr;
}

uint16_t parseTableSpan(const char* value) {
  if (!value || value[0] == '\0') return 1;

  uint32_t span = 0;
  for (const char* current = value; *current != '\0'; ++current) {
    if (*current < '0' || *current > '9') return 1;
    const uint32_t digit = static_cast<uint32_t>(*current - '0');
    if (span > (UINT16_MAX - digit) / 10) return UINT16_MAX;
    span = span * 10 + digit;
  }
  return span == 0 ? UINT16_MAX : static_cast<uint16_t>(span);
}

// Returns true if the HTML element is a purely inline, non-navigable wrapper.
// IDs on these elements are never meaningful navigation targets in epub content.
// Reading-system converters (Kobo KePub, Calibre, etc.) frequently inject thousands
// of such IDs for progress tracking or internal bookkeeping, and recording each one
// as a navigation anchor exhausts the heap on memory-constrained devices.
// Block-level, sectioning, and structural elements are always considered navigable.
bool isNonNavigableInlineElement(const char* name) { return strcmp(name, "span") == 0; }

bool isInternalEpubLink(const char* href) {
  if (!href || href[0] == '\0') return false;
  if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) return false;
  if (strncmp(href, "mailto:", 7) == 0) return false;
  if (strncmp(href, "ftp://", 6) == 0) return false;
  if (strncmp(href, "tel:", 4) == 0) return false;
  if (strncmp(href, "javascript:", 11) == 0) return false;
  return true;
}

bool isHeaderOrBlock(const char* name) {
  return matches(name, HEADER_TAGS, std::size(HEADER_TAGS)) || matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS));
}

bool isTableStructuralTag(const char* name) {
  return strcmp(name, "table") == 0 || strcmp(name, "tr") == 0 || strcmp(name, "td") == 0 || strcmp(name, "th") == 0;
}

void ChapterHtmlSlimParser::applyDirectionToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (css.hasDirection()) {
    entry.hasDirection = true;
    entry.direction = css.direction;
  }
}

EpdFontFamily::Style ChapterHtmlSlimParser::fontStyleForTextDecoration(const CssTextDecoration decoration) {
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  if ((decoration & CssTextDecoration::Underline) != CssTextDecoration::None) {
    style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::UNDERLINE);
  }
  if ((decoration & CssTextDecoration::LineThrough) != CssTextDecoration::None) {
    style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::STRIKETHROUGH);
  }
  return style;
}

void ChapterHtmlSlimParser::applyTextDecorationToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (css.hasTextDecoration()) {
    entry.hasTextDecoration = true;
    entry.textDecoration = css.textDecoration;
  }
}

void ChapterHtmlSlimParser::applyVerticalAlignToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (!css.hasVerticalAlign()) return;
  if (css.verticalAlign == CssVerticalAlign::Super) {
    entry.hasSup = true;
    entry.sup = true;
  } else if (css.verticalAlign == CssVerticalAlign::Sub) {
    entry.hasSub = true;
    entry.sub = true;
  }
}

void ChapterHtmlSlimParser::pushTableTextStyleEntry() {
  if (!tableStartStyle.hasBold && !tableStartStyle.hasItalic && !tableStartStyle.hasTextDecoration &&
      !tableStartStyle.hasDirection && !tableStartStyle.hasTextAlign)
    return;
  inlineStyleStack.push_back(tableStartStyle);
  updateEffectiveInlineStyle();
}

void ChapterHtmlSlimParser::pushDecorationStyleEntry(const CssTextDecoration defaultDecoration,
                                                     const CssStyle& cssStyle) {
  StyleStackEntry entry;
  entry.depth = depth;
  entry.hasTextDecoration = true;
  entry.textDecoration = cssStyle.hasTextDecoration() ? cssStyle.textDecoration : defaultDecoration;
  if (cssStyle.hasFontWeight()) {
    entry.hasBold = true;
    entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
  }
  if (cssStyle.hasFontStyle()) {
    entry.hasItalic = true;
    entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
  }
  applyDirectionToEntry(entry, cssStyle);
  inlineStyleStack.push_back(entry);
  updateEffectiveInlineStyle();
}

// Update effective bold/italic/decorations based on block style and inline style stack
void ChapterHtmlSlimParser::updateEffectiveInlineStyle() {
  // Start with block-level styles
  effectiveBold = currentCssStyle.hasFontWeight() && currentCssStyle.fontWeight == CssFontWeight::Bold;
  effectiveItalic = currentCssStyle.hasFontStyle() && currentCssStyle.fontStyle == CssFontStyle::Italic;
  effectiveTextDecoration =
      currentCssStyle.hasTextDecoration() ? currentCssStyle.textDecoration : CssTextDecoration::None;
  bool paragraphDirectionDefined = false;
  bool paragraphIsRtl = false;
  if (!blockStyleStack.empty()) {
    const auto& blockStyle = blockStyleStack.back();
    paragraphDirectionDefined = blockStyle.directionDefined;
    paragraphIsRtl = blockStyle.isRtl;
  }
  effectiveDirectionDefined = paragraphDirectionDefined;
  effectiveDirection = paragraphIsRtl ? CssTextDirection::Rtl : CssTextDirection::Ltr;
  effectiveTextAlignDefined = currentCssStyle.hasTextAlign();
  effectiveTextAlign = currentCssStyle.textAlign;
  effectiveSup = false;
  effectiveSub = false;

  // Apply inline style stack in order
  for (const auto& entry : inlineStyleStack) {
    if (entry.hasBold) {
      effectiveBold = entry.bold;
    }
    if (entry.hasItalic) {
      effectiveItalic = entry.italic;
    }
    // CSS line decorations propagate through descendants; child entries add
    // their own lines but cannot cancel an ancestor's already active line.
    if (entry.hasTextDecoration) {
      effectiveTextDecoration = effectiveTextDecoration | entry.textDecoration;
    }
    if (entry.hasDirection) {
      effectiveDirectionDefined = true;
      effectiveDirection = entry.direction;
      if (entry.setsParagraphDirection) {
        paragraphDirectionDefined = true;
        paragraphIsRtl = entry.direction == CssTextDirection::Rtl;
      }
    }
    if (entry.hasTextAlign) {
      effectiveTextAlignDefined = true;
      effectiveTextAlign = entry.textAlign;
    }
    if (entry.hasSup) {
      effectiveSup = entry.sup;
      if (entry.sup) effectiveSub = false;
    }
    if (entry.hasSub) {
      effectiveSub = entry.sub;
      if (entry.sub) effectiveSup = false;
    }
  }

  // Keep flow direction in the active empty text block. Inline direction remains
  // available for CSS inheritance without replacing the paragraph's base direction.
  if (currentTextBlock && currentTextBlock->isEmpty()) {
    auto& style = currentTextBlock->getBlockStyle();
    style.directionDefined = paragraphDirectionDefined;
    style.isRtl = paragraphIsRtl;
  }
}

void ChapterHtmlSlimParser::flushPendingAnchor() {
  if (pendingAnchorId.empty()) return;

  // If the pending anchor is a TOC chapter boundary, force a page break after the previous
  // block is flushed so the chapter starts on a fresh page.
  if (std::find(tocAnchors.begin(), tocAnchors.end(), pendingAnchorId) != tocAnchors.end()) {
    if (currentPage && !currentPage->elements.empty()) {
      completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
      completedPageCount++;
      currentPage.reset(new Page());
      currentPageNextY = 0;
      currentPageVisibleOffsetSet = false;
    }
  }

  // Record deferred anchor after previous block is flushed (and any TOC page break)
  anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
  pendingAnchorId.clear();
}

void ChapterHtmlSlimParser::setCurrentPageVisibleOffset(const uint32_t offset) {
  if (currentPageVisibleOffsetSet) return;
  // The first page always begins at the start of the body, even when the XHTML
  // contains leading formatting whitespace before its first rendered word.
  currentPageVisibleOffset = completedPageCount == 0 ? 0 : offset;
  currentPageVisibleOffsetSet = true;
}

// flush the contents of partWordBuffer to currentTextBlock
void ChapterHtmlSlimParser::flushPartWordBuffer() {
  if (!currentTextBlock) {
    partWordBufferIndex = 0;
    nextWordContinues = false;
    return;
  }

  // Determine font style from depth-based tracking and CSS effective style
  const bool isBold = boldUntilDepth < depth || effectiveBold;
  const bool isItalic = italicUntilDepth < depth || effectiveItalic;

  // Combine style flags using bitwise OR
  EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
  if (isBold) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::BOLD);
  }
  if (isItalic) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::ITALIC);
  }
  fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | fontStyleForTextDecoration(effectiveTextDecoration));
  if (effectiveSup) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUP);
  } else if (effectiveSub) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUB);
  }

  // flush the buffer
  partWordBuffer[partWordBufferIndex] = '\0';
  const size_t wordBytes = static_cast<size_t>(partWordBufferIndex);
  if (insideTableCell && !tableRowStacked && tableCellTextBytes + wordBytes > MAX_GRID_TABLE_CELL_BYTES) {
    fallbackTableRowToStacked();
  }

  uint8_t linkId = 0;
  if (insideFootnoteLink) {
    if (!currentTextBlock->linkTargetMatches(currentFootnoteLinkId, currentFootnote.href)) {
      currentFootnoteLinkId = currentTextBlock->addLinkTarget(currentFootnote.href);
    }
    linkId = currentFootnoteLinkId;
  }
  currentTextBlock->addWord(partWordBuffer, fontStyle, false, nextWordContinues, partWordVisibleOffset, linkId);
  if (insideTableCell && !tableRowStacked) {
    tableCellTextBytes += wordBytes;
    if (currentTextBlock->size() > MAX_GRID_TABLE_CELL_WORDS) {
      fallbackTableRowToStacked();
    }
  }
  partWordBufferIndex = 0;
  nextWordContinues = false;
  listItemBulletOnly = false;
}

// start a new text block if needed
void ChapterHtmlSlimParser::startNewTextBlock(const BlockStyle& blockStyle, const ParsePhase after) {
  nextWordContinues = false;  // New block = new paragraph, no continuation
  if (currentTextBlock) {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->isEmpty()) {
      // The stack accumulates horizontal margins and text properties from ancestors.
      // Vertical margins are per-element and not inherited through the stack, but
      // container elements deposit their vertical margins on the empty block when they
      // open. Merge those into the new style so the first child in a container inherits
      // the container's vertical spacing.
      const auto style = currentTextBlock->getBlockStyle();
      BlockStyle incoming = blockStyle;
      if (style.fromBrElement) {
        // The empty block was created by a <br> section separator. Inject a full line of
        // blank space before the following paragraph so the scene/section break is visible.
        // This only fires when the <br> block stayed empty (i.e. no inline text was added).
        const int16_t lineHeight = static_cast<int16_t>(renderer.getLineHeight(fontId, lineCompression));
        incoming.marginTop = static_cast<int16_t>(incoming.marginTop + lineHeight);
      }

      currentTextBlock->setBlockStyle(style.getCombinedBlockStyle(incoming, BlockStyle::CombineAxis::Vertical));

      flushPendingAnchor();
      return;
    }

    // <li> added a bullet as the first word, making the block non-empty. When a nested
    // block-level child (<p>, <div>, etc.) opens, reuse the block instead of flushing
    // the bullet to its own line. The bullet stays inline with the child's text.
    if (listItemBulletOnly) {
      const auto style = currentTextBlock->getBlockStyle();
      currentTextBlock->setBlockStyle(style.getCombinedBlockStyle(blockStyle, BlockStyle::CombineAxis::Vertical));
      listItemBulletOnly = false;
      flushPendingAnchor();
      return;
    }

    if (!beginMakePages()) {
      parsePhase_ = ParsePhase::Error;
    } else {
      nextBlockStyle_ = blockStyle;
      nextBlockPhase_ = after;
      nextBlockBullet_ = false;
      parsePhase_ = ParsePhase::NewBlockLayout;
    }
    pauseParse();
    return;
  }
  createTextBlock(blockStyle);
}

void ChapterHtmlSlimParser::createTextBlock(const BlockStyle& blockStyle) {
  // If the pending anchor is a TOC chapter boundary, force a page break after the previous
  // block is flushed so the chapter starts on a fresh page.
  flushPendingAnchor();
  currentTextBlock = makeUniqueNoThrow<ParsedText>(extraParagraphSpacing, hyphenationEnabled, focusReadingEnabled,
                                                   blockStyle, characterWrap, paragraphIndent);
  if (!currentTextBlock) {
    LOG_ERR("EHP", "OOM: paragraph");
    parsePhase_ = ParsePhase::Error;
  }
  wordsExtractedInBlock = 0;
  listItemBulletOnly = false;
}

void ChapterHtmlSlimParser::emitHorizontalRule(const BlockStyle& blockStyle) {
  if (!currentPage) {
    currentPage.reset(new (std::nothrow) Page());
    if (!currentPage) {
      LOG_ERR("EHP", "Failed to create page for horizontal rule");
      parsePhase_ = ParsePhase::Error;
      return;
    }
    currentPageNextY = 0;
  }

  const int16_t lineHeight = static_cast<int16_t>(renderer.getLineHeight(fontId, lineCompression));
  const int16_t defaultVerticalSpacing = static_cast<int16_t>(lineHeight / 2);
  const int16_t topSpacing =
      static_cast<int16_t>((blockStyle.marginTop > 0 ? blockStyle.marginTop : defaultVerticalSpacing) +
                           (blockStyle.paddingTop > 0 ? blockStyle.paddingTop : 0));
  const int16_t bottomSpacing =
      static_cast<int16_t>((blockStyle.marginBottom > 0 ? blockStyle.marginBottom : defaultVerticalSpacing) +
                           (blockStyle.paddingBottom > 0 ? blockStyle.paddingBottom : 0));
  constexpr uint8_t ruleThickness = 2;
  const int16_t availableWidth =
      std::max<int16_t>(1, static_cast<int16_t>(viewportWidth - blockStyle.totalHorizontalInset()));
  const int16_t width = std::max<int16_t>(1, static_cast<int16_t>(availableWidth / 4));
  const int16_t xPos = static_cast<int16_t>(blockStyle.leftInset() + ((availableWidth - width) / 2));
  const int16_t totalHeight = static_cast<int16_t>(topSpacing + ruleThickness + bottomSpacing);

  if (!currentPage->elements.empty() && currentPageNextY + totalHeight > viewportHeight) {
    setCurrentPageVisibleOffset(visibleTextOffset);
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
    completedPageCount++;
    currentPage.reset(new (std::nothrow) Page());
    if (!currentPage) {
      LOG_ERR("EHP", "Failed to create page after horizontal-rule page break");
      parsePhase_ = ParsePhase::Error;
      return;
    }
    currentPageNextY = 0;
    currentPageVisibleOffsetSet = false;
  }

  currentPageNextY += topSpacing;

  auto pageRule = std::shared_ptr<PageHorizontalRule>(
      new (std::nothrow) PageHorizontalRule(width, ruleThickness, xPos, currentPageNextY));
  if (!pageRule) {
    LOG_ERR("EHP", "Failed to create PageHorizontalRule");
    parsePhase_ = ParsePhase::Error;
    return;
  }
  currentPage->elements.push_back(pageRule);
  setCurrentPageVisibleOffset(visibleTextOffset);
  currentPageNextY = static_cast<int16_t>(currentPageNextY + ruleThickness + bottomSpacing);

  if (!pendingAnchorId.empty()) {
    anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
    pendingAnchorId.clear();
  }
}

void ChapterHtmlSlimParser::beginTableAction(const TableAction action, const CssStyle* style) {
  // A word flush can request prefix output earlier in this same XML callback.
  if (parsePhase_ != ParsePhase::Xml && !(parsePhase_ == ParsePhase::Table && tableStage == TableStage::PrefixTake)) {
    LOG_ERR("EHP", "Overlapping table work");
    parsePhase_ = ParsePhase::Error;
    return;
  }
  if (style) {
    tableStartStyle = {};
    tableStartStyle.depth = depth;
    if (style->hasFontWeight()) {
      tableStartStyle.hasBold = true;
      tableStartStyle.bold = style->fontWeight == CssFontWeight::Bold;
    }
    if (style->hasFontStyle()) {
      tableStartStyle.hasItalic = true;
      tableStartStyle.italic = style->fontStyle == CssFontStyle::Italic;
    }
    applyTextDecorationToEntry(tableStartStyle, *style);
    applyDirectionToEntry(tableStartStyle, *style);
    tableStartStyle.setsParagraphDirection = true;
    if (style->hasTextAlign()) {
      tableStartStyle.hasTextAlign = true;
      tableStartStyle.textAlign = style->textAlign;
    }
  }
  tableAction = action;
  tableAfterPrefix = TableStage::Apply;
  tableStage = action == TableAction::Prefix      ? TableStage::PrefixTake
               : action == TableAction::OpenTable ? TableStage::CaptionBegin
                                                  : TableStage::CloseCell;
  parsePhase_ = ParsePhase::Table;
  pauseParse();
}

void ChapterHtmlSlimParser::beginStackedPrefix(const TableStage after) {
  tableRowStacked = true;
  tableAfterPrefix = after;
  if (tableRowCells.empty()) {
    wordsExtractedInBlock = 0;
    tableStage = after;
  } else {
    tableStage = TableStage::PrefixTake;
  }
}

void ChapterHtmlSlimParser::fallbackTableRowToStacked() {
  if (tableRowStacked) return;
  tableRowStacked = true;
  if (tableRowCells.empty()) {
    wordsExtractedInBlock = 0;
    return;
  }
  beginTableAction(TableAction::Prefix);
}

void ChapterHtmlSlimParser::addTableRowSeparator() {
  if (!currentPage || currentPage->elements.empty() || viewportWidth == 0 ||
      currentPageNextY + TABLE_ROW_SEPARATOR_GAP > viewportHeight) {
    return;
  }

  auto separator = std::shared_ptr<PageHorizontalRule>(
      new (std::nothrow) PageHorizontalRule(viewportWidth, TABLE_ROW_SEPARATOR_THICKNESS, 0, currentPageNextY + 1));
  if (!separator) {
    LOG_ERR("EHP", "OOM: table row separator");
    return;
  }
  if (currentPage->elements.capacity() == currentPage->elements.size()) {
    currentPage->elements.reserve(currentPage->elements.size() + 1);
  }
  currentPage->elements.push_back(std::move(separator));
  currentPageNextY += TABLE_ROW_SEPARATOR_GAP;
}

void ChapterHtmlSlimParser::tableSome(uint16_t maxUnits) {
  const int16_t lineHeight =
      std::max<int16_t>(1, static_cast<int16_t>(renderer.getLineHeight(fontId) * lineCompression));
  while (maxUnits-- != 0) {
    switch (tableStage) {
      case TableStage::CloseCell:
        tableStage = TableStage::AfterCell;
        if (!insideTableCell) break;
        insideTableCell = false;
        if (!currentTextBlock) break;
        if (tableRowStacked || tableRowCells.size() >= MAX_GRID_TABLE_COLUMNS ||
            currentTextBlock->size() > MAX_GRID_TABLE_CELL_WORDS) {
          beginStackedPrefix(TableStage::StackedCellBegin);
        } else {
          tableRowCells.push_back(std::move(currentTextBlock));
        }
        break;
      case TableStage::AfterCell:
        if (tableAction == TableAction::CloseCell) {
          tableStage = TableStage::Apply;
        } else if (tableAction == TableAction::OpenCell || tableAction == TableAction::OpenHeader) {
          tableStage = TableStage::CaptionBegin;
        } else {
          tableStage = TableStage::PrepareRow;
        }
        break;
      case TableStage::PrefixTake:
        tableActiveCell = std::move(currentTextBlock);
        tableColumn = 0;
        tableStage = TableStage::PrefixBegin;
        break;
      case TableStage::PrefixBegin:
        if (tableColumn == tableRowCells.size()) {
          tableRowCells.clear();
          currentTextBlock = std::move(tableActiveCell);
          wordsExtractedInBlock = 0;
          tableStage = tableAfterPrefix;
        } else {
          currentTextBlock = std::move(tableRowCells[tableColumn++]);
          wordsExtractedInBlock = 0;
          if (currentTextBlock && !currentTextBlock->isEmpty()) {
            if (!beginMakePages()) {
              parsePhase_ = ParsePhase::Error;
              return;
            }
            tableStage = TableStage::PrefixLayout;
          }
        }
        break;
      case TableStage::PrefixLayout:
        if (makePagesSome(1)) tableStage = TableStage::PrefixBegin;
        break;
      case TableStage::StackedCellBegin:
        wordsExtractedInBlock = 0;
        if (currentTextBlock && !currentTextBlock->isEmpty()) {
          if (!beginMakePages()) {
            parsePhase_ = ParsePhase::Error;
            return;
          }
          tableStage = TableStage::StackedCellLayout;
        } else {
          currentTextBlock.reset();
          tableStage = TableStage::AfterCell;
        }
        break;
      case TableStage::StackedCellLayout:
        if (makePagesSome(1)) {
          currentTextBlock.reset();
          tableStage = TableStage::AfterCell;
        }
        break;
      case TableStage::PrepareRow: {
        if (tableRowCells.empty()) {
          tableStage = tableRowStacked ? TableStage::FinishRow : TableStage::AfterRow;
          break;
        }
        tableColumnCount = static_cast<uint8_t>(tableRowCells.size());
        const uint16_t cellWidth = static_cast<uint16_t>(viewportWidth / tableColumnCount);
        if (tableColumnCount < 2 || cellWidth <= TABLE_CELL_HORIZONTAL_PADDING * 2 ||
            cellWidth < lineHeight * TABLE_MIN_CELL_WIDTH_LINE_HEIGHTS) {
          beginStackedPrefix(TableStage::FinishRow);
          break;
        }
        for (auto& lines : tableCellLines) lines.clear();
        tableLineVisibleOffsets.clear();
        if (tableLineVisibleOffsets.capacity() < MAX_GRID_TABLE_CELL_WORDS * 2) {
          tableLineVisibleOffsets.reserve(MAX_GRID_TABLE_CELL_WORDS * 2);
        }
        tableColumn = 0;
        tableLine = tableMaxLines = 0;
        tableStage = TableStage::GridCellBegin;
        break;
      }
      case TableStage::GridCellBegin: {
        if (tableColumn == tableColumnCount) {
          tableRowCells.clear();
          tableStage = TableStage::GridLines;
          break;
        }
        auto& lines = tableCellLines[tableColumn];
        if (lines.capacity() < MAX_GRID_TABLE_CELL_WORDS * 2) lines.reserve(MAX_GRID_TABLE_CELL_WORDS * 2);
        const uint16_t textWidth = viewportWidth / tableColumnCount - TABLE_CELL_HORIZONTAL_PADDING * 2;
        if (!tableRowCells[tableColumn]->beginLayout(fontId, textWidth)) {
          parsePhase_ = ParsePhase::Error;
          return;
        }
        tableStage = TableStage::GridCellLayout;
        break;
      }
      case TableStage::GridCellLayout: {
        auto& lines = tableCellLines[tableColumn];
        if (tableRowCells[tableColumn]->layoutSome(
                renderer,
                [this, &lines](const std::shared_ptr<TextBlock>& line, const uint32_t offset) {
                  const size_t lineIndex = lines.size();
                  lines.push_back(line);
                  if (tableLineVisibleOffsets.size() <= lineIndex) {
                    tableLineVisibleOffsets.resize(lineIndex + 1, UINT32_MAX);
                  }
                  tableLineVisibleOffsets[lineIndex] = std::min(tableLineVisibleOffsets[lineIndex], offset);
                },
                1)) {
          tableMaxLines = std::max(tableMaxLines, lines.size());
          ++tableColumn;
          tableStage = TableStage::GridCellBegin;
        }
        break;
      }
      case TableStage::GridLines: {
        if (tableLine == tableMaxLines) {
          tableStage = TableStage::FinishRow;
          break;
        }
        const uint16_t cellWidth = static_cast<uint16_t>(viewportWidth / tableColumnCount);
        const uint32_t lineVisibleOffset =
            tableLine < tableLineVisibleOffsets.size() ? tableLineVisibleOffsets[tableLine] : visibleTextOffset;
        int16_t rowLineHeight = lineHeight;
        for (size_t column = 0; column < tableColumnCount; ++column) {
          if (tableLine < tableCellLines[column].size()) {
            rowLineHeight = std::max<int16_t>(
                rowLineHeight, static_cast<int16_t>(lineHeight + tableCellLines[column][tableLine]->getRubyShift(
                                                                     renderer.getFontAscenderSize(fontId))));
          }
        }

        const bool pageFull =
            currentPage && !currentPage->elements.empty() && currentPageNextY + rowLineHeight > viewportHeight;
        if (!currentPage || pageFull) {
          if (pageFull) {
            setCurrentPageVisibleOffset(lineVisibleOffset);
            completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
            completedPageCount++;
          }
          currentPage = makeUniqueNoThrow<Page>();
          if (!currentPage) {
            LOG_ERR("EHP", "OOM: page for table row");
            parsePhase_ = ParsePhase::Error;
            return;
          }
          currentPageNextY = 0;
          currentPageVisibleOffsetSet = false;
        }

        const int16_t rowY = currentPageNextY;
        const size_t requiredCapacity = currentPage->elements.size() + tableColumnCount;
        if (currentPage->elements.capacity() < requiredCapacity) {
          const size_t linesThatFit =
              std::max<size_t>(1, static_cast<size_t>((viewportHeight - currentPageNextY) / rowLineHeight));
          const size_t linesToReserve = std::min(tableMaxLines - tableLine, linesThatFit);
          currentPage->elements.reserve(currentPage->elements.size() + linesToReserve * tableColumnCount + 1);
        }
        for (size_t column = 0; column < tableColumnCount; ++column) {
          if (tableLine >= tableCellLines[column].size()) {
            continue;
          }

          auto& line = tableCellLines[column][tableLine];
          auto style = line->getBlockStyle();
          const size_t physicalColumn = tableRowRtl ? tableColumnCount - column - 1 : column;
          style.marginLeft = static_cast<int16_t>(physicalColumn * cellWidth + TABLE_CELL_HORIZONTAL_PADDING);
          style.paddingLeft = 0;
          line->setBlockStyle(style);

          // Reset Y so every cell in this slice shares one baseline.
          currentPageNextY = rowY;
          addLineToPage(line, lineVisibleOffset);
        }
        currentPageNextY = static_cast<int16_t>(rowY + rowLineHeight);
        ++tableLine;
        break;
      }
      case TableStage::FinishRow:
        addTableRowSeparator();
        tableRowStacked = false;
        for (auto& lines : tableCellLines) lines.clear();
        tableLineVisibleOffsets.clear();
        tableStage = TableStage::AfterRow;
        break;
      case TableStage::AfterRow:
        tableStage = tableAction == TableAction::CloseRow ? TableStage::Apply : TableStage::CaptionBegin;
        break;
      case TableStage::CaptionBegin:
        if (currentTextBlock && !currentTextBlock->isEmpty()) {
          if (!beginMakePages()) {
            parsePhase_ = ParsePhase::Error;
            return;
          }
          tableStage = TableStage::CaptionLayout;
        } else {
          tableStage = TableStage::AfterCaption;
        }
        break;
      case TableStage::CaptionLayout:
        if (makePagesSome(1)) {
          currentTextBlock.reset();
          tableStage = TableStage::AfterCaption;
        }
        break;
      case TableStage::AfterCaption:
        if (tableAction != TableAction::OpenTable) currentTextBlock.reset();
        if ((tableAction == TableAction::OpenCell || tableAction == TableAction::OpenHeader) &&
            (tableColumnSpan > 1 || tableRowSpan > 1)) {
          beginStackedPrefix(TableStage::Apply);
        } else {
          tableStage = TableStage::Apply;
        }
        break;
      case TableStage::Apply:
        switch (tableAction) {
          case TableAction::Prefix:
          case TableAction::CloseCell:
          case TableAction::CloseRow:
            break;
          case TableAction::PrefixSoft:
            parsePhase_ = ParsePhase::Xml;
            softFlushIfNeeded();
            return;
          case TableAction::OpenTable:
            flushPendingAnchor();
            pushTableTextStyleEntry();
            tableDepth = 1;
            insideTableCell = false;
            tableRowStacked = false;
            tableRowRtl = tableStartStyle.hasDirection && tableStartStyle.direction == CssTextDirection::Rtl;
            tableRowsSpannedRemaining = 0;
            tableCellTextBytes = 0;
            tableRowCells.clear();
            tableRowCells.reserve(MAX_GRID_TABLE_COLUMNS);
            ++depth;
            break;
          case TableAction::OpenRow:
            tableRowStacked = tableRowsSpannedRemaining > 0;
            tableRowRtl = tableStartStyle.hasDirection && tableStartStyle.direction == CssTextDirection::Rtl;
            if (tableRowsSpannedRemaining != UINT16_MAX && tableRowsSpannedRemaining > 0) --tableRowsSpannedRemaining;
            pushTableTextStyleEntry();
            ++depth;
            break;
          case TableAction::OpenCell:
          case TableAction::OpenHeader: {
            if (tableRowSpan > 1) {
              const uint16_t remaining =
                  tableRowSpan == UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(tableRowSpan - 1);
              tableRowsSpannedRemaining = std::max(tableRowsSpannedRemaining, remaining);
            }
            BlockStyle cellStyle;
            cellStyle.textAlignDefined = true;
            cellStyle.alignment =
                tableStartStyle.hasTextAlign ? tableStartStyle.textAlign
                : effectiveTextAlignDefined
                    ? effectiveTextAlign
                    : (tableStartStyle.hasDirection && tableStartStyle.direction == CssTextDirection::Rtl
                           ? CssTextAlign::Right
                           : CssTextAlign::Left);
            if (tableStartStyle.hasDirection) {
              cellStyle.directionDefined = true;
              cellStyle.isRtl = tableStartStyle.direction == CssTextDirection::Rtl;
            }
            currentTextBlock =
                makeUniqueNoThrow<ParsedText>(extraParagraphSpacing, hyphenationEnabled, focusReadingEnabled, cellStyle,
                                              characterWrap, paragraphIndent);
            if (!currentTextBlock) {
              LOG_ERR("EHP", "OOM: table cell");
              skipUntilDepth = depth;
              ++depth;
              break;
            }
            insideTableCell = true;
            tableCellTextBytes = 0;
            wordsExtractedInBlock = 0;
            flushPendingAnchor();
            pushTableTextStyleEntry();
            if (tableAction == TableAction::OpenHeader && (!tableStartStyle.hasBold || tableStartStyle.bold)) {
              boldUntilDepth = std::min(boldUntilDepth, depth);
            }
            ++depth;
            break;
          }
          case TableAction::CloseTable: {
            tableDepth = 0;
            insideTableCell = false;
            tableRowStacked = false;
            tableRowsSpannedRemaining = 0;
            tableCellTextBytes = 0;
            tableRowCells.clear();
            nextWordContinues = false;
            const BlockStyle flowStyle =
                blockStyleStack.empty() ? BlockStyle() : blockStyleStack.back().withoutBottom();
            currentTextBlock =
                makeUniqueNoThrow<ParsedText>(extraParagraphSpacing, hyphenationEnabled, focusReadingEnabled, flowStyle,
                                              characterWrap, paragraphIndent);
            if (!currentTextBlock) LOG_ERR("EHP", "OOM: text block after table");
            wordsExtractedInBlock = 0;
            updateEffectiveInlineStyle();
            break;
          }
        }
        parsePhase_ = ParsePhase::Xml;
        return;
    }
  }
}

void ChapterHtmlSlimParser::imageSome() {
  auto& work = *imageState_;
  auto& src = work.src;
  auto& alt = work.alt;
  auto& resolvedPath = work.resolvedPath;
  auto& cachedImagePath = work.cachedImagePath;
  auto& displayWidth = work.displayWidth;
  auto& displayHeight = work.displayHeight;
  if (work.phase == ImageState::Phase::Alt) {
    italicUntilDepth = std::min(italicUntilDepth, depth);
    ++depth;
    syntheticCharacterData = true;
    parsePhase_ = ParsePhase::Xml;
    characterData(this, alt.get(), static_cast<int>(work.altLength));
    syntheticCharacterData = false;
    skipUntilDepth = depth - 1;
    imageState_.reset();
    return;
  }
  if (work.phase == ImageState::Phase::Place) {
    // Apply vertical margins from the container to the image.
    // Top margin lives on the empty text block (deposited via vertical merge
    // in startNewTextBlock). Bottom margin was stripped by withoutBottom() for
    // deferred application at element close, so read it from the stack.
    int16_t imageMarginTop = 0;
    int16_t imageMarginBottom = 0;
    if (currentTextBlock && currentTextBlock->isEmpty()) {
      const auto& bs = currentTextBlock->getBlockStyle();
      imageMarginTop = bs.topInset();
      if (blockStyleStack.size() > 1) {
        imageMarginBottom = blockStyleStack.back().bottomInset();
      }
    }

    // Create page for image - only break if image won't fit remaining space
    if (currentPage && !currentPage->elements.empty() &&
        (currentPageNextY + imageMarginTop + displayHeight + imageMarginBottom > viewportHeight)) {
      completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
      completedPageCount++;
      currentPage = makeUniqueNoThrow<Page>();
      if (!currentPage) {
        LOG_ERR("EHP", "Failed to create new page");
        parsePhase_ = ParsePhase::Error;
        return;
      }
      currentPageNextY = 0;
      currentPageVisibleOffsetSet = false;
    } else if (!currentPage) {
      currentPage = makeUniqueNoThrow<Page>();
      if (!currentPage) {
        LOG_ERR("EHP", "Failed to create initial page");
        parsePhase_ = ParsePhase::Error;
        return;
      }
      currentPageNextY = 0;
      currentPageVisibleOffsetSet = false;
    }

    // Apply top margin from container block. Clamp it so the image never
    // overflows the page bottom: a full-viewport-height image leaves no room
    // for the margin, and the break above only fires on non-empty pages, so a
    // fresh page would otherwise place the image at y=marginTop and run
    // marginTop pixels past viewportHeight. A large bottom reserve (status
    // bar / big screen margin) absorbs that overflow silently, but with a
    // thin reserve it crosses the physical screen edge and fails
    // ImageBlock::render's bounds check, dropping the image entirely.
    if (currentPageNextY + imageMarginTop + displayHeight > viewportHeight) {
      const int room = viewportHeight - displayHeight - currentPageNextY;
      imageMarginTop = static_cast<int16_t>(room > 0 ? room : 0);
    }
    currentPageNextY += imageMarginTop;

    // Create ImageBlock and add to page
    // nothrow: make_shared uses bare new, which aborts on OOM under
    // -fno-exceptions; images arrive mid-parse when the heap is at its
    // most loaded, so this must fail soft into the null-check below.
    auto imageBlock = std::shared_ptr<ImageBlock>(
        new (std::nothrow) ImageBlock(cachedImagePath, resolvedPath, displayWidth, displayHeight));
    if (!imageBlock) {
      LOG_ERR("EHP", "Failed to create ImageBlock");
      parsePhase_ = ParsePhase::Error;
      return;
    }
    int xPos = (viewportWidth - displayWidth) / 2;
    auto pageImage = std::shared_ptr<PageImage>(new (std::nothrow) PageImage(imageBlock, xPos, currentPageNextY));
    if (!pageImage) {
      LOG_ERR("EHP", "Failed to create PageImage");
      parsePhase_ = ParsePhase::Error;
      return;
    }
    currentPage->elements.push_back(pageImage);
    setCurrentPageVisibleOffset(visibleTextOffset);
    currentPageNextY += displayHeight + imageMarginBottom;

    // The image consumed the empty block's accumulated vertical spacing.
    // Reset the block so the Vertical merge in startNewTextBlock doesn't
    // re-apply the same margins to the next text paragraph.
    if (currentTextBlock && currentTextBlock->isEmpty()) {
      BlockStyle resetStyle;
      resetStyle.alignment = (paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                 ? CssTextAlign::Justify
                                 : static_cast<CssTextAlign>(paragraphAlignment);
      currentTextBlock->setBlockStyle(resetStyle);
    }

    ++depth;
    imageState_.reset();
    parsePhase_ = ParsePhase::Xml;
    return;
  }
  if (work.phase == ImageState::Phase::Prepare) {
    if (src) {
      if (char* fragment = strchr(src.get(), '#')) *fragment = '\0';
    }
    if (src && src[0] && imageRendering != 1) {
      resolvedPath = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(contentBase + src.get()));
      if (ImageDecoderFactory::isFormatSupported(resolvedPath)) {
        const size_t dot = resolvedPath.rfind('.');
        cachedImagePath =
            imageBasePath + std::to_string(imageCounter++) + (dot == std::string::npos ? "" : resolvedPath.substr(dot));
        work.stream = makeUniqueNoThrow<ZipFile>(epub->getPath());
        if (work.stream && work.stream->beginReadFileToStream(resolvedPath.c_str(), 1024, true)) {
          work.phase = ImageState::Phase::Probe;
        } else {
          LOG_ERR("EHP", "Failed to start image probe");
          work.stream.reset();
          work.phase = ImageState::Phase::ExtractBegin;
        }
        return;
      }
    }
    work.phase = ImageState::Phase::Fallback;
  }
  if (work.phase == ImageState::Phase::Probe) {
    if (work.stream->readSome(work.probe) == ZipFile::ReadStatus::More) return;
    work.stream.reset();
    work.phase = work.probe.getDimensions(work.dimensions) ? ImageState::Phase::Size : ImageState::Phase::ExtractBegin;
    return;
  }
  if (work.phase == ImageState::Phase::ExtractBegin) {
    if (popupFn && !imagePopupFired) {
      imagePopupFired = true;
      popupFn();
    }
    const size_t pathSize = cachedImagePath.size() + sizeof(".part");
    work.tempPath = makeUniqueNoThrowForOverwrite<char[]>(pathSize);
    if (!work.tempPath) {
      LOG_ERR("EHP", "OOM: image extraction path");
      parsePhase_ = ParsePhase::Error;
      return;
    }
    snprintf(work.tempPath.get(), pathSize, "%s.part", cachedImagePath.c_str());
    work.stream = makeUniqueNoThrow<ZipFile>(epub->getPath());
    if (work.stream && Storage.openFileForWrite("EHP", work.tempPath.get(), work.output) &&
        work.stream->beginReadFileToStream(resolvedPath.c_str(), 4096)) {
      work.phase = ImageState::Phase::Extract;
    } else {
      LOG_ERR("EHP", "Failed to start image extraction");
      work.discardTemp();
      work.phase = ImageState::Phase::Fallback;
    }
    return;
  }
  if (work.phase == ImageState::Phase::Extract) {
    const auto status = work.stream->readSome(work.output);
    if (status == ZipFile::ReadStatus::More) return;
    work.stream.reset();
    const bool complete = status == ZipFile::ReadStatus::Done;
    if (complete) work.output.flush();
    work.output = HalFile{};
    if (complete && (!Storage.exists(cachedImagePath.c_str()) || Storage.remove(cachedImagePath.c_str())) &&
        Storage.rename(work.tempPath.get(), cachedImagePath.c_str())) {
      work.tempPath.reset();
      work.phase = ImageState::Phase::Dimensions;
    } else {
      LOG_ERR("EHP", "Failed to extract image");
      work.discardTemp();
      work.phase = ImageState::Phase::Fallback;
    }
    return;
  }
  if (work.phase == ImageState::Phase::Dimensions) {
    // Keep the existing slow-card retries, returning between decoder attempts.
    if (work.dimensionAttempts > 0) delay(50);
    ++work.dimensionAttempts;
    auto* decoder = ImageDecoderFactory::getDecoder(cachedImagePath);
    if (decoder && decoder->getDimensions(cachedImagePath, work.dimensions)) {
      work.phase = ImageState::Phase::Size;
    } else if (work.dimensionAttempts == 3) {
      LOG_ERR("EHP", "Failed to get image dimensions");
      Storage.remove(cachedImagePath.c_str());
      work.phase = ImageState::Phase::Fallback;
    }
    return;
  }
  if (work.phase == ImageState::Phase::Size) {
    const auto& dims = work.dimensions;
    displayWidth = 0;
    displayHeight = 0;
    const float emSize = static_cast<float>(renderer.getFontAscenderSize(fontId));
    const bool hasCssHeight = work.heightDefined;
    const bool hasCssWidth = work.widthDefined;

    // Compute effective container width for percentage-based image sizes.
    // If the image is inside a block with horizontal margins/padding (e.g.
    // <div style="margin: 1em 40%">), percentage widths like width:100%
    // should resolve against the container width, not the full viewport.
    int containerWidth = viewportWidth;
    if (currentTextBlock) {
      const int inset = currentTextBlock->getBlockStyle().totalHorizontalInset();
      if (inset > 0 && inset < viewportWidth) {
        containerWidth = viewportWidth - inset;
      }
    }

    if (hasCssHeight && hasCssWidth && dims.width > 0 && dims.height > 0) {
      // Both CSS height and width set: resolve both, then clamp to viewport preserving requested ratio
      displayHeight = static_cast<int>(work.cssHeight.toPixels(emSize, static_cast<float>(viewportHeight)) + 0.5f);
      displayWidth = static_cast<int>(work.cssWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
      if (displayHeight < 1) displayHeight = 1;
      if (displayWidth < 1) displayWidth = 1;
      if (displayWidth > containerWidth || displayHeight > viewportHeight) {
        float scaleX = (displayWidth > containerWidth) ? static_cast<float>(containerWidth) / displayWidth : 1.0f;
        float scaleY = (displayHeight > viewportHeight) ? static_cast<float>(viewportHeight) / displayHeight : 1.0f;
        float scale = (scaleX < scaleY) ? scaleX : scaleY;
        displayWidth = static_cast<int>(displayWidth * scale + 0.5f);
        displayHeight = static_cast<int>(displayHeight * scale + 0.5f);
        if (displayWidth < 1) displayWidth = 1;
        if (displayHeight < 1) displayHeight = 1;
      }
      LOG_DBG("EHP", "Display size from CSS height+width: %dx%d", displayWidth, displayHeight);
    } else if (hasCssHeight && !hasCssWidth && dims.width > 0 && dims.height > 0) {
      // Use CSS height (resolve % against viewport height) and derive width from aspect ratio
      displayHeight = static_cast<int>(work.cssHeight.toPixels(emSize, static_cast<float>(viewportHeight)) + 0.5f);
      if (displayHeight < 1) displayHeight = 1;
      displayWidth = static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
      if (displayHeight > viewportHeight) {
        displayHeight = viewportHeight;
        // Rescale width to preserve aspect ratio when height is clamped
        displayWidth = static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
        if (displayWidth < 1) displayWidth = 1;
      }
      if (displayWidth > containerWidth) {
        displayWidth = containerWidth;
        // Rescale height to preserve aspect ratio when width is clamped
        displayHeight = static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
        if (displayHeight < 1) displayHeight = 1;
      }
      if (displayWidth < 1) displayWidth = 1;
      LOG_DBG("EHP", "Display size from CSS height: %dx%d", displayWidth, displayHeight);
    } else if (hasCssWidth && !hasCssHeight && dims.width > 0 && dims.height > 0) {
      // Use CSS width (resolve % against container width) and derive height from aspect ratio
      displayWidth = static_cast<int>(work.cssWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
      if (displayWidth > containerWidth) displayWidth = containerWidth;
      if (displayWidth < 1) displayWidth = 1;
      displayHeight = static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
      if (displayHeight > viewportHeight) {
        displayHeight = viewportHeight;
        // Rescale width to preserve aspect ratio when height is clamped
        displayWidth = static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
        if (displayWidth < 1) displayWidth = 1;
      }
      if (displayHeight < 1) displayHeight = 1;
      LOG_DBG("EHP", "Display size from CSS width: %dx%d", displayWidth, displayHeight);
    } else {
      // Scale to fit container while maintaining aspect ratio
      int maxWidth = containerWidth;
      int maxHeight = viewportHeight;
      float scaleX = (dims.width > maxWidth) ? (float)maxWidth / dims.width : 1.0f;
      float scaleY = (dims.height > maxHeight) ? (float)maxHeight / dims.height : 1.0f;
      float scale = (scaleX < scaleY) ? scaleX : scaleY;
      if (scale > 1.0f) scale = 1.0f;

      displayWidth = (int)(dims.width * scale);
      displayHeight = (int)(dims.height * scale);
      LOG_DBG("EHP", "Display size: %dx%d (scale %.2f)", displayWidth, displayHeight, scale);
    }

    work.phase = ImageState::Phase::Place;
    if (partWordBufferIndex > 0) flushPartWordBuffer();
    if (currentTextBlock && !currentTextBlock->isEmpty()) {
      const BlockStyle parentBlockStyle = currentTextBlock->getBlockStyle();
      startNewTextBlock(parentBlockStyle, ParsePhase::Image);
    }
    return;
  }

  if (alt) {
    BlockStyle centeredBlockStyle;
    centeredBlockStyle.textAlignDefined = true;
    centeredBlockStyle.alignment = CssTextAlign::Center;
    work.phase = ImageState::Phase::Alt;
    startNewTextBlock(blockStyleStack.back()
                          .getCombinedBlockStyle(centeredBlockStyle, BlockStyle::CombineAxis::Horizontal)
                          .withoutBottom(),
                      ParsePhase::Image);
    return;
  }
  skipUntilDepth = depth;
  ++depth;
  imageState_.reset();
  parsePhase_ = ParsePhase::Xml;
}

void XMLCALL ChapterHtmlSlimParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  if (self->parsePhase_ == ParsePhase::Error) return;
  if (strcasecmp(name, "body") == 0) {
    // Case-insensitive to match ParagraphStreamer's tag matching (ProgressMapper). A case
    // mismatch here would leave visibleTextOffset at 0 for the whole section, so every page
    // would record offset 0 while the sync resolver still counts a non-zero offset.
    self->insideBody = true;
  }
  if (self->insideBody && (self->nonVisibleTextDepth > 0 || isNonVisibleTextTag(name))) {
    self->nonVisibleTextDepth++;
  }

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    self->depth += 1;
    return;
  }

  if (strcmp(name, "p") == 0) {
    self->xpathParagraphIndex++;
  }
  if (strcmp(name, "li") == 0) {
    self->xpathListItemIndex++;
  }

  // Extract class, style, id, and dir attributes for CSS/RTL processing
  std::string classAttr;
  std::string styleAttr;
  std::string dirAttr;
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "class") == 0) {
        classAttr = atts[i + 1];
      } else if (strcmp(atts[i], "style") == 0) {
        styleAttr = atts[i + 1];
      } else if (strcmp(atts[i], "id") == 0) {
        // Defer both anchor recording and TOC page breaks until startNewTextBlock,
        // after the previous block's paragraph layout finishes.
        //
        // Skip IDs on non-navigable inline elements (e.g. <span>): these are never
        // link targets in epub content, but reading-system converters can inject tens
        // of thousands of them per chapter, exhausting the heap. TOC anchors are
        // always recorded regardless of element type, since they drive page breaks.
        const char* idValue = atts[i + 1];
        const bool isTocAnchor =
            std::find(self->tocAnchors.begin(), self->tocAnchors.end(), idValue) != self->tocAnchors.end();
        if (isTocAnchor || (!isNonNavigableInlineElement(name) && self->anchorData.size() < MAX_ANCHORS_PER_CHAPTER)) {
          // Flush a displaced anchor before overwriting. Consecutive non-block elements
          // (e.g. <aside id="fn1">text</aside><aside id="fn2">) with no intervening block
          // never trigger startNewTextBlock, so fn1 gets silently overwritten. That leaves
          // fn1 missing from the anchor map -> getPageForAnchor returns nullopt -> reader
          // lands at page 0 (section start) instead of the footnote.
          if (!self->pendingAnchorId.empty()) {
            self->flushPendingAnchor();
          }
          self->pendingAnchorId = idValue;
        }
      } else if (strcmp(atts[i], "dir") == 0) {
        dirAttr = atts[i + 1];
      }
    }
  }

  // Compute CSS style for this element early so display:none can short-circuit
  // before tag-specific branches emit any content or metadata.
  CssStyle cssStyle;
  if (self->cssParser) {
    cssStyle = self->cssParser->resolveStyle(name, classAttr);
    if (!styleAttr.empty()) {
      CssStyle inlineStyle = CssParser::parseInlineStyle(styleAttr);
      cssStyle.applyOver(inlineStyle);
    }
  } else if (self->embeddedStyle && !styleAttr.empty()) {
    // Honor explicit paragraph indent even without a stylesheet, without
    // enabling unrelated inline CSS that upstream ignores in this case.
    const CssStyle inlineStyle = CssParser::parseInlineStyle(styleAttr);
    cssStyle.textIndent = inlineStyle.textIndent;
    cssStyle.defined.textIndent = inlineStyle.defined.textIndent;
  }

  // HTML dir attribute overrides CSS direction (case-insensitive per HTML spec)
  if (!dirAttr.empty()) {
    if (strcasecmp(dirAttr.c_str(), "rtl") == 0) {
      cssStyle.direction = CssTextDirection::Rtl;
      cssStyle.defined.direction = 1;
    } else if (strcasecmp(dirAttr.c_str(), "ltr") == 0) {
      cssStyle.direction = CssTextDirection::Ltr;
      cssStyle.defined.direction = 1;
    }
  }

  // Direction is inherited in HTML/CSS. If this element does not define one, carry
  // the currently active inherited direction into its computed style.
  if (!cssStyle.hasDirection() && self->effectiveDirectionDefined) {
    cssStyle.direction = self->effectiveDirection;
    cssStyle.defined.direction = 1;
  }

  // Skip elements with display:none before all fast paths (tables, links, etc.).
  if (cssStyle.hasDisplay() && cssStyle.display == CssDisplay::None) {
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Buffer one simple row; oversized rows fall back to full-width flow.
  if (strcmp(name, "table") == 0) {
    // Flatten nested content without allocating a recursive row buffer.
    if (self->tableDepth > 0) {
      if (self->tableDepth == 1 && self->insideTableCell && self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      self->nextWordContinues = false;
      self->tableDepth += 1;
      self->depth += 1;
      return;
    }

    if (self->partWordBufferIndex > 0) self->flushPartWordBuffer();
    self->beginTableAction(TableAction::OpenTable, &cssStyle);
    return;
  }

  if (self->tableDepth == 1 && strcmp(name, "tr") == 0) {
    self->beginTableAction(TableAction::OpenRow, &cssStyle);
    return;
  }

  if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    if (self->partWordBufferIndex > 0) self->flushPartWordBuffer();
    self->tableColumnSpan = parseTableSpan(getAttribute(atts, "colspan"));
    self->tableRowSpan = parseTableSpan(getAttribute(atts, "rowspan"));
    self->beginTableAction(strcmp(name, "th") == 0 ? TableAction::OpenHeader : TableAction::OpenCell, &cssStyle);
    return;
  }

  if (self->tableDepth >= 1 && strcmp(name, "hr") == 0) {
    self->depth += 1;
    return;
  }

  if (self->tableDepth >= 1 && self->insideTableCell && isHeaderOrBlock(name)) {
    // Collapse block markup inside a cell to a word boundary.
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->nextWordContinues = false;
    self->depth += 1;
    return;
  }

  if (self->tableDepth >= 1 && self->insideTableCell && matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS))) {
    // Preserve alt text without allocating an image framebuffer in the row.
    const char* alt = getAttribute(atts, "alt");
    if (alt && alt[0] != '\0') {
      self->syntheticCharacterData = true;
      self->characterData(userData, alt, strlen(alt));
      self->syntheticCharacterData = false;
    }
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  if (matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS))) {
    if (self->imageRendering == 2) {
      self->skipUntilDepth = self->depth++;
      return;
    }
    const char* src = nullptr;
    const char* alt = nullptr;
    if (atts) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "src") == 0) {
          src = atts[i + 1];
        } else if ((!src || !*src) && (strcmp(atts[i], "href") == 0 || strcmp(atts[i], "xlink:href") == 0)) {
          src = atts[i + 1];
        } else if (strcmp(atts[i], "alt") == 0) {
          alt = atts[i + 1];
        }
      }
    }
    if ((!src || !*src) && (!alt || !*alt)) {
      self->skipUntilDepth = self->depth++;
      return;
    }
    auto image = makeUniqueNoThrow<ImageState>();
    if (!image) {
      LOG_ERR("EHP", "OOM: image preparation");
      self->parsePhase_ = ParsePhase::Error;
      self->pauseParse();
      return;
    }
    // Expat owns the attributes only for this callback. Copy the bytes before
    // suspending, with explicit failure instead of throwing string construction.
    if (src) {
      const size_t length = strlen(src) + 1;
      image->src = makeUniqueNoThrowForOverwrite<char[]>(length);
      if (image->src) memcpy(image->src.get(), src, length);
    }
    if (alt && *alt) {
      image->altLength = strlen(alt) + 9;  // "[Image: " + alt + "]"
      image->alt = makeUniqueNoThrowForOverwrite<char[]>(image->altLength + 1);
      if (image->alt) snprintf(image->alt.get(), image->altLength + 1, "[Image: %s]", alt);
    }
    if ((src && !image->src) || (alt && *alt && !image->alt)) {
      LOG_ERR("EHP", "OOM: image attributes");
      self->parsePhase_ = ParsePhase::Error;
      self->pauseParse();
      return;
    }
    image->widthDefined = cssStyle.hasImageWidth();
    image->heightDefined = cssStyle.hasImageHeight();
    image->cssWidth = cssStyle.imageWidth;
    image->cssHeight = cssStyle.imageHeight;
    self->imageState_ = std::move(image);
    self->parsePhase_ = ParsePhase::Image;
    self->pauseParse();
    return;
  }

  // Ruby tag handling
  if (strcmp(name, "ruby") == 0) {
    // <ruby> is an inline element: a base that follows text with no whitespace between them
    // continues the same visual word, exactly like <b>/<i> handling in endElement().
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->inRuby = true;
    self->rubyStartWordIndex = self->currentTextBlock ? static_cast<int>(self->currentTextBlock->size()) : 0;
    if (self->currentTextBlock) {
      self->currentTextBlock->ensureRubyCapacity();
    }
    self->rubyTextBuffer.clear();
    self->depth += 1;
    return;
  }
  if (strcmp(name, "rt") == 0) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->collectingRubyText = true;
    self->depth += 1;
    return;
  }

  if (VisibleTextUtils::isNonVisibleElement(name)) {
    // start skip
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Skip blocks with role="doc-pagebreak" and epub:type="pagebreak"
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "role") == 0 && strcmp(atts[i + 1], "doc-pagebreak") == 0 ||
          strcmp(atts[i], "epub:type") == 0 && strcmp(atts[i + 1], "pagebreak") == 0) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }
    }
  }

  // Detect internal <a href="..."> links (footnotes, cross-references)
  // Note: <aside epub:type="footnote"> elements are rendered as normal content
  // without special handling. Links pointing to them are collected as footnotes.
  if (strcmp(name, "a") == 0) {
    const char* href = getAttribute(atts, "href");

    bool isInternalLink = isInternalEpubLink(href);

    // Special case: javascript:void(0) links with data attributes
    // Example: <a href="javascript:void(0)"
    // data-xyz="{&quot;name&quot;:&quot;OPS/ch2.xhtml&quot;,&quot;frag&quot;:&quot;id46&quot;}">
    if (href && strncmp(href, "javascript:", 11) == 0) {
      isInternalLink = false;
      // TODO: Parse data-* attributes to extract actual href
    }

    if (isInternalLink) {
      // Footnote indices are block-relative, so linked rows use ordinary flow.
      if (self->tableDepth >= 1 && self->insideTableCell && !self->tableRowStacked) {
        self->fallbackTableRowToStacked();
      }

      // Flush buffer before style change
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      self->insideFootnoteLink = true;
      self->footnoteLinkDepth = self->depth;
      self->currentFootnoteLinkId = self->currentTextBlock ? self->currentTextBlock->addLinkTarget(href) : 0;
      self->currentFootnote.href[0] = '\0';
      if (self->currentFootnoteLinkId != 0) strcpy(self->currentFootnote.href, href);
      self->currentFootnote.number[0] = '\0';
      self->currentFootnoteLinkTextLen = 0;

      // Apply underline style to visually indicate the link.
      StyleStackEntry entry;
      entry.depth = self->depth;
      entry.hasTextDecoration = true;
      entry.textDecoration = CssTextDecoration::Underline;
      applyDirectionToEntry(entry, cssStyle);
      applyVerticalAlignToEntry(entry, cssStyle);
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();

      // Skip CSS resolution — we already handled styling for this <a> tag
      self->depth += 1;
      return;
    }
  }

  const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
  const auto userAlignmentBlockStyle = BlockStyle::fromCssStyle(
      cssStyle, emSize, static_cast<CssTextAlign>(self->paragraphAlignment), self->viewportWidth);

  if (strcmp(name, "hr") == 0) {
    auto hrBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Left, self->viewportWidth);
    if (!self->embeddedStyle) {
      hrBlockStyle.marginLeft = 0;
      hrBlockStyle.marginRight = 0;
      hrBlockStyle.marginTop = 0;
      hrBlockStyle.marginBottom = 0;
      hrBlockStyle.paddingLeft = 0;
      hrBlockStyle.paddingRight = 0;
      hrBlockStyle.paddingTop = 0;
      hrBlockStyle.paddingBottom = 0;
      hrBlockStyle.textIndentDefined = false;
      hrBlockStyle.textIndent = 0;
    }
    if (self->partWordBufferIndex > 0) self->flushPartWordBuffer();
    self->pendingRuleStyle_ = hrBlockStyle;
    self->parsePhase_ = ParsePhase::BeforeRule;
    self->pauseParse();
    self->depth += 1;
    return;
  }

  if (matches(name, HEADER_TAGS, std::size(HEADER_TAGS))) {
    self->currentCssStyle = cssStyle;
    auto headerBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Center, self->viewportWidth);
    headerBlockStyle.textAlignDefined = true;
    if (self->embeddedStyle && cssStyle.hasTextAlign()) {
      headerBlockStyle.alignment = cssStyle.textAlign;
    }
    const auto accumulated =
        self->blockStyleStack.back().getCombinedBlockStyle(headerBlockStyle, BlockStyle::CombineAxis::Horizontal);
    self->blockStyleStack.push_back(accumulated);
    self->startNewTextBlock(accumulated.withoutBottom());
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS))) {
    if (strcmp(name, "br") == 0) {
      if (self->partWordBufferIndex > 0) {
        // flush word preceding <br/> to currentTextBlock before calling startNewTextBlock
        self->flushPartWordBuffer();
      }
      // A <br> after text is a line break: start the next block with the container's
      // vertical margins stripped, matching browsers, which never apply paragraph
      // margins at a <br>. This is what keeps <br>-per-paragraph books (common CJK
      // web-novel formatting) from re-adding container spacing at every paragraph
      // and collapsing page capacity.
      // A <br> on an empty block (consecutive <br>s, or a standalone <br> between
      // blocks) is a scene-break separator: keep the container margins so deposited
      // vertical spacing survives. Either way the block is tagged so that if it
      // stays empty, startNewTextBlock injects a full line-height gap when the next
      // block opens; once text follows the tag is inert.
      // Style comes from the block style stack, not the current block, so a closed
      // element's style can't leak through (#2679).
      BlockStyle brStyle = self->blockStyleStack.back();
      if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
        brStyle = brStyle.withoutTop().withoutBottom();
      }
      brStyle.fromBrElement = true;
      self->startNewTextBlock(brStyle);
    } else {
      self->currentCssStyle = cssStyle;
      const auto accumulated = self->blockStyleStack.back().getCombinedBlockStyle(userAlignmentBlockStyle,
                                                                                  BlockStyle::CombineAxis::Horizontal);
      self->blockStyleStack.push_back(accumulated);
      self->startNewTextBlock(accumulated.withoutBottom());
      self->updateEffectiveInlineStyle();

      if (strcmp(name, "li") == 0 && self->parsePhase_ == ParsePhase::NewBlockLayout) {
        self->nextBlockBullet_ = true;
      } else if (strcmp(name, "li") == 0 && self->currentTextBlock) {
        self->currentTextBlock->addWord("\xe2\x80\xa2", EpdFontFamily::REGULAR, false, false, self->visibleTextOffset);
        self->listItemBulletOnly = true;
      }
    }
  } else if (matches(name, UNDERLINE_TAGS, std::size(UNDERLINE_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->pushDecorationStyleEntry(CssTextDecoration::Underline, cssStyle);
  } else if (matches(name, LINETHROUGH_TAGS, std::size(LINETHROUGH_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->pushDecorationStyleEntry(CssTextDecoration::LineThrough, cssStyle);
  } else if (matches(name, BOLD_TAGS, std::size(BOLD_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    // Push inline style entry for bold tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasBold = true;
    entry.bold = true;
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    applyTextDecorationToEntry(entry, cssStyle);
    applyDirectionToEntry(entry, cssStyle);
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, ITALIC_TAGS, std::size(ITALIC_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
    // Push inline style entry for italic tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasItalic = true;
    entry.italic = true;
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    applyTextDecorationToEntry(entry, cssStyle);
    applyDirectionToEntry(entry, cssStyle);
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "sup") == 0 || strcmp(name, "sub") == 0) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    StyleStackEntry entry;
    entry.depth = self->depth;
    if (strcmp(name, "sup") == 0) {
      entry.hasSup = true;
      entry.sup = true;
    } else {
      entry.hasSub = true;
      entry.sub = true;
    }
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "span") == 0 || !isHeaderOrBlock(name)) {
    // Handle span and other inline elements for CSS styling.
    const bool inheritedTableTextAlign = self->tableDepth >= 1 && cssStyle.hasTextAlign();
    if (cssStyle.hasFontWeight() || cssStyle.hasFontStyle() || cssStyle.hasTextDecoration() ||
        cssStyle.hasDirection() || cssStyle.hasVerticalAlign() || inheritedTableTextAlign) {
      // Flush buffer before style change so preceding text gets current style
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      StyleStackEntry entry;
      entry.depth = self->depth;  // Track depth for matching pop
      if (cssStyle.hasFontWeight()) {
        entry.hasBold = true;
        entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
      }
      if (cssStyle.hasFontStyle()) {
        entry.hasItalic = true;
        entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
      }
      applyTextDecorationToEntry(entry, cssStyle);
      applyDirectionToEntry(entry, cssStyle);
      entry.setsParagraphDirection = strcmp(name, "html") == 0 || strcmp(name, "body") == 0;
      if (inheritedTableTextAlign) {
        entry.hasTextAlign = true;
        entry.textAlign = cssStyle.textAlign;
      }
      applyVerticalAlignToEntry(entry, cssStyle);
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();
    }
  }

  // Unprocessed tag, just increasing depth and continue forward
  self->depth += 1;
}

void XMLCALL ChapterHtmlSlimParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  if (self->parsePhase_ == ParsePhase::Error) return;
  if (self->parsePhase_ != ParsePhase::Xml) {
    if (len <= 0) return;
    if (self->deferredTextBytes_ > SIZE_MAX - sizeof(len) - static_cast<size_t>(len)) {
      LOG_ERR("EHP", "Deferred XML text is too large");
      self->parsePhase_ = ParsePhase::Error;
      return;
    }
    const size_t bytes = self->deferredTextBytes_ + sizeof(len) + static_cast<size_t>(len);
    auto copy = makeUniqueNoThrow<char[]>(bytes);
    if (!copy) {
      LOG_ERR("EHP", "OOM: deferred XML text");
      self->parsePhase_ = ParsePhase::Error;
      return;
    }
    if (self->deferredTextBytes_) memcpy(copy.get(), self->deferredText_.get(), self->deferredTextBytes_);
    memcpy(copy.get() + self->deferredTextBytes_, &len, sizeof(len));
    memcpy(copy.get() + self->deferredTextBytes_ + sizeof(len), s, len);
    self->deferredText_ = std::move(copy);
    self->deferredTextBytes_ = bytes;
    return;
  }
  const bool countVisibleOffsets = self->insideBody && self->nonVisibleTextDepth == 0 && !self->syntheticCharacterData;
  const uint32_t callbackVisibleOffset = self->visibleTextOffset;
  if (countVisibleOffsets) {
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(s);
    const unsigned char* end = ptr + len;
    while (ptr < end) {
      utf8NextCodepoint(&ptr);
      self->visibleTextOffset++;
    }
  }

  // Nested content needs an enclosing bounded cell collector.
  if (self->tableDepth > 1 && !self->insideTableCell) {
    return;
  }

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    return;
  }

  // Collect ruby text instead of normal word processing.
  if (self->collectingRubyText) {
    self->rubyTextBuffer.append(s, len);
    return;
  }

  if (self->tableDepth == 1 && !self->insideTableCell) {
    bool onlyWhitespace = true;
    for (int i = 0; i < len; ++i) {
      if (!isWhitespace(s[i])) {
        onlyWhitespace = false;
        break;
      }
    }
    if (onlyWhitespace) {
      return;
    }
  }

  // Recreate flow storage for valid text (for example a caption) after a row.
  if (!self->currentTextBlock) {
    const BlockStyle flowStyle =
        self->blockStyleStack.empty() ? BlockStyle() : self->blockStyleStack.back().withoutBottom();
    self->currentTextBlock =
        makeUniqueNoThrow<ParsedText>(self->extraParagraphSpacing, self->hyphenationEnabled, self->focusReadingEnabled,
                                      flowStyle, self->characterWrap, self->paragraphIndent);
    if (!self->currentTextBlock) {
      LOG_ERR("EHP", "OOM: text block for character data");
      return;
    }
    self->wordsExtractedInBlock = 0;
  }

  // Collect footnote link display text (for the number label)
  // Skip whitespace and brackets to normalize noterefs like "[1]" → "1"
  if (self->insideFootnoteLink) {
    int start = 0;
    int end = len - 1;

    // Example input and output texts:
    // "     [  12  ]   " => "12"
    // "   turn to 256  " => "turn to 256"

    // Ignore leading whitespaces and left square brackets
    while (start < len && (isWhitespace(s[start]) || (s[start] == '['))) {
      ++start;
    }

    // Ignore trailing whitespaces and right square brackets
    while (end >= start && (isWhitespace(s[end]) || (s[end] == ']'))) {
      --end;
    }

    // Extract footnote link text
    for (int i = start; (self->currentFootnoteLinkTextLen < sizeof(self->currentFootnote.number) - 1) && (i <= end);
         ++i) {
      self->currentFootnote.number[self->currentFootnoteLinkTextLen++] = s[i];
    }
    self->currentFootnote.number[self->currentFootnoteLinkTextLen] = '\0';
  }

  uint32_t nextCodepointOffset = callbackVisibleOffset;
  for (int i = 0; i < len; i++) {
    const uint32_t codepointOffset = nextCodepointOffset;
    if (countVisibleOffsets && (static_cast<uint8_t>(s[i]) & 0xC0) != 0x80) {
      nextCodepointOffset++;
    }

    if (isWhitespace(s[i])) {
      // Currently looking at whitespace, if there's anything in the partWordBuffer, flush it
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      // Whitespace is a real word boundary — reset continuation state
      self->nextWordContinues = false;
      // Skip the whitespace char
      continue;
    }

    // Detect U+00A0 (non-breaking space, UTF-8: 0xC2 0xA0) or
    //        U+202F (narrow no-break space, UTF-8: 0xE2 0x80 0xAF).
    //
    // Both are rendered as a visible space but must never allow a line break around them.
    // We split the no-break space into its own word token and link the surrounding words
    // with continuation flags so the layout engine treats them as an indivisible group.
    //
    // Example: "200&#xA0;Quadratkilometer" or "200&#x202F;Quadratkilometer"
    //   Input bytes:  "200\xC2\xA0Quadratkilometer"  (or 0xE2 0x80 0xAF for U+202F)
    //   Tokens produced:
    //     [0] "200"               continues=false
    //     [1] " "                 continues=true   (attaches to "200", no gap)
    //     [2] "Quadratkilometer"  continues=true   (attaches to " ", no gap)
    //
    //   The continuation flags prevent the line-breaker from inserting a line break
    //   between "200" and "Quadratkilometer". However, "Quadratkilometer" is now a
    //   standalone word for hyphenation purposes, so Liang patterns can produce
    //   "200 Quadrat-" / "kilometer" instead of the unusable "200" / "Quadratkilometer".
    if (static_cast<uint8_t>(s[i]) == 0xC2 && i + 1 < len && static_cast<uint8_t>(s[i + 1]) == 0xA0) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->partWordVisibleOffset = codepointOffset;
      self->nextWordContinues = true;  // Attach space to previous word (no break).
      self->flushPartWordBuffer();

      self->nextWordContinues = true;  // Next real word attaches to this space (no break).

      i++;  // Skip the second byte (0xA0)
      continue;
    }

    // U+202F (narrow no-break space) — identical logic to U+00A0 above.
    if (static_cast<uint8_t>(s[i]) == 0xE2 && i + 2 < len && static_cast<uint8_t>(s[i + 1]) == 0x80 &&
        static_cast<uint8_t>(s[i + 2]) == 0xAF) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->partWordVisibleOffset = codepointOffset;
      self->nextWordContinues = true;
      self->flushPartWordBuffer();

      self->nextWordContinues = true;

      i += 2;  // Skip the remaining two bytes (0x80 0xAF)
      continue;
    }

    // Skip Zero Width No-Break Space / BOM (U+FEFF) = 0xEF 0xBB 0xBF
    const XML_Char FEFF_BYTE_1 = static_cast<XML_Char>(0xEF);
    const XML_Char FEFF_BYTE_2 = static_cast<XML_Char>(0xBB);
    const XML_Char FEFF_BYTE_3 = static_cast<XML_Char>(0xBF);

    if (s[i] == FEFF_BYTE_1) {
      // Check if the next two bytes complete the 3-byte sequence
      if ((i + 2 < len) && (s[i + 1] == FEFF_BYTE_2) && (s[i + 2] == FEFF_BYTE_3)) {
        // Sequence 0xEF 0xBB 0xBF found!
        i += 2;    // Skip the next two bytes
        continue;  // Move to the next iteration
      }
    }

    // If we're about to run out of space, then cut the word off and start a new one.
    // For CJK text (no spaces), this is the primary word-breaking mechanism.
    // We must avoid splitting multi-byte UTF-8 sequences across word boundaries,
    // otherwise the trailing bytes become orphaned continuation bytes that the
    // decoder can't interpret.
    if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
      int safeLen = utf8SafeTruncateBuffer(self->partWordBuffer, self->partWordBufferIndex);

      if (safeLen < self->partWordBufferIndex && safeLen > 0) {
        // Incomplete UTF-8 sequence at the end — save it before flushing
        int overflow = self->partWordBufferIndex - safeLen;
        uint32_t overflowVisibleOffset = self->partWordVisibleOffset;
        const unsigned char* offsetPtr = reinterpret_cast<const unsigned char*>(self->partWordBuffer);
        const unsigned char* const safeEnd = offsetPtr + safeLen;
        while (offsetPtr < safeEnd) {
          utf8NextCodepoint(&offsetPtr);
          overflowVisibleOffset++;
        }
        char saved[4];
        for (int j = 0; j < overflow; j++) {
          saved[j] = self->partWordBuffer[safeLen + j];
        }
        self->partWordBufferIndex = safeLen;
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
        for (int j = 0; j < overflow; j++) {
          self->partWordBuffer[j] = saved[j];
        }
        self->partWordBufferIndex = overflow;
        self->partWordVisibleOffset = overflowVisibleOffset;
      } else {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
    }

    if (self->partWordBufferIndex == 0) {
      self->partWordVisibleOffset = codepointOffset;
    }
    self->partWordBuffer[self->partWordBufferIndex++] = s[i];
  }

  self->softFlushIfNeeded();
}

void ChapterHtmlSlimParser::softFlushIfNeeded() {
  if (parsePhase_ == ParsePhase::Table) {
    if (tableAction == TableAction::Prefix) tableAction = TableAction::PrefixSoft;
    return;
  }
  // Keep token growth bounded: CSS-heavy spans can fragment text into many tiny
  // words, so flush earlier when embedded CSS is active. We still keep the
  // "exclude last line" behavior to preserve paragraph flow across chunks.
  const size_t blockWordCount = currentTextBlock->size();
  const size_t softFlushThreshold = embeddedStyle ? TEXT_BLOCK_SOFT_FLUSH_WORDS_WITH_CSS : TEXT_BLOCK_SOFT_FLUSH_WORDS;
  if (blockWordCount > softFlushThreshold && !inRuby) {
    LOG_DBG("EHP", "Text block soft flush (%u words)", static_cast<unsigned>(blockWordCount));
    const int horizontalInset = currentTextBlock->getBlockStyle().totalHorizontalInset();
    const uint16_t effectiveWidth =
        (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;
    if (xmlParser_) {
      parsePhase_ =
          currentTextBlock->beginLayout(fontId, effectiveWidth, false) ? ParsePhase::SoftLayout : ParsePhase::Error;
      pauseParse();
    } else {
      currentTextBlock->layoutAndExtractLines(
          renderer, fontId, effectiveWidth,
          [this](const std::shared_ptr<TextBlock>& textBlock, const uint32_t offset) {
            addLineToPage(textBlock, offset);
          },
          false);
    }
  }
}

void XMLCALL ChapterHtmlSlimParser::defaultHandlerExpand(void* userData, const XML_Char* s, const int len) {
  // Check if this looks like an entity reference (&...;)
  if (len >= 3 && s[0] == '&' && s[len - 1] == ';') {
    const char* utf8Value = lookupHtmlEntity(s, static_cast<size_t>(len));
    if (utf8Value != nullptr) {
      // Known entity: expand to its UTF-8 value
      characterData(userData, utf8Value, strlen(utf8Value));
      return;
    }
    // Unknown entity: preserve original &...; sequence
    characterData(userData, s, len);
    return;
  }
  // Not an entity we recognize - skip it
}

void XMLCALL ChapterHtmlSlimParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  if (self->parsePhase_ == ParsePhase::Error) return;
  if (self->parsePhase_ != ParsePhase::Xml) {
    if (self->deferredEnd_) {
      LOG_ERR("EHP", "Unexpected second end callback while suspended");
      self->parsePhase_ = ParsePhase::Error;
      return;
    }
    const size_t bytes = strlen(name) + 1;
    self->deferredEnd_ = makeUniqueNoThrow<char[]>(bytes);
    if (!self->deferredEnd_) {
      LOG_ERR("EHP", "OOM: deferred XML end");
      self->parsePhase_ = ParsePhase::Error;
      return;
    }
    memcpy(self->deferredEnd_.get(), name, bytes);
    return;
  }
  if (self->nonVisibleTextDepth > 0) {
    self->nonVisibleTextDepth--;
  }

  // Ruby text: </rt> distributes ruby to base words, </ruby> resets ruby state
  if (strcmp(name, "rt") == 0) {
    self->collectingRubyText = false;
    if (self->inRuby && self->currentTextBlock) {
      const int currentWordCount = static_cast<int>(self->currentTextBlock->size());
      const int baseWordCount = currentWordCount - self->rubyStartWordIndex;
      std::string cleanRuby = trimAndNormalize(self->rubyTextBuffer);
      if (!cleanRuby.empty()) {
        if (baseWordCount > 0) {
          self->currentTextBlock->setRubyGroupAt(self->rubyStartWordIndex, baseWordCount, cleanRuby);
          self->rubyStartWordIndex = currentWordCount;
        } else if (self->rubyStartWordIndex > 0) {
          int leaderIdx = self->rubyStartWordIndex - 1;
          while (leaderIdx >= 0 &&
                 (self->currentTextBlock->getWordStyleAt(leaderIdx) & EpdFontFamily::RUBY_CONTINUE) != 0) {
            leaderIdx--;
          }
          if (leaderIdx >= 0) {
            std::string prevRuby = self->currentTextBlock->getRubyTextAt(leaderIdx);
            self->currentTextBlock->setRubyForWordAt(leaderIdx, prevRuby + cleanRuby);
          }
        }
      }
    }
    self->rubyTextBuffer.clear();
    // Inline close: the next base (e.g. 字 in <ruby>漢<rt>かん</rt>字<rt>じ</rt></ruby>) joins the
    // preceding one with no space. Whitespace in the source resets this in characterData().
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->nextWordContinues = true;
    }
    self->depth -= 1;
    return;
  }
  if (strcmp(name, "ruby") == 0 && self->inRuby) {
    self->inRuby = false;
    self->rubyStartWordIndex = -1;
    self->rubyTextBuffer.clear();
    // Inline close: text following </ruby> joins the annotated base with no space.
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->nextWordContinues = true;
    }
    self->depth -= 1;
    return;
  }
  // Check if any style state will change after we decrement depth
  // If so, we MUST flush the partWordBuffer with the CURRENT style first
  // Note: depth hasn't been decremented yet, so we check against (depth - 1)
  const bool willPopStyleStack =
      !self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth - 1;
  const bool willClearBold = self->boldUntilDepth == self->depth - 1;
  const bool willClearItalic = self->italicUntilDepth == self->depth - 1;

  const bool styleWillChange = willPopStyleStack || willClearBold || willClearItalic;
  const bool headerOrBlockTag = isHeaderOrBlock(name);
  const bool tableStructuralTag = isTableStructuralTag(name);
  const bool insideSkippedSubtree = self->depth - 1 >= self->skipUntilDepth;

  if (!insideSkippedSubtree && self->tableDepth > 1 && strcmp(name, "table") == 0) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->nextWordContinues = false;
    self->tableDepth -= 1;
    self->depth -= 1;
    LOG_DBG("EHP", "nested table flattened into enclosing cell");
    return;
  }

  if (!insideSkippedSubtree && self->tableDepth >= 1 && self->insideTableCell && headerOrBlockTag) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->nextWordContinues = false;
    self->depth -= 1;
    return;
  }

  // Flush buffer with current style BEFORE any style changes
  if (self->partWordBufferIndex > 0) {
    // Flush if style will change OR if we're closing a block/structural element
    const bool isInlineTag = !headerOrBlockTag && !tableStructuralTag &&
                             !matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS)) && self->depth != 1;
    const bool shouldFlush = styleWillChange || headerOrBlockTag || matches(name, BOLD_TAGS, std::size(BOLD_TAGS)) ||
                             matches(name, ITALIC_TAGS, std::size(ITALIC_TAGS)) ||
                             matches(name, UNDERLINE_TAGS, std::size(UNDERLINE_TAGS)) ||
                             matches(name, LINETHROUGH_TAGS, std::size(LINETHROUGH_TAGS)) || tableStructuralTag ||
                             matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS)) || self->depth == 1;

    if (shouldFlush) {
      self->flushPartWordBuffer();
      // If closing an inline element, the next word fragment continues the same visual word
      if (isInlineTag) {
        self->nextWordContinues = true;
      }
    }
  }

  self->depth -= 1;

  // Closing a footnote link — create entry from collected text and href
  if (self->insideFootnoteLink && self->depth == self->footnoteLinkDepth) {
    if (self->currentFootnote.number[0] != '\0' && self->currentFootnote.href[0] != '\0') {
      FootnoteEntry entry;
      strncpy(entry.number, self->currentFootnote.number, sizeof(entry.number) - 1);
      entry.number[sizeof(entry.number) - 1] = '\0';
      strncpy(entry.href, self->currentFootnote.href, sizeof(entry.href) - 1);
      entry.href[sizeof(entry.href) - 1] = '\0';
      int wordIndex =
          self->wordsExtractedInBlock + (self->currentTextBlock ? static_cast<int>(self->currentTextBlock->size()) : 0);
      self->pendingFootnotes.push_back({wordIndex, entry});
    }
    self->insideFootnoteLink = false;
    self->currentFootnoteLinkId = 0;
  }

  // Leaving skip
  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
  }

  if (!insideSkippedSubtree && self->tableDepth == 1) {
    if (strcmp(name, "td") == 0 || strcmp(name, "th") == 0) {
      self->beginTableAction(TableAction::CloseCell);
      self->nextWordContinues = false;
    } else if (strcmp(name, "tr") == 0) {
      self->beginTableAction(TableAction::CloseRow);
      self->nextWordContinues = false;
    } else if (strcmp(name, "table") == 0) {
      self->beginTableAction(TableAction::CloseTable);
    }
  }

  // Leaving bold tag
  if (self->boldUntilDepth == self->depth) {
    self->boldUntilDepth = INT_MAX;
  }

  // Leaving italic tag
  if (self->italicUntilDepth == self->depth) {
    self->italicUntilDepth = INT_MAX;
  }

  // Pop from inline style stack if we pushed an entry at this depth
  // This handles all inline elements: b, i, u, span, etc.
  if (!self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth) {
    self->inlineStyleStack.pop_back();
    self->updateEffectiveInlineStyle();
  }

  // Clear block style when leaving header or block elements
  if (headerOrBlockTag && !insideSkippedSubtree) {
    self->currentCssStyle.reset();
    self->updateEffectiveInlineStyle();

    // br is self-closing and not a container — it doesn't push/pop the stack.
    if (strcmp(name, "br") != 0 && self->blockStyleStack.size() > 1) {
      // Apply closing element's bottom margin to the current text block so
      // container spacing appears after the element's content (on the last child),
      // not on the first child via the empty-block merge in startNewTextBlock.
      if (self->currentTextBlock) {
        const auto style = self->currentTextBlock->getBlockStyle();
        self->currentTextBlock->setBlockStyle(style.addBottom(self->blockStyleStack.back()));
      }
      self->blockStyleStack.pop_back();
      // Start a new text block with the parent style to prevent subsequent bare text
      // from inheriting the closed block style (e.g. alignment or margins).
      // Vertical margins and paddings are stripped
      self->startNewTextBlock(self->blockStyleStack.back().withoutTop().withoutBottom());
      self->updateEffectiveInlineStyle();
    }

    // </li> closes: if the bullet never got inline text (empty <li> or <li> with only
    // block children that were flushed), clear the flag so the next sibling doesn't
    // merge into this block.
    if (strcmp(name, "li") == 0) {
      self->listItemBulletOnly = false;
    }
  }
  if (strcmp(name, "body") == 0) {
    self->insideBody = false;
  }
  if (strcmp(name, "html") == 0) {
    self->htmlEnded_ = true;
  }
}

ChapterHtmlSlimParser::~ChapterHtmlSlimParser() { abortParse(); }

bool ChapterHtmlSlimParser::beginParse() {
  htmlEnded_ = false;
  parsePhase_ = ParsePhase::Xml;
  xmlStatus_ = XML_STATUS_OK;
  // Initialize block style stack with a root entry representing "no ancestor block elements".
  // The user's paragraph alignment is set as the default so child elements without explicit
  // text-align inherit it correctly through getCombinedBlockStyle.
  BlockStyle rootBlockStyle;
  rootBlockStyle.alignment = (this->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                 ? CssTextAlign::Justify
                                 : static_cast<CssTextAlign>(this->paragraphAlignment);
  blockStyleStack.clear();
  blockStyleStack.reserve(8);
  blockStyleStack.push_back(rootBlockStyle);

  tableDepth = 0;
  insideTableCell = false;
  tableRowStacked = false;
  tableRowsSpannedRemaining = 0;
  tableCellTextBytes = 0;
  tableRowCells.clear();
  for (auto& lines : tableCellLines) {
    lines.clear();
  }
  tableLineVisibleOffsets.clear();

  auto paragraphAlignmentBlockStyle = BlockStyle();
  paragraphAlignmentBlockStyle.textAlignDefined = true;
  const auto align = rootBlockStyle.alignment;
  paragraphAlignmentBlockStyle.alignment = align;
  startNewTextBlock(paragraphAlignmentBlockStyle);

  xmlParser_ = XML_ParserCreate(nullptr);
  if (!xmlParser_) {
    LOG_ERR("EHP", "Couldn't allocate memory for parser");
    return false;
  }

  // Handle HTML entities (like &nbsp;) that aren't in XML spec or DTD
  // Using DefaultHandlerExpand preserves normal entity expansion from DOCTYPE
  XML_SetDefaultHandlerExpand(xmlParser_, defaultHandlerExpand);

  if (!Storage.openFileForRead("EHP", filepath, parseFile_)) {
    destroyXmlParser(xmlParser_);
    xmlParser_ = nullptr;
    return false;
  }

  // Get file size to decide whether to show indexing popup.
  if (popupFn && parseFile_.size() >= MIN_SIZE_FOR_POPUP) {
    popupFn();
  }

  XML_SetUserData(xmlParser_, this);
  XML_SetElementHandler(xmlParser_, startElement, endElement);
  XML_SetCharacterDataHandler(xmlParser_, characterData);

  parseStartTime_ = millis();
  return true;
}

void ChapterHtmlSlimParser::pauseParse() {
  XML_ParsingStatus status;
  XML_GetParsingStatus(xmlParser_, &status);
  // Deferred callbacks run while Expat is already suspended.
  if (status.parsing == XML_PARSING && XML_StopParser(xmlParser_, XML_TRUE) == XML_STATUS_ERROR) {
    LOG_ERR("EHP", "Could not suspend XML parser");
    parsePhase_ = ParsePhase::Error;
  }
}

ChapterHtmlSlimParser::ParseStatus ChapterHtmlSlimParser::parseStep(const uint16_t maxLayoutUnits) {
  if (parsePhase_ == ParsePhase::Done) return ParseStatus::Done;
  if (parsePhase_ == ParsePhase::Error) return ParseStatus::Error;
  if (!xmlParser_) {
    LOG_ERR("EHP", "No active parse");
    return ParseStatus::Error;
  }
  if (parsePhase_ == ParsePhase::Image) {
    if (maxLayoutUnits != 0) imageSome();
    return parsePhase_ == ParsePhase::Error ? ParseStatus::Error : ParseStatus::More;
  }
  if (parsePhase_ == ParsePhase::BeforeRule) {
    if (maxLayoutUnits == 0) return ParseStatus::More;
    if (currentTextBlock) {
      const BlockStyle parent = currentTextBlock->getBlockStyle();
      startNewTextBlock(parent, ParsePhase::Rule);
    }
    if (parsePhase_ == ParsePhase::BeforeRule) parsePhase_ = ParsePhase::Rule;
    return parsePhase_ == ParsePhase::Error ? ParseStatus::Error : ParseStatus::More;
  }
  if (parsePhase_ == ParsePhase::Rule) {
    if (maxLayoutUnits == 0) return ParseStatus::More;
    emitHorizontalRule(pendingRuleStyle_);
    if (parsePhase_ == ParsePhase::Error) return ParseStatus::Error;
    parsePhase_ = ParsePhase::Xml;
    return ParseStatus::More;
  }
  if (parsePhase_ == ParsePhase::Table) {
    tableSome(maxLayoutUnits);
    return parsePhase_ == ParsePhase::Error ? ParseStatus::Error : ParseStatus::More;
  }
  if (parsePhase_ == ParsePhase::SoftLayout) {
    if (currentTextBlock->layoutSome(
            renderer,
            [this](const std::shared_ptr<TextBlock>& line, const uint32_t offset) { addLineToPage(line, offset); },
            maxLayoutUnits)) {
      parsePhase_ = ParsePhase::Xml;
    }
    return ParseStatus::More;
  }
  if (parsePhase_ == ParsePhase::NewBlockLayout) {
    if (makePagesSome(maxLayoutUnits)) {
      parsePhase_ = nextBlockPhase_;
      createTextBlock(nextBlockStyle_);
      if (parsePhase_ == ParsePhase::Error) return ParseStatus::Error;
      // Ordinary tags have already updated their style stack before yielding.
      if (nextBlockPhase_ == ParsePhase::Xml) updateEffectiveInlineStyle();
      if (nextBlockBullet_) {
        currentTextBlock->addWord("\xe2\x80\xa2", EpdFontFamily::REGULAR, false, false, visibleTextOffset);
        listItemBulletOnly = true;
      }
      nextBlockBullet_ = false;
    }
    return ParseStatus::More;
  }
  if (parsePhase_ == ParsePhase::TrailingLayout) {
    if (makePagesSome(maxLayoutUnits)) parsePhase_ = ParsePhase::TrailingPage;
    return ParseStatus::More;
  }
  if (parsePhase_ == ParsePhase::TrailingPage) {
    if (currentTextBlock) {
      if (!pendingAnchorId.empty()) {
        anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
        pendingAnchorId.clear();
      }
      setCurrentPageVisibleOffset(visibleTextOffset);
      completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
      completedPageCount++;
      currentTextBlock.reset();
    }
    parsePhase_ = ParsePhase::Done;
    return ParseStatus::Done;
  }

  if (deferredTextCursor_ < deferredTextBytes_) {
    int len;
    memcpy(&len, deferredText_.get() + deferredTextCursor_, sizeof(len));
    deferredTextCursor_ += sizeof(len);
    characterData(this, deferredText_.get() + deferredTextCursor_, len);
    deferredTextCursor_ += len;
    if (deferredTextCursor_ == deferredTextBytes_) {
      deferredText_.reset();
      deferredTextBytes_ = deferredTextCursor_ = 0;
    }
    return parsePhase_ == ParsePhase::Error ? ParseStatus::Error : ParseStatus::More;
  }
  if (deferredEnd_) {
    auto name = std::move(deferredEnd_);
    endElement(this, name.get());
    return parsePhase_ == ParsePhase::Error ? ParseStatus::Error : ParseStatus::More;
  }

  if (xmlStatus_ == XML_STATUS_SUSPENDED) {
    xmlStatus_ = XML_ResumeParser(xmlParser_);
  } else {
    void* const buf = XML_GetBuffer(xmlParser_, PARSE_BUFFER_SIZE);
    if (!buf) {
      LOG_ERR("EHP", "Couldn't allocate memory for buffer");
      return ParseStatus::Error;
    }
    const int len = parseFile_.read(buf, PARSE_BUFFER_SIZE);
    if (len < 0 || (len == 0 && parseFile_.available() > 0)) {
      LOG_ERR("EHP", "File read error");
      return ParseStatus::Error;
    }
    xmlStatus_ = XML_ParseBuffer(xmlParser_, len, parseFile_.available() == 0);
  }

  if (parsePhase_ == ParsePhase::Error) return ParseStatus::Error;
  if (xmlStatus_ == XML_STATUS_SUSPENDED) return ParseStatus::More;
  if (xmlStatus_ == XML_STATUS_ERROR) {
    if (!htmlEnded_) {
      LOG_ERR("EHP", "Parse error at line %lu:\n%s", XML_GetCurrentLineNumber(xmlParser_),
              XML_ErrorString(XML_GetErrorCode(xmlParser_)));
      return ParseStatus::Error;
    }
    LOG_DBG("EHP", "Ignoring trailing data after </html>: %s", XML_ErrorString(XML_GetErrorCode(xmlParser_)));
  }
  XML_ParsingStatus status;
  XML_GetParsingStatus(xmlParser_, &status);
  if (status.finalBuffer || xmlStatus_ == XML_STATUS_ERROR) {
    if (currentTextBlock && !beginMakePages()) return ParseStatus::Error;
    parsePhase_ = currentTextBlock ? ParsePhase::TrailingLayout : ParsePhase::TrailingPage;
  }
  return ParseStatus::More;
}

void ChapterHtmlSlimParser::abortParse() {
  if (xmlParser_) {
    destroyXmlParser(xmlParser_);
    xmlParser_ = nullptr;
  }
  // Only close the file if it was successfully opened in beginParse()
  if (parseFile_.isOpen()) {
    parseFile_.close();
  }
  currentTextBlock.reset();
  currentPage.reset();
  tableRowCells.clear();
  tableActiveCell.reset();
  imageState_.reset();
  for (auto& lines : tableCellLines) lines.clear();
  deferredText_.reset();
  deferredTextBytes_ = deferredTextCursor_ = 0;
  deferredEnd_.reset();
  parsePhase_ = ParsePhase::Xml;
}

bool ChapterHtmlSlimParser::finishParse() {
  if (parsePhase_ != ParsePhase::Done) {
    LOG_ERR("EHP", "Cannot finish an incomplete parse");
    return false;
  }
  if (xmlParser_) {
    LOG_DBG("EHP", "Time to parse and build pages: %lu ms", millis() - parseStartTime_);
    destroyXmlParser(xmlParser_);
    xmlParser_ = nullptr;
  }
  parseFile_ = HalFile{};

  return true;
}

bool ChapterHtmlSlimParser::parseAndBuildPages() {
  if (!beginParse()) {
    return false;
  }
  for (;;) {
    const ParseStatus status = parseStep();
    if (status == ParseStatus::Error) {
      abortParse();
      return false;
    }
    if (status == ParseStatus::Done) {
      break;
    }
  }
  return finishParse();
}

void ChapterHtmlSlimParser::addLineToPage(std::shared_ptr<TextBlock> line, const uint32_t visibleOffset) {
  const int lineHeight =
      renderer.getLineHeight(fontId, lineCompression) + line->getRubyShift(renderer.getFontAscenderSize(fontId));

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
    currentPageVisibleOffsetSet = false;
  }

  if (currentPageNextY + lineHeight > viewportHeight) {
    setCurrentPageVisibleOffset(visibleOffset);
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
    completedPageCount++;
    currentPage.reset(new Page());
    currentPageNextY = 0;
    currentPageVisibleOffsetSet = false;
  }
  setCurrentPageVisibleOffset(visibleOffset);

  // Track cumulative words to assign footnotes to the page containing their anchor
  wordsExtractedInBlock += line->wordCount();
  auto footnoteIt = pendingFootnotes.begin();
  while (footnoteIt != pendingFootnotes.end() && footnoteIt->first <= wordsExtractedInBlock) {
    currentPage->addFootnote(footnoteIt->second.number, footnoteIt->second.href);
    ++footnoteIt;
  }
  pendingFootnotes.erase(pendingFootnotes.begin(), footnoteIt);

  // Apply horizontal left inset (margin + padding) as x position offset
  const int16_t xOffset = line->getBlockStyle().leftInset();
  const int rubyShift = line->getRubyShift(renderer.getFontAscenderSize(fontId));
  const int baseLineHeight = renderer.getLineHeight(fontId, lineCompression);
  for (const auto& link : line->takeLinkSpans()) {
    if (!currentPage->addLink(link.href, static_cast<int16_t>(xOffset + link.x),
                              static_cast<int16_t>(currentPageNextY + rubyShift - link.topLift), link.width,
                              static_cast<int16_t>(baseLineHeight + link.topLift))) {
      LOG_DBG("EHP", "Dropped page link: %.48s", link.href);
    }
  }
  currentPage->elements.push_back(std::make_shared<PageLine>(line, xOffset, currentPageNextY));
  currentPageNextY += lineHeight;
}

bool ChapterHtmlSlimParser::beginMakePages() {
  if (!currentTextBlock) {
    LOG_ERR("EHP", "!! No text block to make pages for !!");
    return false;
  }

  if (!currentPage) {
    currentPage = makeUniqueNoThrow<Page>();
    if (!currentPage) {
      LOG_ERR("EHP", "OOM: paragraph page");
      return false;
    }
    currentPageNextY = 0;
    currentPageVisibleOffsetSet = false;
  }

  // Apply top spacing before the paragraph (stored in pixels)
  const BlockStyle& blockStyle = currentTextBlock->getBlockStyle();
  if (blockStyle.marginTop > 0) {
    currentPageNextY += blockStyle.marginTop;
  }
  if (blockStyle.paddingTop > 0) {
    currentPageNextY += blockStyle.paddingTop;
  }

  // Calculate effective width accounting for horizontal margins/padding
  const int horizontalInset = blockStyle.totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;

  return currentTextBlock->beginLayout(fontId, effectiveWidth);
}

bool ChapterHtmlSlimParser::makePagesSome(const uint16_t maxUnits) {
  if (!currentTextBlock->layoutSome(
          renderer,
          [this](const std::shared_ptr<TextBlock>& textBlock, const uint32_t offset) {
            addLineToPage(textBlock, offset);
          },
          maxUnits)) {
    return false;
  }

  // Fallback: transfer any remaining pending footnotes to current page.
  // Normally addLineToPage handles this via word-index tracking, but this catches
  // edge cases where a footnote's word index equals the exact block size.
  if (!pendingFootnotes.empty() && currentPage) {
    for (const auto& [idx, fn] : pendingFootnotes) {
      currentPage->addFootnote(fn.number, fn.href);
    }
    pendingFootnotes.clear();
  }

  // Apply bottom spacing after the paragraph (stored in pixels)
  const BlockStyle& blockStyle = currentTextBlock->getBlockStyle();
  if (blockStyle.marginBottom > 0) {
    currentPageNextY += blockStyle.marginBottom;
  }
  if (blockStyle.paddingBottom > 0) {
    currentPageNextY += blockStyle.paddingBottom;
  }

  // Extra paragraph spacing if enabled (default behavior)
  if (extraParagraphSpacing) {
    currentPageNextY += renderer.getLineHeight(fontId, lineCompression) / 2;
  }
  return true;
}
