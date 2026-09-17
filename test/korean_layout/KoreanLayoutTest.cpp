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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>
#include <string>
#include <vector>

#include "activities/reader/EpubPageTurns.h"
#include "activities/reader/EpubReaderUtils.h"
#include "test/xtc_memory/SdFontFixture.h"

namespace {
bool failNextArray = false;
size_t lastArrayBytes = 0;
int failScalarAfter = -1;
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
  void TearDown() override {
    failNextArray = false;
    failScalarAfter = -1;
  }
  void layout(ParsedText& text, uint16_t width, bool last = true, int fontId = 1) {
    text.layoutAndExtractLines(
        renderer, fontId, width,
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

// Poison fallible arrays so resumed work cannot rely on allocator contents.
// The next-allocation failure also checks cleanup at parser/layout boundaries.
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  lastArrayBytes = size;
  if (failNextArray) {
    failNextArray = false;
    return nullptr;
  }
  void* memory = ::operator new[](size);
  std::memset(memory, 0xa5, size);
  return memory;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  if (failScalarAfter == 0) {
    failScalarAfter = -1;
    return nullptr;
  }
  if (failScalarAfter > 0) --failScalarAfter;
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}

TEST_F(KoreanLayoutTest, RuleLayoutFailureCannotPublishAnIncompleteSection) {
  const std::string html = "<html><body><aside>앞본문<hr/>뒷본문</aside></body></html>";
  storage_test::files["cache/html/0.html"] = std::vector<uint8_t>(html.begin(), html.end());
  const auto epub = std::make_shared<Epub>();
  ReaderRenderSpec spec;
  spec.fontId = 1;
  spec.viewportWidth = 227;
  spec.viewportHeight = 160;
  spec.extraParagraphSpacing = true;
  spec.characterWrap = false;
  // Cover a failed first build and a failed rebuild over the successful control.
  for (int mode = 0; mode < 3; ++mode) {
    {
      Section section(epub, 0, renderer);
      ASSERT_TRUE(section.startBuild(spec));
      // First Page succeeds; the following paragraph LayoutState fails.
      failScalarAfter = mode == 1 ? -1 : 1;
      const bool built = section.buildSomeMore(0);
      EXPECT_EQ(failScalarAfter, -1);
      failScalarAfter = -1;
      EXPECT_EQ(built, mode == 1);
      EXPECT_EQ(section.isBuildComplete(), mode == 1);
      EXPECT_FALSE(section.isBuilding());
    }
    EXPECT_EQ(storage_test::openHandles, 0u);
    EXPECT_FALSE(Storage.exists("cache/sections/0.bin.part"));
    if (mode == 0) {
      EXPECT_FALSE(Storage.exists("cache/sections/0.bin"));
    } else if (mode == 1) {
      Section reopened(epub, 0, renderer);
      ASSERT_TRUE(reopened.loadSectionFile(spec));
      for (int i = 0; i < reopened.pageCount; ++i) {
        const auto page = reopened.loadPage(i);
        ASSERT_NE(page, nullptr);
        for (const auto& element : page->elements) {
          if (element->getTag() == TAG_PageLine)
            lines.push_back({static_cast<const PageLine&>(*element).getBlock(), 0});
        }
      }
      EXPECT_EQ(contents(lines), "앞본문뒷본문");
    } else {
      EXPECT_FALSE(Storage.exists("cache/sections/0.bin"));
    }
  }
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

TEST_F(KoreanLayoutTest, AdvanceScratchBoundsIncludeExtraTextAndReservedCharacters) {
  storage_test::files["font.cpfont"] = sd_font_fixture::makeWideCoverageFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("font.cpfont"));
  const std::deque<std::string> words{sd_font_fixture::hangulRange(0, 100), "😀"};
  auto work = font.beginAdvanceTable(words, true, 1, "fi");
  lastArrayBytes = 0;
  constexpr int pending = SdCardFont::AdvancePreparation::PENDING;
  EXPECT_EQ(work.prepareSome(4), pending);
  EXPECT_EQ(lastArrayBytes, 1232u);  // 306 UTF-8 bytes plus space/hyphen, uint32_t each.
  int result = pending;
  for (int i = 0; i < 10000 && result == pending; ++i) result = work.prepareSome(1);
  ASSERT_EQ(result, 0);
  EXPECT_EQ(font.getAdvance(0x1f600, 0), 192);
  EXPECT_EQ(font.getAdvance('f', 0), 192);
  EXPECT_EQ(font.getAdvance('-', 0), 192);
  EXPECT_EQ(font.getAdvance(' ', 0), 128);
  EXPECT_EQ(font.buildAdvanceTable(nullptr, 1, "가"), 0);

  const std::deque<std::string> capped{sd_font_fixture::hangulRange(0, 4097)};
  work = font.beginAdvanceTable(capped, true, 1, "extra");
  lastArrayBytes = 0;
  EXPECT_EQ(work.prepareSome(2), pending);
  EXPECT_EQ(lastArrayBytes, 16388u);  // Existing 4,096 cap plus the requested hyphen slot.
  work = {};
  EXPECT_EQ(storage_test::openHandles, 0u);
}

TEST_F(KoreanLayoutTest, ResumedLayoutPreservesBreaksMetadataAndSuppressedTail) {
  storage_test::files["font.cpfont"] = sd_font_fixture::makeWideCoverageFont();
  SdCardFont sd;
  ASSERT_TRUE(sd.load("font.cpfont"));
  renderer.insertFont(100, EpdFontFamily(sd.getEpdFont()));
  renderer.registerSdCardFont(100, &sd);
  // DP/ruby, character wrap, hyphenation, and SD advances cross different resume boundaries.
  for (int mode = 0; mode < 4; ++mode) {
    SCOPED_TRACE(mode);
    const int fontId = mode == 3 ? 100 : 1;
    const auto populate = [mode](ParsedText& text) {
      const uint8_t link = text.addLinkTarget("chapter.xhtml#note");
      for (uint32_t i = 0; i < 12; ++i) {
        const size_t start = text.size();
        const std::string word = mode == 3   ? sd_font_fixture::hangulRange(i * 4, 4)
                                 : mode == 2 ? "internationalization"
                                             : "한글본문";
        text.addWord(word, i % 2 ? R : B, false, false, 65530 + i * 100, i % 3 == 0 ? link : 0);
        if (mode == 0 && i % 3 == 0) text.setRubyGroupAt(start, text.size() - start, "한국어");
      }
    };
    lines.clear();
    ParsedText batch(true, mode == 2, mode != 3, zeroIndent(), mode == 1);
    populate(batch);
    const size_t beforeBatch = storage_test::reads;
    layout(batch, 130, false, fontId);
    const size_t retained = batch.size();
    layout(batch, 130, true, fontId);
    const size_t batchReads = storage_test::reads - beforeBatch;
    if (mode == 3) {
      EXPECT_GT(batchReads, 0u);
      sd.clearPersistentCache();
    }
    auto expected = std::move(lines);
    lines.clear();
    ASSERT_GT(expected.size(), 1u);

    ParsedText sliced(true, mode == 2, mode != 3, zeroIndent(), mode == 1);
    populate(sliced);
    const size_t beforeResume = storage_test::reads;
    const auto emit = [this](std::shared_ptr<TextBlock> block, uint32_t offset) {
      lines.push_back({std::move(block), offset});
    };
    for (const bool last : {false, true}) {
      ASSERT_TRUE(sliced.beginLayout(fontId, 130, last));
      const size_t beforeZero = lines.size();
      EXPECT_FALSE(sliced.layoutSome(renderer, emit, 0));
      EXPECT_EQ(lines.size(), beforeZero);
      bool complete = false;
      for (unsigned step = 0; !complete && step < 100000; ++step) {
        const size_t before = lines.size();
        const size_t beforeRead = storage_test::reads;
        complete = sliced.layoutSome(renderer, emit, 1);
        EXPECT_LE(lines.size() - before, 1u);
        EXPECT_LE(storage_test::reads - beforeRead, 1u);
      }
      ASSERT_TRUE(complete);
      EXPECT_EQ(sliced.size(), last ? 0u : retained);
    }
    ASSERT_EQ(lines.size(), expected.size());
    EXPECT_EQ(storage_test::reads - beforeResume, batchReads);
    const auto serialize = [](const std::vector<Line>& source, const char* path) {
      HalFile file;
      EXPECT_TRUE(Storage.openFileForWrite("test", path, file));
      for (const auto& line : source) {
        EXPECT_EQ(file.write(&line.offset, sizeof(line.offset)), sizeof(line.offset));
        EXPECT_TRUE(line.block->serialize(file));
      }
    };
    serialize(expected, "batch.bin");
    serialize(lines, "sliced.bin");
    EXPECT_EQ(storage_test::files["batch.bin"], storage_test::files["sliced.bin"]);
    for (size_t i = 0; i < lines.size(); ++i) {
      const auto oldLinks = expected[i].block->takeLinkSpans();
      const auto newLinks = lines[i].block->takeLinkSpans();
      ASSERT_EQ(oldLinks.size(), newLinks.size());
      for (size_t link = 0; link < oldLinks.size(); ++link) {
        EXPECT_STREQ(oldLinks[link].href, newLinks[link].href);
        EXPECT_EQ(oldLinks[link].x, newLinks[link].x);
        EXPECT_EQ(oldLinks[link].width, newLinks[link].width);
        EXPECT_EQ(oldLinks[link].topLift, newLinks[link].topLift);
      }
    }
  }
  renderer.removeFont(100);
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
  constexpr int paragraphCount = 2000;
  std::string html = "<html><body><!--" + std::string(32768, 'x') + "-->";
  for (int i = 0; i < paragraphCount; ++i) html += "<p>가나다라마바사 함께 읽는 한글 문단입니다.</p>";
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
  using Action = EpubReaderUtils::IdleAction;
  EpubReaderUtils::IdleFacts idle{
      .section = &section, .canStartBuild = true, .hasRendered = true, .freeHeap = 32 * 1024, .maxBlock = 16 * 1024};
  EXPECT_EQ(EpubReaderUtils::nextIdleWork(idle).delayMs, 0u);
  --idle.freeHeap;
  EXPECT_EQ(EpubReaderUtils::nextIdleWork(idle).delayMs, 250u);
  ++idle.freeHeap;
  --idle.maxBlock;
  EXPECT_EQ(EpubReaderUtils::nextIdleWork(idle).delayMs, 250u);
  ++idle.maxBlock;
  idle.inputActive = true;
  EXPECT_EQ(EpubReaderUtils::nextIdleWork(idle).action, Action::None);
  idle.inputActive = false;
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
  bool sawRetained = false;
  bool sawWindowWait = false;
  // Steps now include paragraph work and XML resumes, not only 1 KiB input chunks.
  for (int tick = 0; tick < paragraphCount * 8 && section.isBuilding(); ++tick) {
    idle.freeHeap = 32 * 1024;
    if (section.hasRetainedBuildOperation()) {
      sawRetained = true;
      idle.freeHeap = 1;
      EXPECT_EQ(EpubReaderUtils::nextIdleWork(idle).action, Action::Section);
      EXPECT_EQ(EpubReaderUtils::nextIdleWork(idle).delayMs, 0u);
    } else if (section.pageCount >= 5) {
      sawWindowWait = true;
      idle.idleMs = 999;
      EXPECT_EQ(EpubReaderUtils::nextIdleWork(idle).delayMs, 1u);
      idle.idleMs = 1000;
      EXPECT_EQ(EpubReaderUtils::nextIdleWork(idle).delayMs, 0u);
    }
    ASSERT_TRUE(section.buildSomeMore(2, 1));
  }
  EXPECT_TRUE(sawRetained);
  EXPECT_TRUE(sawWindowWait);
  EXPECT_TRUE(section.isBuildComplete());
  EXPECT_FALSE(section.isBuilding());
  EXPECT_EQ(section.currentPage, 0);  // finishes beyond the five-page window while stationary
  ASSERT_GT(section.pageCount, 5);
  idle.freeHeap = 32 * 1024;
  idle.maxBlock = 16 * 1024 + 1;
  EXPECT_EQ(EpubReaderUtils::nextIdleWork(idle).action, Action::None);
  idle.scanNextPage = true;
  idle.idleMs = 399;
  EXPECT_EQ(EpubReaderUtils::nextIdleWork(idle).action, Action::ScanNextPage);
  EXPECT_EQ(EpubReaderUtils::nextIdleWork(idle).delayMs, 1u);
  idle.idleMs = 400;
  EXPECT_EQ(EpubReaderUtils::nextIdleWork(idle).delayMs, 0u);
  idle.freeHeap = 24 * 1024;
  EXPECT_EQ(EpubReaderUtils::nextIdleWork(idle).delayMs, 250u);
  idle.fontPending = true;
  EXPECT_EQ(EpubReaderUtils::nextIdleWork(idle).action, Action::Font);
  EXPECT_EQ(EpubReaderUtils::nextIdleWork(idle).delayMs, 0u);
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

TEST_F(KoreanLayoutTest, XmlPauseKeepsConvertedTextAndEmptyElementOrder) {
  std::u16string html = u"<?xml version='1.0' encoding='UTF-16'?><html><body><p>";
  std::string expectedText;
  for (int i = 0; i < 300; ++i) {
    html += u"<span>가 </span>";
    expectedText += "가";
  }
  // Start the converted run at a 1 KiB read boundary. A full UTF-16 input chunk
  // then exceeds Expat's UTF-8 conversion buffer after the 320-word soft limit.
  html += u"<!--";
  html.append((512 - (html.size() + 4) % 512) % 512, u'x');  // closing --> plus BOM
  html += u"-->";
  for (int i = 0; i < 500; ++i) {
    html += u"한글말 ";
    expectedText += "한글말";
  }
  html += u"<br/><a href='#tail'>[1]</a><ruby>漢<rt>かん</rt></ruby></p><li>목록</li><p id='tail'>끝</p>";
  expectedText += "[1]漢•목록끝";
  html +=
      u"<aside style='margin: 4px 8px; font-weight: bold'>가로선앞<hr id='rule'/>이미지앞 "
      u"<img src='unused.png' alt='";
  expectedText += "가로선앞이미지앞[Image:";
  for (int i = 0; i < 400; ++i) {
    html += u"그림 ";
    expectedText += "그림";
  }
  html += u"'/><span>뒤</span></aside></body></html>";
  expectedText += "]뒤";
  const std::string path = "converted.xhtml";
  auto& bytes = storage_test::files[path];
  bytes.reserve(2 + html.size() * 2);
  bytes = {0xff, 0xfe};
  for (const char16_t ch : html) {
    bytes.push_back(static_cast<uint8_t>(ch));
    bytes.push_back(static_cast<uint8_t>(ch >> 8));
  }
  std::vector<std::vector<uint8_t>> expectedPages;
  std::vector<uint32_t> expectedMetadata;
  std::vector<std::pair<std::string, uint16_t>> expectedAnchors;
  size_t expectedReads = 0;
  for (int sliced = 0; sliced < 2; ++sliced) {
    lines.clear();
    std::vector<std::vector<uint8_t>> pageBytes;
    std::vector<uint32_t> metadata;
    ChapterHtmlSlimParser parser(
        nullptr, path, renderer, 1, 1.0f, true, 0, 227, 160, false, false,
        [&](std::unique_ptr<Page> page, uint16_t paragraph, uint16_t item, uint32_t offset) {
          for (const auto& element : page->elements) {
            if (element->getTag() == TAG_PageLine) {
              lines.push_back({static_cast<const PageLine&>(*element).getBlock(), offset});
            }
          }
          metadata.insert(metadata.end(), {paragraph, item, offset});
          HalFile file;
          ASSERT_TRUE(Storage.openFileForWrite("TEST", "converted.page", file));
          ASSERT_TRUE(page->serialize(file));
          pageBytes.push_back(storage_test::files.at("converted.page"));
        },
        true, "", "", 1, {}, nullptr, nullptr, false, false);
    const size_t reads = storage_test::reads;
    if (!sliced) {
      ASSERT_TRUE(parser.parseAndBuildPages());
      expectedPages = pageBytes;
      expectedMetadata = metadata;
      expectedAnchors = parser.getAnchors();
      expectedReads = storage_test::reads - reads;
    } else {
      ASSERT_TRUE(parser.beginParse());
      auto status = ChapterHtmlSlimParser::ParseStatus::More;
      for (int tick = 0; tick < 200000 && status == ChapterHtmlSlimParser::ParseStatus::More; ++tick) {
        const size_t before = pageBytes.size();
        status = parser.parseStep(1);
        EXPECT_LE(pageBytes.size() - before, 1u);
      }
      ASSERT_EQ(status, ChapterHtmlSlimParser::ParseStatus::Done);
      ASSERT_TRUE(parser.finishParse());
      EXPECT_EQ(pageBytes, expectedPages);
      EXPECT_EQ(metadata, expectedMetadata);
      EXPECT_EQ(parser.getAnchors(), expectedAnchors);
      EXPECT_EQ(storage_test::reads - reads, expectedReads);
    }
    EXPECT_EQ(contents(lines), expectedText);
  }

  // Deferred Expat end tag and the two retained image attributes own different buffers.
  for (const char* content : {"가<br/>나", "<img src='unused.png'/>", "<img alt='그림'/>"}) {
    const std::string html = std::string("<html><body>") + content + "</body></html>";
    storage_test::files[path] = std::vector<uint8_t>(html.begin(), html.end());
    ChapterHtmlSlimParser failed(
        nullptr, path, renderer, 1, 1.0f, true, 0, 227, 160, false, false,
        [](std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t) { FAIL() << "incomplete paragraph published"; }, true,
        "", "");
    ASSERT_TRUE(failed.beginParse());
    failNextArray = true;
    EXPECT_EQ(failed.parseStep(), ChapterHtmlSlimParser::ParseStatus::Error);
    EXPECT_FALSE(failNextArray);
    failed.abortParse();
    EXPECT_EQ(storage_test::openHandles, 0u);
  }
}

TEST_F(KoreanLayoutTest, TableLayoutResumesSdWorkAndKeepsMarkupOrder) {
  constexpr int sdFontId = 2;
  storage_test::files["grid.cpfont"] = sd_font_fixture::makeWideCoverageFont();
  const std::string path = "grid.xhtml";
  const auto words = [](const int first, const int count) {
    std::string text;
    for (int i = 0; i < count; ++i) text += sd_font_fixture::hangulRange(first + i * 3, 3) + " ";
    return text;
  };
  const std::string first = words(0, 10);
  const std::string second = words(64, 10);
  struct TableCase {
    const char* name;
    std::string content;
    bool builtin = false;
    bool cancel = false;
    bool utf16 = false;
  };
  const TableCase cases[] = {
      {"grid", "<table dir='rtl'><tr id='grid'><td>가 " + first + "</td><td>" + second + "</td></tr></table>", false,
       true},
      {"linked", "<table><tr><td><a href='#grid'>가</a> " + first + "</td><td>" + second +
                     "</td></tr><tr><td>가</td><td><a href='#grid'>각</a> " + second + "</td></tr></table>"},
      {"token prefix", "<table><tr><td>" + first + "</td><td>" + words(64, 16) + "</td></tr></table>", false, true},
      {"spans", "<table><tr><td>" + first + "</td><td colspan='2' rowspan='2'>" + second + "</td></tr><tr/></table>"},
      {"column prefix", "<table><tr><td>가</td><td>각</td><td>간</td><td>갈</td><td>감</td></tr></table>"},
      {"caption and single column", "가 <table><caption>각 </caption><tr><th>" + first + "</th></tr>간 </table>갈"},
      {"nested row",
       "<table dir='rtl'><tr><td>가</td><td>각</td><tr dir='ltr'><td>간</td><td>갈</td></tr></tr></table>"},
      {"prefix then soft flush",
       "<table><tr><td>가</td><td><!--align-->" + sd_font_fixture::hangulRange(64, 500) + "</td></tr></table>", false,
       false, true},
      {"byte prefix",
       "<table><tr><td>가</td><td>" + std::string(180, 'a') + " " + std::string(180, 'b') + " " +
           std::string(180, 'c') + " </td></tr></table>",
       true},
  };
  for (size_t input = 0; input < std::size(cases); ++input) {
    const auto& test = cases[input];
    SCOPED_TRACE(test.name);
    const std::string html = "<html><body>" + test.content + "</body></html>";
    auto& inputBytes = storage_test::files[path];
    if (test.utf16) {
      std::u16string text;
      const auto* cursor = reinterpret_cast<const unsigned char*>(html.c_str());
      while (*cursor) text += static_cast<char16_t>(utf8NextCodepoint(&cursor));
      const size_t align = text.find(u"<!--align-->");
      ASSERT_NE(align, std::u16string::npos);
      text.replace(align, 12, u"<!--" + std::u16string((512 - (align + 8) % 512) % 512, u'x') + u"-->");
      inputBytes = {0xff, 0xfe};
      inputBytes.reserve(2 + text.size() * 2);
      for (const char16_t ch : text) {
        inputBytes.push_back(static_cast<uint8_t>(ch));
        inputBytes.push_back(static_cast<uint8_t>(ch >> 8));
      }
    } else {
      inputBytes.assign(html.begin(), html.end());
    }
    std::vector<std::vector<uint8_t>> expectedPages;
    std::vector<uint32_t> expectedMetadata;
    std::vector<std::pair<std::string, uint16_t>> expectedAnchors;
    size_t expectedReads = 0;
    for (int mode = 0; mode < (test.cancel ? 3 : 2); ++mode) {
      SCOPED_TRACE(mode);
      SdCardFont font;
      ASSERT_TRUE(font.load("grid.cpfont"));
      renderer.insertFont(sdFontId, EpdFontFamily(font.getEpdFont()));
      renderer.registerSdCardFont(sdFontId, &font);
      std::vector<std::vector<uint8_t>> pageBytes;
      std::vector<uint32_t> metadata;
      size_t links = 0;
      ChapterHtmlSlimParser parser(
          nullptr, path, renderer, test.builtin ? 1 : sdFontId, 1.0f, true, 0, 320, 50, false, false,
          [&](std::unique_ptr<Page> page, uint16_t paragraph, uint16_t item, uint32_t offset) {
            if (input == 0 && pageBytes.empty()) {
              ASSERT_GE(page->elements.size(), 2u);
              EXPECT_EQ(page->elements[0]->xPos, 164);  // first logical cell is on the right
              EXPECT_EQ(page->elements[1]->xPos, 4);
            }
            links += page->links.size();
            metadata.insert(metadata.end(), {paragraph, item, offset});
            HalFile file;
            ASSERT_TRUE(Storage.openFileForWrite("TEST", "grid.page", file));
            ASSERT_TRUE(page->serialize(file));
            pageBytes.push_back(storage_test::files.at("grid.page"));
          },
          true, "", "", 0, {}, nullptr, nullptr, false, false);
      const size_t reads = storage_test::reads;
      if (mode == 0) {
        ASSERT_TRUE(parser.parseAndBuildPages());
        expectedPages = pageBytes;
        expectedMetadata = metadata;
        expectedAnchors = parser.getAnchors();
        expectedReads = storage_test::reads - reads;
        ASSERT_FALSE(pageBytes.empty());
        if (input == 0) {
          ASSERT_GT(pageBytes.size(), 1u);
          ASSERT_EQ(expectedAnchors.size(), 1u);
          EXPECT_EQ(expectedAnchors.front().first, "grid");
        }
        if (input == 1) EXPECT_GT(links, 0u);
      } else {
        ASSERT_TRUE(parser.beginParse());
        ASSERT_EQ(parser.parseStep(1), ChapterHtmlSlimParser::ParseStatus::More);
        EXPECT_EQ(storage_test::reads, reads + 1);  // XML read; the row's SD work has not run yet.
        EXPECT_TRUE(pageBytes.empty());
        EXPECT_EQ(parser.parseStep(0), ChapterHtmlSlimParser::ParseStatus::More);
        EXPECT_EQ(storage_test::reads, reads + 1);
        auto status = ChapterHtmlSlimParser::ParseStatus::More;
        for (int tick = 0; tick < 50000 && status == ChapterHtmlSlimParser::ParseStatus::More; ++tick) {
          const size_t before = storage_test::reads;
          status = parser.parseStep(1);
          EXPECT_LE(storage_test::reads - before, 1u);
          if (mode == 2 && storage_test::reads > reads + 1) break;
        }
        if (mode == 2) {
          ASSERT_GT(storage_test::reads, reads + 1);
          parser.abortParse();
          EXPECT_TRUE(pageBytes.empty());
          EXPECT_EQ(storage_test::openHandles, 0u);
        } else {
          ASSERT_EQ(status, ChapterHtmlSlimParser::ParseStatus::Done);
          ASSERT_TRUE(parser.finishParse());
          EXPECT_EQ(pageBytes, expectedPages);
          EXPECT_EQ(metadata, expectedMetadata);
          EXPECT_EQ(parser.getAnchors(), expectedAnchors);
          EXPECT_EQ(storage_test::reads - reads, expectedReads);
        }
      }
      renderer.unregisterSdCardFont(sdFontId);
    }
  }
}

TEST_F(KoreanLayoutTest, TrailingParagraphYieldsWithoutRereadingAndAbortsItsFontWork) {
  constexpr int sdFontId = 2;
  storage_test::files["trailing.cpfont"] = sd_font_fixture::makeWideCoverageFont();
  for (const char* boundary : {"", "<hr/>", "<img alt='그림'/>"}) {
    SCOPED_TRACE(boundary);
    SdCardFont font;
    ASSERT_TRUE(font.load("trailing.cpfont"));
    renderer.insertFont(sdFontId, EpdFontFamily(font.getEpdFont()));
    renderer.registerSdCardFont(sdFontId, &font);
    const std::string path = "trailing.xhtml";
    const std::string text = sd_font_fixture::hangulRange(0, 100);
    const std::string html = "<html><body><aside id='tail'>" + text + " " + boundary + "</aside></body></html>";
    storage_test::files[path] = std::vector<uint8_t>(html.begin(), html.end());
    size_t emitted = 0;
    ChapterHtmlSlimParser parser(
        nullptr, path, renderer, sdFontId, 1.0f, true, 0, 227, 160, false, false,
        [&](std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t) { ++emitted; }, true, "", "", 0, {}, nullptr, nullptr,
        false, false);
    ASSERT_TRUE(parser.beginParse());
    ASSERT_EQ(parser.parseStep(1), ChapterHtmlSlimParser::ParseStatus::More);
    EXPECT_EQ(parser.parseBytesConsumed(), html.size());
    EXPECT_EQ(emitted, 0u);
    const size_t reads = storage_test::reads;
    EXPECT_EQ(parser.parseStep(0), ChapterHtmlSlimParser::ParseStatus::More);
    EXPECT_EQ(storage_test::reads, reads);
    // Stop during SD advance loading at EOF or before inserting the visual element.
    for (int tick = 0; tick < 5000 && storage_test::reads == reads; ++tick) {
      ASSERT_EQ(parser.parseStep(1), ChapterHtmlSlimParser::ParseStatus::More);
    }
    ASSERT_GT(storage_test::reads, reads);
    EXPECT_EQ(emitted, 0u);
    const size_t handles = storage_test::openHandles;
    parser.abortParse();
    EXPECT_LT(storage_test::openHandles, handles);
    EXPECT_EQ(parser.parseStep(), ChapterHtmlSlimParser::ParseStatus::Error);
    EXPECT_EQ(emitted, 0u);
    renderer.unregisterSdCardFont(sdFontId);
  }
}

TEST_F(KoreanLayoutTest, SectionExtractionResumesAndOnlyPublishesCompleteHtml) {
  std::ifstream archive(
      std::filesystem::path(__FILE__).parent_path().parent_path() / "epubs/font-prewarm-benchmark.epub",
      std::ios::binary);
  ASSERT_TRUE(archive);
  const auto epub = std::make_shared<Epub>();
  epub->spineHref = "OEBPS/mixed.xhtml";
  storage_test::files[epub->getPath()] = {std::istreambuf_iterator<char>(archive), std::istreambuf_iterator<char>()};
  ReaderRenderSpec spec;
  spec.fontId = 1;
  spec.viewportWidth = 227;
  spec.viewportHeight = 160;
  {
    Section batch(epub, 0, renderer);
    GfxRenderer::FrameBufferLoan loan(renderer);
    ASSERT_TRUE(batch.createSectionFile(spec));
  }
  const auto expectedHtml = storage_test::files.at("cache/html/0.html");
  const auto expectedPages = storage_test::files.at("cache/sections/0.bin");
  ASSERT_EQ(expectedHtml.size(), 44925u);
  for (int mode = 0; mode < 3; ++mode) {
    SCOPED_TRACE(mode);  // complete, cancel during extraction, repeated short write
    Storage.remove("cache/html/0.html");
    if (mode == 2) storage_test::shortWriteAt = 0;
    int popups = 0;
    Section section(epub, 0, renderer);
    const size_t reads = storage_test::reads;
    ASSERT_TRUE(section.startBuild(spec, [&] { ++popups; }));
    EXPECT_EQ(storage_test::reads, reads);
    EXPECT_EQ(popups, 0);
    EXPECT_FALSE(section.hasRetainedBuildOperation());
    bool success = true;
    bool sawPartial = false;
    for (int step = 0; step < 20000 && section.isBuilding(); ++step) {
      const bool extracting = !section.hasHtmlCache();
      const size_t before = storage_test::reads;
      success = section.buildSomeMore(0, 1);
      if (extracting) EXPECT_LE(storage_test::reads - before, 2u);
      if (extracting && section.hasHtmlCache()) EXPECT_FALSE(section.hasRetainedBuildOperation());
      if (!success) break;
      const auto temp = storage_test::files.find("cache/html/.tmp_0.html");
      if (temp != storage_test::files.end() && !temp->second.empty()) {
        sawPartial = true;
        EXPECT_TRUE(section.hasRetainedBuildOperation());
        EXPECT_FALSE(section.hasHtmlCache());
        EXPECT_LT(temp->second.size(), expectedHtml.size());
        EXPECT_EQ(section.pageCount, 0);
        EXPECT_EQ(popups, 0);
        if (mode == 1) {
          section.suspendBuild();
          break;
        }
      }
    }
    storage_test::shortWriteAt = std::numeric_limits<size_t>::max();
    EXPECT_EQ(success, mode != 2);
    EXPECT_FALSE(section.isBuilding());
    EXPECT_FALSE(section.hasRetainedBuildOperation());
    EXPECT_EQ(section.isBuildComplete(), mode == 0);
    EXPECT_FALSE(Storage.exists("cache/html/.tmp_0.html"));
    EXPECT_EQ(storage_test::openHandles, 0u);
    EXPECT_EQ(storage_test::files.at("cache/sections/0.bin"), expectedPages);
    if (mode == 0) {
      EXPECT_TRUE(sawPartial);
      EXPECT_EQ(popups, 1);
      EXPECT_EQ(storage_test::files.at("cache/html/0.html"), expectedHtml);
    } else {
      EXPECT_EQ(popups, 0);
      EXPECT_FALSE(section.hasHtmlCache());
      if (mode == 1) EXPECT_TRUE(sawPartial);
    }
  }
}

TEST_F(KoreanLayoutTest, SectionCommitResumesAfterPageReadsAndRetiresWithoutRewritingTables) {
  std::string html = "<html><body><aside id='tail'>";
  for (int i = 0; i < 100; ++i) html += "한글 문단과 Englishreading을 함께 읽습니다. ";
  html += "<a href='#tail'>[1]</a> 마지막</aside></body></html>";
  storage_test::files["cache/html/0.html"] = std::vector<uint8_t>(html.begin(), html.end());
  const auto epub = std::make_shared<Epub>();
  ReaderRenderSpec spec;
  spec.fontId = 1;
  spec.viewportWidth = 227;
  spec.viewportHeight = 160;
  spec.extraParagraphSpacing = true;
  spec.characterWrap = false;
  uint16_t pageCount;
  {
    Section batch(epub, 0, renderer);
    ASSERT_TRUE(batch.createSectionFile(spec));
    pageCount = batch.pageCount;
    ASSERT_GT(pageCount, 16);
  }
  const auto expected = storage_test::files.at("cache/sections/0.bin");
  const size_t writes = storage_test::writes;
  const size_t bytes = storage_test::writeBytes;
  // Normal resume, retirement during commit, and a short write before publication.
  for (int mode = 0; mode < 3; ++mode) {
    SCOPED_TRACE(mode);
    if (mode != 2) Storage.remove("cache/sections/0.bin");
    storage_test::writes = storage_test::writeBytes = 0;
    Section resumed(epub, 0, renderer);
    ASSERT_TRUE(resumed.startBuild(spec));
    EXPECT_FALSE(resumed.hasRetainedBuildOperation());
    bool sawRetainedLayout = false;
    for (int tick = 0; tick < 10000 && resumed.pageCount < pageCount; ++tick) {
      ASSERT_TRUE(resumed.buildSomeMore(0, 1));
      if (resumed.pageCount < pageCount && resumed.hasRetainedBuildOperation()) sawRetainedLayout = true;
    }
    EXPECT_TRUE(sawRetainedLayout);
    ASSERT_EQ(resumed.pageCount, pageCount);
    ASSERT_TRUE(resumed.isBuilding());
    ASSERT_FALSE(resumed.isBuildComplete());
    EXPECT_TRUE(resumed.hasRetainedBuildOperation());
    EXPECT_EQ(storage_test::files.at("cache/sections/0.bin.part")[0], 0);
    ASSERT_NE(resumed.loadPage(pageCount - 1), nullptr);
    if (mode == 2) {
      storage_test::shortWriteAt = storage_test::files.at("cache/sections/0.bin.part").size();
      EXPECT_FALSE(resumed.buildSomeMore(0, 1));
      storage_test::shortWriteAt = std::numeric_limits<size_t>::max();
      EXPECT_FALSE(resumed.isBuilding());
      EXPECT_FALSE(resumed.isBuildComplete());
      EXPECT_FALSE(Storage.exists("cache/sections/0.bin.part"));
    } else {
      if (mode == 1) {
        resumed.suspendBuild();
      } else {
        for (int tick = 0; tick < 1000 && resumed.isBuilding(); ++tick) {
          ASSERT_NE(resumed.loadPage(0), nullptr);
          const size_t before = storage_test::writes;
          ASSERT_TRUE(resumed.buildSomeMore(0, 1));
          EXPECT_LE(storage_test::writes - before, 96u);
        }
      }
      EXPECT_TRUE(resumed.isBuildComplete());
      EXPECT_FALSE(resumed.isBuilding());
      EXPECT_EQ(storage_test::writes, writes);
      EXPECT_EQ(storage_test::writeBytes, bytes);
    }
    EXPECT_FALSE(resumed.hasRetainedBuildOperation());
    EXPECT_EQ(storage_test::files.at("cache/sections/0.bin"), expected);
  }
}

TEST_F(KoreanLayoutTest, RetainedTurnsWaitForRealPartialPagesAndAccountForChapterBoundaries) {
  using Action = EpubPageTurns::Action;
  using Step = EpubPageTurns::Step;
  using Outcome = EpubPageTurns::Outcome;
  // Same long paragraph as the existing partial-cache fixture above.
  std::string html = "<html><body><p>";
  for (int i = 0; i < 2000; ++i) html += "가나다라마바사 아자차카타파하 ";
  html += "</p></body></html>";
  storage_test::files["cache/html/0.html"] = std::vector<uint8_t>(html.begin(), html.end());
  const auto epub = std::make_shared<Epub>();
  ReaderRenderSpec spec;
  spec.fontId = 1;
  spec.viewportWidth = 227;
  spec.viewportHeight = 160;
  spec.characterWrap = false;
  spec.extraParagraphSpacing = true;
  EpubPageTurns turns;
  std::optional<uint16_t> jump;
  int spine = 0;
  ASSERT_TRUE(turns.accept(Action::NextPage, 100, 100));
  ASSERT_TRUE(turns.accept(Action::PreviousPage, 180, 180));
  {
    Section building(epub, spine, renderer);
    ASSERT_TRUE(building.startBuild(spec));
    EXPECT_EQ(turns.applyTo(building, spine, 2, jump, 200), Step::NeedsNextPage);
    EXPECT_EQ(building.currentPage, 0);
    EXPECT_EQ(spine, 0);
    ASSERT_TRUE(building.buildSomeMore(EpubReaderUtils::PARTIAL_REBUILD_START_MARGIN + 1));
    ASSERT_FALSE(building.isBuildComplete());
    building.suspendBuild();
  }
  Section section(epub, spine, renderer);
  ASSERT_TRUE(section.loadSectionFile(spec));
  ASSERT_TRUE(section.isPartial());
  const int watermark = section.pageCount;
  EpubReaderUtils::IdleFacts idle{.section = &section,
                                  .canStartBuild = true,
                                  .hasRendered = true,
                                  .idleMs = 1000,
                                  .freeHeap = 32 * 1024,
                                  .maxBlock = 16 * 1024};
  ASSERT_GT(watermark, EpubReaderUtils::PARTIAL_REBUILD_START_MARGIN);
  EXPECT_EQ(EpubReaderUtils::nextIdleWork(idle).action, EpubReaderUtils::IdleAction::None);
  section.currentPage = watermark - EpubReaderUtils::PARTIAL_REBUILD_START_MARGIN;
  EXPECT_EQ(EpubReaderUtils::nextIdleWork(idle).action, EpubReaderUtils::IdleAction::StartSection);
  idle.canStartBuild = false;
  EXPECT_EQ(EpubReaderUtils::nextIdleWork(idle).action, EpubReaderUtils::IdleAction::None);
  section.currentPage = watermark - 1;
  EXPECT_EQ(turns.applyTo(section, spine, 2, jump, 400), Step::NeedsNextPage);
  EXPECT_EQ(turns.applyTo(section, spine, 2, jump, 600), Step::NeedsNextPage);
  EXPECT_EQ(section.currentPage, watermark - 1);
  EXPECT_EQ(spine, 0);
  EXPECT_EQ(turns.getCounts().pending, 2u);
  ASSERT_TRUE(section.startBuild(spec));
  for (int chunk = 0; chunk < 1000 && section.pageCount <= watermark; ++chunk) {
    ASSERT_TRUE(section.buildSomeMore(1, 1));
  }
  ASSERT_GT(section.pageCount, watermark);
  EXPECT_EQ(turns.applyTo(section, spine, 2, jump, 800), Step::Applied);
  EXPECT_EQ(section.currentPage, watermark);
  EXPECT_EQ(turns.applyTo(section, spine, 2, jump, 1000), Step::Applied);
  EXPECT_EQ(section.currentPage, watermark - 1);
  EXPECT_EQ(turns.getCounts().applied, 2u);
  ASSERT_TRUE(section.buildSomeMore(0));
  ASSERT_TRUE(section.isBuildComplete());

  section.currentPage = 0;
  ASSERT_TRUE(turns.accept(Action::PreviousPage, 1100, 1100));
  ASSERT_TRUE(turns.accept(Action::NextPage, 1180, 1180));
  EXPECT_EQ(turns.applyTo(section, spine, 2, jump, 1200), Step::Boundary);
  EXPECT_EQ(section.currentPage, 0);
  EXPECT_EQ(turns.applyTo(section, spine, 2, jump, 1400), Step::Applied);
  EXPECT_EQ(section.currentPage, 1);  // Previous at book start cannot cancel out Next.

  ASSERT_TRUE(turns.accept(Action::NextChapter, 1500, 1500));
  EXPECT_EQ(turns.applyTo(section, spine, 2, jump, 1600), Step::SectionChanged);
  EXPECT_EQ(spine, 1);
  EXPECT_FALSE(jump.has_value());
  section.currentPage = 0;
  ASSERT_TRUE(turns.accept(Action::PreviousPage, 1700, 1700));
  EXPECT_EQ(turns.applyTo(section, spine, 2, jump, 1800), Step::SectionChanged);
  EXPECT_EQ(spine, 0);
  EXPECT_EQ(jump, UINT16_MAX);
  ASSERT_TRUE(turns.accept(Action::NextChapter, 1900, 1900));
  EXPECT_EQ(turns.applyTo(section, spine, 2, jump, 2000), Step::SectionChanged);
  EXPECT_EQ(spine, 1);
  EXPECT_FALSE(jump.has_value());
  section.currentPage = section.pageCount - 1;
  ASSERT_TRUE(turns.accept(Action::NextPage, 2100, 2100));
  EXPECT_EQ(turns.applyTo(section, spine, 2, jump, 2200), Step::SectionChanged);
  EXPECT_EQ(spine, 2);
  ASSERT_TRUE(turns.accept(Action::NextPage, 2300, 2300));
  EXPECT_EQ(turns.applyTo(section, spine, 2, jump, 2400), Step::Boundary);
  ASSERT_TRUE(turns.accept(Action::PreviousPage, 2500, 2500));
  turns.cancelPending(Outcome::ContextChanged, 2600);
  const auto counts = turns.getCounts();
  EXPECT_EQ(counts.accepted, counts.applied + counts.cancelled + counts.failed + counts.pending);
  EXPECT_EQ(counts.accepted, 10u);
  EXPECT_EQ(counts.applied, 7u);
  EXPECT_EQ(counts.cancelled, 3u);
  EXPECT_EQ(counts.pending, 0u);
}
