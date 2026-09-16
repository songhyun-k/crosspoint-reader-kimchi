#include <Epub/Page.h>
#include <Epub/ParsedText.h>
#include <Epub/ReaderRenderSpec.h>
#include <Epub/Section.h>
#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <Utf8.h>
#include <builtinFonts/kimchi_batang_14_regular.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <new>
#include <string>
#include <vector>

namespace {
bool failNextArray = false;
using Style = EpdFontFamily::Style;
constexpr Style R = EpdFontFamily::REGULAR;
constexpr Style B = EpdFontFamily::BOLD;
struct Line {
  std::shared_ptr<TextBlock> block;
  uint32_t offset;
};
std::string contents(const std::vector<Line>& lines) {
  std::string result;
  for (const auto& line : lines) {
    for (uint16_t i = 0; i < line.block->wordCount(); ++i) result += line.block->wordText(i);
  }
  return result;
}
BlockStyle zeroIndent(CssTextAlign alignment = CssTextAlign::Justify) {
  BlockStyle style;
  style.alignment = alignment;
  style.textIndentDefined = true;
  return style;
}

class KoreanLayoutTest : public testing::Test {
 protected:
  HalDisplay panel;
  GfxRenderer renderer{panel};
  EpdFont body{&kimchi_batang_14_regular};
  std::vector<Line> lines;
  std::vector<std::unique_ptr<Page>> pages;
  void SetUp() override {
    storage_test::reset();
    failNextArray = false;
    renderer.begin();
    renderer.insertFont(1, EpdFontFamily(&body));
    renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  }
  void TearDown() override { failNextArray = false; }
  void layout(ParsedText& text, uint16_t width, bool last = true) {
    text.layoutAndExtractLines(
        renderer, 1, width,
        [this](std::shared_ptr<TextBlock> block, uint32_t offset) { lines.push_back({std::move(block), offset}); },
        last);
  }
  bool parse(const std::string& content, uint16_t width = 220, uint16_t height = 150, bool characterWrap = true,
             bool paragraphIndent = false, bool embeddedStyle = true) {
    const std::string path = "chapter.xhtml";
    const std::string html = "<html><body>" + content + "</body></html>";
    storage_test::files[path] = std::vector<uint8_t>(html.begin(), html.end());
    ChapterHtmlSlimParser parser(
        nullptr, path, renderer, 1, 1.0f, true, 0, width, height, false, false,
        [this](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t offset) {
          if (!page) return;
          for (const auto& element : page->elements) {
            if (element->getTag() == TAG_PageLine) {
              lines.push_back({static_cast<const PageLine&>(*element).getBlock(), offset});
            }
          }
          pages.push_back(std::move(page));
        },
        embeddedStyle, "", "", 0, {}, nullptr, nullptr, characterWrap, paragraphIndent);
    return parser.parseAndBuildPages();
  }
};
}  // namespace

// Only the fallible array allocation used by TextBlock's arena is injected.
// Successful allocations still use the matching standard new[]/delete[] pair.
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  if (failNextArray) {
    failNextArray = false;
    return nullptr;
  }
  return ::operator new[](size);
}

TEST_F(KoreanLayoutTest, RepeatedSuppressedTrailingLinesRetainEverySourceCharacter) {
  ParsedText text(true, false, false, zeroIndent(), true);
  text.addWord("한글문단", B, false, false, 65530);
  const size_t tokens = text.size();
  for (int i = 0; i < 8; ++i) {
    layout(text, 480, false);
    EXPECT_EQ(text.size(), tokens);
    EXPECT_TRUE(lines.empty());
  }
  text.addWord("이어짐", R, false, true, 65534);
  layout(text, 480);
  EXPECT_EQ(contents(lines), "한글문단이어짐");
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines.front().offset, 65530u);
  EXPECT_TRUE(text.isEmpty());
  EXPECT_EQ(lines[0].block->wordStyle(0), B);
  EXPECT_EQ(lines[0].block->wordStyle(lines[0].block->wordCount() - 1), R);
}

