#include <GfxRenderer.h>
#include <HalStorage.h>
#include <SdCardFont.h>
#include <Utf8.h>
#include <gtest/gtest.h>

#include <array>

#include "BinaryFixture.h"

namespace {
struct FontFixture {
  std::vector<uint8_t> bytes;
  std::array<size_t, 2> ligatureOffsets{};
};

FontFixture makeFont(const uint8_t styleCount = 1) {
  using binary_fixture::append;
  using binary_fixture::put;
  FontFixture result;
  auto& bytes = result.bytes;
  bytes.resize(32 + 32 * styleCount);
  std::memcpy(bytes.data(), "CPFONT\0\0", 8);
  put(bytes, 8, CPFONT_VERSION, 2);
  bytes[12] = styleCount;
  for (uint8_t style = 0; style < styleCount; ++style) {
    const size_t toc = 32 + 32 * style;
    bytes[toc] = style;
    put(bytes, toc + 4, 4, 4);  // f, i, fi ligature, replacement intervals
    put(bytes, toc + 8, 4, 4);
    bytes[toc + 12] = 12;
    put(bytes, toc + 13, 10, 2);
    put(bytes, toc + 15, static_cast<uint16_t>(-2), 2);
    put(bytes, toc + 17, 1, 2);
    put(bytes, toc + 19, 1, 2);
    bytes[toc + 21] = bytes[toc + 22] = bytes[toc + 23] = 1;
    put(bytes, toc + 24, bytes.size(), 4);
    append(bytes, EpdUnicodeInterval{'f', 'f', 0});
    append(bytes, EpdUnicodeInterval{'i', 'i', 1});
    append(bytes, EpdUnicodeInterval{0xFB01, 0xFB01, 2});
    append(bytes, EpdUnicodeInterval{0xFFFD, 0xFFFD, 3});
    for (uint32_t glyph = 0; glyph < 4; ++glyph) {
      EpdGlyph data{};
      data.width = data.height = 1;
      data.advanceX = 16;
      data.top = 1;
      data.dataLength = 1;
      data.dataOffset = glyph;
      append(bytes, data);
    }
    append(bytes, EpdKernClassEntry{'f', 1});
    append(bytes, EpdKernClassEntry{'i', 1});
    bytes.push_back(static_cast<uint8_t>(-4));
    result.ligatureOffsets[style] = bytes.size();
    append(bytes, EpdLigaturePair{('f' << 16) | 'i', 0xFB01});
    bytes.insert(bytes.end(), 4, 0x80);
  }
  return result;
}

class SdCardFontLifetimeTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(storage_test::openHandles, 0u);
    storage_test::reset();
    ESP = TestEsp{};
  }
  void TearDown() override { EXPECT_EQ(storage_test::openHandles, 0u); }
};
}  // namespace

TEST_F(SdCardFontLifetimeTest, ReleaseClearsBothPublishedViewsAndPreservesCoverage) {
  storage_test::files["font.cpfont"] = makeFont().bytes;
  SdCardFont font;
  ASSERT_TRUE(font.load("font.cpfont"));
  EpdFont* epd = font.getEpdFont();
  const auto* stub = epd->data;
  ASSERT_EQ(font.prewarm("fiﬁ", 1), 0);
  const auto* mini = epd->data;
  ASSERT_NE(mini, stub);
  ASSERT_NE(stub->ligaturePairs, nullptr);
  ASSERT_EQ(epd->getLigature('f', 'i'), 0xFB01u);
  ASSERT_EQ(epd->getKerning('f', 'i'), -4);
  ASSERT_EQ(font.buildAdvanceTable("fiﬁ", 1), 0);
  font.releaseResidentCaches();
  EXPECT_EQ(epd->data, stub);
  EXPECT_EQ(stub->ligaturePairs, nullptr);
  EXPECT_EQ(stub->ligaturePairCount, 0u);
  EXPECT_EQ(mini->ligaturePairs, nullptr);
  EXPECT_EQ(mini->ligaturePairCount, 0u);
  EXPECT_FALSE(font.hasAdvanceTable());
  EXPECT_TRUE(epd->hasCodepoint('f'));
  EXPECT_TRUE(epd->hasCodepoint(0xFB01));
  EXPECT_FALSE(epd->hasCodepoint('z'));
  // Do not dereference a dangling table in the pre-fix red test.
  if (stub->ligaturePairs == nullptr) EXPECT_EQ(epd->getLigature('f', 'i'), 0u);
  ASSERT_NE(epd->getGlyph('f'), nullptr);  // on-demand miss callback still works
}