TEST_F(KoreanLayoutTest, ThousandsOfChunkedKoreanTokensDoNotDisappearOrDuplicate) {
  ParsedText text(true, false, false, zeroIndent(), true);
  std::string expected;
  uint32_t offset = 90000;
  for (int chunk = 0; chunk < 180; ++chunk) {
    const std::string part = "가나다라마바사아자차카타파하";
    expected += part;
    text.addWord(part, chunk % 2 ? B : R, false, true, offset);
    const auto* cursor = reinterpret_cast<const uint8_t*>(part.c_str());
    while (utf8NextCodepoint(&cursor)) ++offset;
    layout(text, 173, false);
    ASSERT_FALSE(text.isEmpty());
    EXPECT_LE(text.size(), 8u);
  }
  layout(text, 173);
  EXPECT_EQ(contents(lines), expected);
  for (size_t i = 1; i < lines.size(); ++i) EXPECT_GT(lines[i].offset, lines[i - 1].offset);
}

TEST_F(KoreanLayoutTest, GluedCjkHasNoInsertedSpacingButRealGapsReachTheRightEdge) {
  ParsedText text(true, false, false, zeroIndent(), true);
  for (int i = 0; i < 15; ++i) text.addWord("가나다", R, false, false, i * 4);
  layout(text, 227);
  ASSERT_GT(lines.size(), 1u);
  const auto& block = *lines[0].block;
  for (uint16_t i = 1; i < block.wordCount(); ++i) {
    if (std::string(block.wordText(i)) == "나" || std::string(block.wordText(i)) == "다") {
      EXPECT_EQ(block.wordXpos(i), block.wordXpos(i - 1) + renderer.getTextAdvanceX(1, block.wordText(i - 1), R));
    }
  }
  const uint16_t last = block.wordCount() - 1;
  EXPECT_EQ(block.wordXpos(last) + renderer.getTextAdvanceX(1, block.wordText(last), R), 227);
}

TEST_F(KoreanLayoutTest, TurningCharacterWrapOffRestoresUpstreamCjkGapExpansion) {
  const std::string source = "가나다라마바사아자차카타파하";
  const int natural = renderer.getTextAdvanceX(1, "가", R);
  for (const bool enabled : {true, false}) {
    lines.clear();
    ParsedText text(true, false, false, zeroIndent(), enabled);
    text.addWord(source, R);
    layout(text, natural * 3 + 11);
    ASSERT_GT(lines.size(), 1u);
    const auto& first = *lines[0].block;
    ASSERT_GT(first.wordCount(), 1);
    if (enabled)
      EXPECT_EQ(first.wordXpos(1) - first.wordXpos(0), natural);
    else
      EXPECT_GT(first.wordXpos(1) - first.wordXpos(0), natural);
    EXPECT_EQ(contents(lines), source);
  }
}

TEST_F(KoreanLayoutTest, NonJustifiedAlignmentDoesNotUseTheKoreanLayoutMode) {
  std::vector<int16_t> reference;
  for (const bool enabled : {false, true}) {
    lines.clear();
    ParsedText text(true, false, false, zeroIndent(CssTextAlign::Left), enabled);
    text.addWord("가나다라마바사아자차카타파하", R);
    layout(text, 110);
    std::vector<int16_t> positions;
    for (const auto& line : lines) {
      for (uint16_t i = 0; i < line.block->wordCount(); ++i) positions.push_back(line.block->wordXpos(i));
    }
    if (!enabled)
      reference = positions;
    else
      EXPECT_EQ(positions, reference);
  }
}

TEST_F(KoreanLayoutTest, SettingChangesJustifiedLatinCharacterFillingWithoutInsertedHyphens) {
  const std::string source = "ABCDEFGH";
  ParsedText text(true, false, false, zeroIndent(), true);
  text.addWord(source, R);
  layout(text, renderer.getTextAdvanceX(1, "ABC", R) + 1, false);
  ASSERT_FALSE(lines.empty());
  layout(text, 300);
  EXPECT_EQ(contents(lines), source);
  for (const auto& line : lines) EXPECT_EQ(contents({line}).find('-'), std::string::npos);
}