TEST_F(SdCardFontLifetimeTest, AdvanceTableStillMeasuresLoadedLigaturesAndKerning) {
  storage_test::files["font.cpfont"] = makeFont().bytes;
  SdCardFont font;
  ASSERT_TRUE(font.load("font.cpfont"));
  ASSERT_EQ(font.prewarm("fiﬁ", 1), 0);
  ASSERT_EQ(font.buildAdvanceTable("fiﬁ", 1), 0);
  HalDisplay panel;
  GfxRenderer renderer(panel);
  renderer.insertFont(100, EpdFontFamily(font.getEpdFont()));
  renderer.registerSdCardFont(100, &font);
  const size_t reads = storage_test::reads;
  EXPECT_EQ(renderer.getTextAdvanceX(100, "fi", EpdFontFamily::REGULAR), 1);

  // Exercise the same loaded data without ligatures, with a one-pixel kern.
  EpdFontData data = *font.getEpdFont()->data;
  const int8_t kern = -16;
  data.ligaturePairs = nullptr;
  data.ligaturePairCount = 0;
  data.kernMatrix = &kern;
  EpdFont kernFont(&data);
  renderer.insertFont(100, EpdFontFamily(&kernFont));
  EXPECT_EQ(renderer.getTextAdvanceX(100, "fi", EpdFontFamily::REGULAR), 1);
  EXPECT_EQ(storage_test::reads, reads);
  renderer.removeFont(100);
}

TEST_F(SdCardFontLifetimeTest, EveryStyleCanBeReleasedAndRewarmedRepeatedly) {
  storage_test::files["font.cpfont"] = makeFont(2).bytes;
  SdCardFont font;
  ASSERT_TRUE(font.load("font.cpfont"));
  const auto hash = font.contentHash();
  for (int pass = 0; pass < 16; ++pass) {
    ASSERT_EQ(font.prewarm("fiﬁ", 3), 0);
    for (uint8_t style = 0; style < 2; ++style) {
      EXPECT_EQ(font.getEpdFont(style)->getLigature('f', 'i'), 0xFB01u);
      EXPECT_EQ(font.getEpdFont(style)->getKerning('f', 'i'), -4);
    }
    font.releaseResidentCaches();
    for (uint8_t style = 0; style < 2; ++style) {
      const auto* data = font.getEpdFont(style)->data;
      ASSERT_EQ(data->ligaturePairs, nullptr);
      EXPECT_EQ(data->ligaturePairCount, 0u);
      EXPECT_NE(data->glyphMissHandler, nullptr);
    }
    EXPECT_EQ(font.contentHash(), hash);
    EXPECT_EQ(font.styleCount(), 2);
  }
}

TEST_F(SdCardFontLifetimeTest, FailedLigatureReadDoesNotRepublishADeadTable) {
  auto fixture = makeFont();
  storage_test::files["font.cpfont"] = fixture.bytes;
  SdCardFont font;
  ASSERT_TRUE(font.load("font.cpfont"));
  ASSERT_EQ(font.prewarm("fiﬁ", 1), 0);
  font.releaseResidentCaches();
  storage_test::shortReadAt = fixture.ligatureOffsets[0];
  font.prewarm("fiﬁ", 1);
  const auto* data = font.getEpdFont()->data;
  EXPECT_EQ(data->ligaturePairs, nullptr);
  EXPECT_EQ(data->ligaturePairCount, 0u);
  storage_test::shortReadAt = std::numeric_limits<size_t>::max();
  font.releaseResidentCaches();
  ASSERT_EQ(font.prewarm("fiﬁ", 1), 0);
  EXPECT_EQ(font.getEpdFont()->getLigature('f', 'i'), 0xFB01u);
}

TEST_F(SdCardFontLifetimeTest, ReleasingAnEmptyOrAlreadyReleasedFontIsSafe) {
  SdCardFont font;
  font.releaseResidentCaches();
  storage_test::files["font.cpfont"] = makeFont().bytes;
  ASSERT_TRUE(font.load("font.cpfont"));
  font.releaseResidentCaches();
  font.releaseResidentCaches();
  ASSERT_EQ(font.prewarm("fiﬁ", 1), 0);
  EXPECT_EQ(font.getEpdFont()->getLigature('f', 'i'), 0xFB01u);
}

namespace {
// More than the bounded advance table, with visible bitmap payloads so a
// measurement accidentally loading pixels is observable through the real HAL.
std::vector<uint8_t> makeWideCoverageFont() {
  using binary_fixture::append;
  using binary_fixture::put;
  constexpr uint32_t count = 1026;  // space, 1024 Hangul syllables, replacement
  std::vector<uint8_t> bytes(64);
  bytes.reserve(64 + 36 + count * 48);
  std::memcpy(bytes.data(), "CPFONT\0\0", 8);
  put(bytes, 8, CPFONT_VERSION, 2);
  bytes[12] = 1;
  put(bytes, 36, 3, 4);
  put(bytes, 40, count, 4);
  bytes[44] = 20;
  put(bytes, 45, 16, 2);
  put(bytes, 47, static_cast<uint16_t>(-4), 2);
  put(bytes, 56, 64, 4);
  append(bytes, EpdUnicodeInterval{' ', ' ', 0});
  append(bytes, EpdUnicodeInterval{0xAC00, 0xAFFF, 1});
  append(bytes, EpdUnicodeInterval{0xFFFD, 0xFFFD, count - 1});
  for (uint32_t i = 0; i < count; ++i) {
    EpdGlyph glyph{};
    glyph.width = glyph.height = glyph.top = 16;
    glyph.advanceX = (i == 0 ? 8 : i == count - 1 ? 12 : 16) * 16;
    glyph.dataLength = 32;
    glyph.dataOffset = i * 32;
    append(bytes, glyph);
  }
  bytes.insert(bytes.end(), count * 32, 0xFF);
  return bytes;
}

std::string hangulRange(uint32_t first, uint32_t count) {
  std::string text;
  text.reserve(count * 3);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t cp = 0xAC00 + first + i;
    text += static_cast<char>(0xE0 | (cp >> 12));
    text += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    text += static_cast<char>(0x80 | (cp & 0x3F));
  }
  return text;
}
}  // namespace

TEST_F(SdCardFontLifetimeTest, SaturatedAdvanceTablePrioritizesTheNextParagraph) {
  storage_test::files["font.cpfont"] = makeWideCoverageFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("font.cpfont"));
  ASSERT_EQ(font.buildAdvanceTable(hangulRange(0, 768).c_str(), 1), 0);
  ASSERT_EQ(font.getAdvance(0xAC00, 0), 256);
  for (int pass = 0; pass < 6; ++pass) {
    const uint32_t first = pass % 2 == 0 ? 768 : 0;
    const auto text = hangulRange(first, 256);
    ASSERT_EQ(font.buildAdvanceTable(text.c_str(), 1), 0);
    for (uint32_t i = 0; i < 256; ++i) {
      ASSERT_EQ(font.getAdvance(0xAC00 + first + i, 0), 256);
    }
    const size_t reads = storage_test::reads;
    ASSERT_EQ(font.buildAdvanceTable(text.c_str(), 1), 0);
    EXPECT_EQ(storage_test::reads, reads);  // repeated paragraph stays warm
  }
}