TEST_F(KoreanLayoutTest, ArenaFailureDoesNotEmitAnInvalidBlock) {
  ParsedText text(true, false, false, zeroIndent(), true);
  text.addWord("한글문단", R);
  failNextArray = true;
  layout(text, 480);
  EXPECT_TRUE(lines.empty());
  EXPECT_TRUE(text.isEmpty());  // Upstream drops a line when its arena cannot be allocated.
  text.addWord("다음문단", R);
  layout(text, 480);
  EXPECT_EQ(contents(lines), "다음문단");
}

TEST_F(KoreanLayoutTest, LinksFocusStylesRubyAndTextBlockCacheSurviveLineExtraction) {
  ParsedText text(true, false, true, zeroIndent(), true);
  const uint8_t link = text.addLinkTarget("chapter.xhtml#note");
  text.addWord("한글", B, false, false, 100, link);
  text.setRubyGroupAt(0, text.size(), "한국어");
  text.addWord("reading", R, false, false, 103);
  layout(text, 480, false);
  EXPECT_TRUE(lines.empty());
  layout(text, 480);
  ASSERT_EQ(lines.size(), 1u);
  auto& block = *lines[0].block;
  EXPECT_TRUE(block.hasRuby());
  EXPECT_FALSE(block.getRubyTexts()[0].empty());
  const auto spans = block.takeLinkSpans();
  ASSERT_EQ(spans.size(), 1u);
  EXPECT_STREQ(spans[0].href, "chapter.xhtml#note");
  EXPECT_GT(spans[0].width, 0);
  EXPECT_GT(block.focusBoundary(block.wordCount() - 1), 0);
  HalFile file;
  ASSERT_TRUE(Storage.openFileForWrite("test", "line.bin", file));
  ASSERT_TRUE(block.serialize(file));
  ASSERT_TRUE(file.seek(0));
  const auto restored = TextBlock::deserialize(file);
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->getRubyTexts(), block.getRubyTexts());
  for (uint16_t i = 0; i < block.wordCount(); ++i) {
    EXPECT_STREQ(restored->wordText(i), block.wordText(i));
    EXPECT_EQ(restored->wordXpos(i), block.wordXpos(i));
    EXPECT_EQ(restored->wordStyle(i), block.wordStyle(i));
    EXPECT_EQ(restored->focusBoundary(i), block.focusBoundary(i));
  }
}

TEST_F(KoreanLayoutTest, RealXmlParserSoftFlushKeepsLongMixedParagraphsAndLinks) {
  std::string html = "<p style='text-indent:0'>";
  std::string expected;
  for (int i = 0; i < 220; ++i) {
    html += "가나다<a href='#note'><b>라마바</b></a>사아자 ";
    expected += "가나다라마바사아자";
  }
  html += "</p><p id='note'>끝</p>";
  expected += "끝";
  ASSERT_TRUE(parse(html));
  EXPECT_EQ(contents(lines), expected);
  EXPECT_GT(pages.size(), 10u);
  EXPECT_TRUE(std::any_of(pages.begin(), pages.end(), [](const auto& page) { return !page->links.empty(); }));
  for (size_t i = 1; i < lines.size(); ++i) EXPECT_GE(lines[i].offset, lines[i - 1].offset);
}

TEST_F(KoreanLayoutTest, RealXmlParserKeepsRubyAndGridTableText) {
  ASSERT_TRUE(
      parse("<p><ruby>漢字<rt>한자</rt></ruby>본문</p><table><tr><td>왼쪽</td><td>오른쪽</td>"
            "</tr><tr><td>하나</td><td>둘</td></tr></table><p>마지막</p>",
            480));
  EXPECT_EQ(contents(lines), "漢字본문왼쪽오른쪽하나둘마지막");
  EXPECT_TRUE(std::any_of(lines.begin(), lines.end(), [](const Line& line) { return line.block->hasRuby(); }));
}