TEST_F(SdCardFontLifetimeTest, LayoutBeyondAdvanceCapacityNeverReadsBitmaps) {
  storage_test::files["font.cpfont"] = makeWideCoverageFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("font.cpfont"));
  const auto text = hangulRange(0, 900);
  ASSERT_EQ(font.buildAdvanceTable(text.c_str(), 1), 0);
  HalDisplay panel;
  GfxRenderer renderer(panel);
  renderer.insertFont(100, EpdFontFamily(font.getEpdFont()));
  renderer.registerSdCardFont(100, &font);
  storage_test::reads = storage_test::largestRead = 0;
  EXPECT_EQ(renderer.getTextAdvanceX(100, text.c_str(), EpdFontFamily::REGULAR), 900 * 16);
  EXPECT_EQ(storage_test::reads, 900u - 768u);  // one metadata read per uncached glyph
  EXPECT_LE(storage_test::largestRead, sizeof(EpdGlyph));
  renderer.removeFont(100);
}

TEST_F(SdCardFontLifetimeTest, ColdMeasurementAndStyleFallbackNeverLoadPixels) {
  storage_test::files["font.cpfont"] = makeWideCoverageFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("font.cpfont"));
  HalDisplay panel;
  GfxRenderer renderer(panel);
  renderer.insertFont(100, EpdFontFamily(font.getEpdFont()));
  renderer.registerSdCardFont(100, &font);
  storage_test::reads = storage_test::largestRead = 0;
  EXPECT_EQ(renderer.getTextAdvanceX(100, "가", EpdFontFamily::BOLD), 17);
  EXPECT_EQ(renderer.getTextAdvanceX(100, "A", EpdFontFamily::REGULAR), 12);  // replacement advance
  EXPECT_EQ(renderer.getSpaceWidth(100), 8);
  EXPECT_EQ(renderer.getSpaceAdvance(100, 0xAC00, 0xAC01, EpdFontFamily::REGULAR), 8);
  EXPECT_EQ(storage_test::reads, 4u);
  EXPECT_LE(storage_test::largestRead, sizeof(EpdGlyph));
  renderer.removeFont(100);
}

TEST_F(SdCardFontLifetimeTest, LoadedShapingStillUsesMetadataOnlyOnAdvanceMiss) {
  storage_test::files["font.cpfont"] = makeFont().bytes;
  SdCardFont font;
  ASSERT_TRUE(font.load("font.cpfont"));
  ASSERT_EQ(font.prewarm("f", 1), 0);
  ASSERT_NE(font.getEpdFont()->data->ligaturePairs, nullptr);
  ASSERT_EQ(font.buildAdvanceTable("f", 1), 0);
  HalDisplay panel;
  GfxRenderer renderer(panel);
  renderer.insertFont(100, EpdFontFamily(font.getEpdFont()));
  renderer.registerSdCardFont(100, &font);
  storage_test::reads = 0;
  EXPECT_EQ(renderer.getTextAdvanceX(100, "i", EpdFontFamily::REGULAR), 1);
  EXPECT_EQ(storage_test::reads, 1u);  // i is absent from both mini and advance caches
  renderer.removeFont(100);
}

TEST_F(SdCardFontLifetimeTest, CachedZeroAdvanceDoesNotBecomeAnIoMiss) {
  auto bytes = makeFont().bytes;
  // First glyph is f: header/TOC (64) + four intervals (48), then advanceX at +2.
  binary_fixture::put(bytes, 64 + 48 + 2, 0, 2);
  storage_test::files["font.cpfont"] = std::move(bytes);
  SdCardFont font;
  ASSERT_TRUE(font.load("font.cpfont"));
  ASSERT_EQ(font.buildAdvanceTable("f", 1), 0);
  HalDisplay panel;
  GfxRenderer renderer(panel);
  renderer.insertFont(100, EpdFontFamily(font.getEpdFont()));
  renderer.registerSdCardFont(100, &font);
  storage_test::reads = 0;
  EXPECT_EQ(renderer.getTextAdvanceX(100, "f", EpdFontFamily::REGULAR), 0);
  EXPECT_EQ(storage_test::reads, 0u);
  renderer.removeFont(100);
}