TEST_F(KoreanLayoutTest, RealSectionCacheRebuildsOnSettingChangeAndKeepsVisiblePositionLookups) {
  std::string html = "<html><body><p style='text-indent:0'>";
  for (int i = 0; i < 100; ++i) html += "한글 문단과 Englishreading을 함께 읽습니다. ";
  html += "</p><p id='end'>끝</p></body></html>";
  storage_test::files["cache/html/0.html"] = std::vector<uint8_t>(html.begin(), html.end());
  ReaderRenderSpec spec;
  spec.fontId = 1;
  spec.viewportWidth = 227;
  spec.viewportHeight = 160;
  spec.extraParagraphSpacing = true;
  const auto epub = std::make_shared<Epub>();
  {
    Section built(epub, 0, renderer);
    ASSERT_TRUE(built.createSectionFile(spec));
    EXPECT_TRUE(built.isBuildComplete());
    ASSERT_GT(built.pageCount, 5);
    auto page = built.loadPage(2);
    ASSERT_NE(page, nullptr);
    const auto offset = built.getVisibleTextOffsetForPage(2);
    ASSERT_TRUE(offset.has_value());
    EXPECT_EQ(built.getPageForVisibleTextOffset(*offset), 2);
    EXPECT_EQ(built.getCachedPageCount(), built.pageCount);
    EXPECT_TRUE(built.getPageForAnchor("end").has_value());
  }
  {
    Section reloaded(epub, 0, renderer);
    ASSERT_TRUE(reloaded.loadSectionFile(spec));
    ASSERT_NE(reloaded.loadPage(2), nullptr);
  }
  {
    Section changed(epub, 0, renderer);
    spec.characterWrap = false;
    EXPECT_FALSE(changed.loadSectionFile(spec));
    EXPECT_FALSE(Storage.exists("cache/sections/0.bin"));
    EXPECT_TRUE(Storage.exists("cache/html/0.html"));
    ASSERT_TRUE(changed.createSectionFile(spec));
    EXPECT_TRUE(changed.loadSectionFile(spec));
  }
}

TEST_F(KoreanLayoutTest, RealPartialCacheAlsoRejectsAChangedCharacterWrapSetting) {
  std::string html = "<html><body><p>";
  for (int i = 0; i < 2000; ++i) html += "가나다라마바사 아자차카타파하 ";
  html += "</p></body></html>";
  storage_test::files["cache/html/0.html"] = std::vector<uint8_t>(html.begin(), html.end());
  const auto epub = std::make_shared<Epub>();
  ReaderRenderSpec spec;
  spec.fontId = 1;
  spec.viewportWidth = 227;
  spec.viewportHeight = 160;
  {
    Section partial(epub, 0, renderer);
    ASSERT_TRUE(partial.startBuild(spec));
    ASSERT_TRUE(partial.buildSomeMore(1));
    ASSERT_FALSE(partial.isBuildComplete());
    partial.suspendBuild();
  }
  Section reopened(epub, 0, renderer);
  ASSERT_TRUE(reopened.loadSectionFile(spec));
  EXPECT_TRUE(reopened.isPartial());
  ASSERT_NE(reopened.loadPage(0), nullptr);
  const auto saved = storage_test::files.at("cache/sections/0.bin");
  spec.characterWrap = false;
  EXPECT_FALSE(reopened.loadSectionFile(spec));
  storage_test::files["cache/sections/0.bin"] = saved;
  spec.characterWrap = true;
  spec.paragraphIndent = true;
  EXPECT_FALSE(reopened.loadSectionFile(spec));
}

TEST_F(KoreanLayoutTest, ParagraphIndentDefaultsOffAndIsIndependentOfParagraphSpacing) {
  const int indentWidth = renderer.getTextAdvanceX(1, "\xE3\x80\x80", R);
  ASSERT_GT(indentWidth, 0);
  for (bool spacing : {false, true}) {
    for (bool indent : {false, true}) {
      for (bool wrap : {false, true}) {
        lines.clear();
        ParsedText text(spacing, false, false, BlockStyle{}, wrap, indent);
        text.addWord("가나다라마바사아자차카타파하", R);
        layout(text, 125);
        ASSERT_GT(lines.size(), 1u);
        EXPECT_EQ(lines[0].block->wordXpos(0), indent ? indentWidth : 0);
        for (size_t i = 1; i < lines.size(); ++i) EXPECT_EQ(lines[i].block->wordXpos(0), 0);
        EXPECT_EQ(contents(lines), "가나다라마바사아자차카타파하");
      }
    }
  }
}

TEST_F(KoreanLayoutTest, ExplicitCssZeroPositiveAndHangingIndentNeverDoubleWithUserIndent) {
  for (bool spacing : {false, true}) {
    for (bool indent : {false, true}) {
      for (int css : {0, 17, -7}) {
        lines.clear();
        BlockStyle style = zeroIndent();
        style.textIndent = css;
        ParsedText text(spacing, false, false, style, true, indent);
        text.addWord("가나다라마바사아자차카타파하", R);
        layout(text, 125);
        ASSERT_GT(lines.size(), 1u);
        EXPECT_EQ(lines[0].block->wordXpos(0), css);
        EXPECT_EQ(lines[1].block->wordXpos(0), 0);
      }
    }
  }
}

TEST_F(KoreanLayoutTest, SoftFlushDoesNotRepeatIndentOrChangeVisibleOffsets) {
  ParsedText text(false, false, false, BlockStyle{}, true, true);
  const int indentWidth = renderer.getTextAdvanceX(1, "\xE3\x80\x80", R);
  text.addWord("가나다", R, false, false, 1200);
  layout(text, 480, false);
  ASSERT_TRUE(lines.empty());
  for (int i = 0; i < 25; ++i) {
    text.addWord("라마바사아자", R, false, true, 1203 + i * 6);
    layout(text, 125, false);
  }
  layout(text, 125);
  ASSERT_GT(lines.size(), 20u);
  EXPECT_EQ(lines[0].block->wordXpos(0), indentWidth);
  EXPECT_EQ(lines[0].offset, 1200u);
  for (size_t i = 1; i < lines.size(); ++i) EXPECT_EQ(lines[i].block->wordXpos(0), 0);
  lines.clear();
  text.setBlockStyle(BlockStyle{});  // empty parser object reused for the next paragraph
  text.addWord("새문단", R, false, false, 1500);
  layout(text, 480);
  EXPECT_EQ(lines[0].block->wordXpos(0), indentWidth);
  EXPECT_EQ(lines[0].offset, 1500u);
}

TEST_F(KoreanLayoutTest, HtmlBreakContinuesItsParagraphButANewParagraphGetsItsOwnIndent) {
  ASSERT_TRUE(parse("<p style='text-indent:17px'>가나다<br/>라마바</p><p>사아자</p>", 480, 800, true, true));
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0].block->wordXpos(0), 17);
  EXPECT_EQ(lines[1].block->wordXpos(0), 0);
  EXPECT_EQ(lines[2].block->wordXpos(0), renderer.getTextAdvanceX(1, "\xE3\x80\x80", R));
  EXPECT_EQ(contents(lines), "가나다라마바사아자");
}

TEST_F(KoreanLayoutTest, InlineCssIndentWorksWithoutAnExternalStylesheetAndRespectsStyleToggle) {
  for (const int indent : {0, 17, -7}) {
    for (const bool styles : {true, false}) {
      lines.clear();
      const std::string html = "<p style='text-indent:" + std::to_string(indent) +
                               "px;display:none;font-weight:bold;text-align:right;margin-left:50px'>한글문단</p>";
      ASSERT_TRUE(parse(html, 480, 800, true, true, styles));
      ASSERT_EQ(lines.size(), 1u);
      EXPECT_EQ(lines[0].block->wordXpos(0), styles ? indent : renderer.getTextAdvanceX(1, "\xE3\x80\x80", R));
      EXPECT_EQ(lines[0].block->wordStyle(0), R);
    }
  }
}

TEST_F(KoreanLayoutTest, RealSectionInvalidatesIndentAndPreservesContentPositionAcrossRepagination) {
  std::string html = "<html><body>";
  for (int i = 0; i < 45; ++i) html += "<p>가나다라마바사아자차카타파하 가나다라마바</p>";
  html += "</body></html>";
  storage_test::files["cache/html/0.html"] = std::vector<uint8_t>(html.begin(), html.end());
  const auto epub = std::make_shared<Epub>();
  ReaderRenderSpec spec;
  spec.fontId = 1;
  spec.viewportWidth = 227;
  spec.viewportHeight = 170;
  uint32_t position = 0;
  {
    Section original(epub, 0, renderer);
    ASSERT_TRUE(original.createSectionFile(spec));
    position = original.getVisibleTextOffsetForPage(3).value();
    auto first = original.loadPage(0);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(static_cast<PageLine&>(*first->elements[0]).getBlock()->wordXpos(0), 0);
  }
  spec.paragraphIndent = true;
  Section changed(epub, 0, renderer);
  EXPECT_FALSE(changed.loadSectionFile(spec));
  ASSERT_TRUE(changed.createSectionFile(spec));
  ASSERT_TRUE(changed.loadSectionFile(spec));
  const auto page = changed.getPageForVisibleTextOffset(position);
  ASSERT_TRUE(page.has_value());
  EXPECT_LE(changed.getVisibleTextOffsetForPage(*page).value(), position);
  if (*page + 1 < changed.pageCount) EXPECT_GT(changed.getVisibleTextOffsetForPage(*page + 1).value(), position);
  auto first = changed.loadPage(0);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(static_cast<PageLine&>(*first->elements[0]).getBlock()->wordXpos(0),
            renderer.getTextAdvanceX(1, "\xE3\x80\x80", R));
}

TEST_F(KoreanLayoutTest, IdleChunksPrewarmBuiltPagesAndFinishWithoutMovingTheReader) {
  std::string html = "<html><body><!--" + std::string(32768, 'x') + "-->";
  for (int i = 0; i < 2000; ++i) html += "<p>가나다라마바사 함께 읽는 한글 문단입니다.</p>";
  html += "</body></html>";
  storage_test::files["cache/html/0.html"] = std::vector<uint8_t>(html.begin(), html.end());
  FontDecompressor decompressor;
  FontCacheManager manager(renderer.getFontMap(), renderer.getSdCardFonts());
  manager.setFontDecompressor(&decompressor);
  renderer.setFontCacheManager(&manager);
  struct ClearManager {
    GfxRenderer& renderer;
    ~ClearManager() { renderer.setFontCacheManager(nullptr); }
  } guard{renderer};
  const auto epub = std::make_shared<Epub>();
  ReaderRenderSpec spec;
  spec.fontId = 1;
  spec.viewportWidth = 227;
  spec.viewportHeight = 160;
  Section section(epub, 0, renderer);
  ASSERT_TRUE(section.startBuild(spec));
  const size_t reads = storage_test::reads;
  ASSERT_TRUE(section.buildSomeMore(2, 1));
  EXPECT_EQ(storage_test::reads, reads + 1);  // yields even through a long comment
  EXPECT_EQ(section.pageCount, 0);
  ASSERT_TRUE(section.isBuilding());
  for (int tick = 0; tick < 100 && section.pageCount < 2; ++tick) {
    ASSERT_TRUE(section.buildSomeMore(2, 1));
  }
  ASSERT_GE(section.pageCount, 2);
  ASSERT_TRUE(section.isBuilding());
  const auto next = section.loadPage(1);
  ASSERT_NE(next, nullptr);
  {
    auto scope = manager.createPrewarmScope();
    next->render(renderer, 1, 0, 0);
    scope.endScanAndPrewarm();
  }
  {
    HalFile file;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", "before.page", file));
    ASSERT_TRUE(next->serialize(file));
  }
  for (int tick = 0; tick < 1000 && section.isBuilding(); ++tick) {
    ASSERT_TRUE(section.buildSomeMore(2, 1));
  }
  EXPECT_TRUE(section.isBuildComplete());
  EXPECT_FALSE(section.isBuilding());
  EXPECT_EQ(section.currentPage, 0);  // finishes beyond the five-page window while stationary
  ASSERT_GT(section.pageCount, 5);
  Section reopened(epub, 0, renderer);
  ASSERT_TRUE(reopened.loadSectionFile(spec));
  EXPECT_FALSE(reopened.isPartial());
  const auto committed = reopened.loadPage(1);
  ASSERT_NE(committed, nullptr);
  {
    HalFile file;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", "after.page", file));
    ASSERT_TRUE(committed->serialize(file));
  }
  EXPECT_EQ(storage_test::files.at("before.page"), storage_test::files.at("after.page"));
}

TEST_F(KoreanLayoutTest, CjkBoundaryTokensKeepPunctuationMarksAndSoftHyphens) {
  struct Case {
    std::string source;
    size_t tokens;
    std::string visible;
  };
  const Case cases[] = {{"", 0, ""},
                        {"가", 1, "가"},
                        {"ASCII", 1, "ASCII"},
                        {"(가나)", 2, "(가나)"},
                        {"가(나)다", 3, "가(나)다"},
                        {"“가나”", 2, "“가나”"},
                        {"漢字かな", 4, "漢字かな"},
                        {"ab가cd나", 4, "ab가cd나"},
                        {"가\xCC\x81나", 2, "가\xCC\x81나"},
                        {"가\xC2\xAD나", 3, "가나"},
                        {"𠀀가", 2, "𠀀가"}};
  for (const auto& item : cases) {
    SCOPED_TRACE(item.source);
    lines.clear();
    ParsedText text(true, false, false, zeroIndent(), true);
    text.addWord(item.source, R, false, false, 65530);
    EXPECT_EQ(text.size(), item.tokens);
    layout(text, 480);
    EXPECT_EQ(contents(lines), item.visible);
    if (!lines.empty()) EXPECT_EQ(lines.front().offset, 65530u);
  }
}

TEST_F(KoreanLayoutTest, CjkAttachmentsKeepActualSpacesStylesAndLinks) {
  for (const std::string next : {"나", "(나)", "“나”", "漢"}) {
    for (const bool attached : {false, true}) {
      SCOPED_TRACE(next + (attached ? " attached" : " spaced"));
      lines.clear();
      ParsedText text(true, false, false, zeroIndent(), true);
      const auto link = text.addLinkTarget("chapter.xhtml#note");
      text.addWord("가", B, false, false, 65530);
      text.addWord(next, R, false, attached, attached ? 65531 : 65532, link);
      layout(text, 480);
      ASSERT_EQ(lines.size(), 1u);
      auto& block = *lines.front().block;
      ASSERT_EQ(block.wordCount(), 2);
      EXPECT_STREQ(block.wordText(0), "가");
      EXPECT_STREQ(block.wordText(1), next.c_str());
      EXPECT_EQ(block.wordStyle(0), B);
      EXPECT_EQ(block.wordStyle(1), R);
      EXPECT_EQ(block.wordXpos(1),
                renderer.getTextAdvanceX(1, "가", B) + (attached ? 0 : renderer.getSpaceWidth(1, B)));
      EXPECT_EQ(lines.front().offset, 65530u);
      const auto spans = block.takeLinkSpans();
      ASSERT_EQ(spans.size(), 1u);
      EXPECT_STREQ(spans[0].href, "chapter.xhtml#note");
      EXPECT_EQ(spans[0].x, block.wordXpos(1));
    }
  }
}

TEST_F(KoreanLayoutTest, ParserCjkChunksPreserveThreeAndFourByteCharacters) {
  struct Case {
    const char* unit;
    int count;
  };
  const Case cases[] = {{"가", 65}, {"가", 66},  {"가", 67},  {"𠀀", 49}, {"𠀀", 50},
                        {"𠀀", 51}, {"a가", 49}, {"a가", 50}, {"a가", 51}};
  for (const auto& item : cases) {
    lines.clear();
    pages.clear();
    std::string source;
    for (int i = 0; i < item.count; ++i) source += item.unit;
    SCOPED_TRACE(source.size());
    ASSERT_TRUE(parse("<p>" + source + "<a href='#note'><b>끝</b></a></p>", 79, 70));
    EXPECT_EQ(contents(lines), source + "끝");
    ASSERT_FALSE(pages.empty());
    EXPECT_TRUE(std::any_of(pages.begin(), pages.end(), [](const auto& page) { return !page->links.empty(); }));
    for (size_t i = 1; i < lines.size(); ++i) EXPECT_GE(lines[i].offset, lines[i - 1].offset);
  }
}
