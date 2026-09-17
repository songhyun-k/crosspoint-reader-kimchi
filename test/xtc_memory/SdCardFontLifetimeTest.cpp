#include <GfxRenderer.h>
#include <HalStorage.h>
#include <SdCardFont.h>
#include <Utf8.h>
#include <gtest/gtest.h>

#include <array>

#include "BinaryFixture.h"
#include "SdFontFixture.h"

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

using sd_font_fixture::hangulRange;
using sd_font_fixture::makeWideCoverageFont;

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

TEST_F(SdCardFontLifetimeTest, ResumedAdvancesPreserveIoAndMergeAnInterleavedFill) {
  storage_test::files["font.cpfont"] = makeWideCoverageFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("font.cpfont"));
  constexpr int pending = SdCardFont::AdvancePreparation::PENDING;
  const std::deque<std::string> words{hangulRange(0, 64), hangulRange(64, 64)};
  const auto batchText = hangulRange(0, 128) + " ";
  const size_t reads = storage_test::reads, bytes = storage_test::readBytes, seeks = storage_test::seeks;
  ASSERT_EQ(font.buildAdvanceTable(batchText.c_str(), 2), 0);
  const size_t batchReads = storage_test::reads - reads;
  const size_t batchBytes = storage_test::readBytes - bytes;
  const size_t batchSeeks = storage_test::seeks - seeks;
  font.clearPersistentCache();
  const size_t startReads = storage_test::reads, startBytes = storage_test::readBytes, startSeeks = storage_test::seeks;
  auto work = font.beginAdvanceTable(words, false, 2);
  EXPECT_EQ(work.prepareSome(0), pending);
  EXPECT_EQ(storage_test::reads, startReads);
  int result = pending;
  for (unsigned step = 0; result == pending && step < 2000; ++step) {
    const size_t before = storage_test::reads;
    result = work.prepareSome(1);
    EXPECT_LE(storage_test::reads - before, 1u);
  }
  ASSERT_EQ(result, 0);
  EXPECT_EQ(storage_test::reads - startReads, batchReads);
  EXPECT_EQ(storage_test::readBytes - startBytes, batchBytes);
  EXPECT_EQ(storage_test::seeks - startSeeks, batchSeeks);
  EXPECT_EQ(storage_test::openHandles, 0u);
  for (uint32_t cp = 0xAC00; cp < 0xAC80; ++cp) EXPECT_EQ(font.getAdvance(cp, 0), 256);

  font.clearPersistentCache();
  const std::deque<std::string> prefix{hangulRange(0, 128)};
  work = font.beginAdvanceTable(prefix, false, 1);
  const size_t beforeRead = storage_test::reads;
  for (unsigned step = 0; storage_test::reads == beforeRead && step < 2000; ++step) {
    ASSERT_EQ(work.prepareSome(1), pending);
  }
  ASSERT_GT(storage_test::reads, beforeRead);
  ASSERT_EQ(font.buildAdvanceTable(prefix.front().c_str(), 1), 0);
  font.clearCache();  // Glyph-cache release must not destroy a caller's advance operation.
  result = pending;
  for (unsigned step = 0; result == pending && step < 2000; ++step) result = work.prepareSome(1);
  ASSERT_EQ(result, 0);
  // Duplicating the overlapping prefix would prematurely fill the768-entry cache.
  ASSERT_EQ(font.buildAdvanceTable(hangulRange(128, 640).c_str(), 1), 0);
  for (uint32_t cp = 0xAC00; cp < 0xAF00; ++cp) EXPECT_EQ(font.getAdvance(cp, 0), 256);
}

TEST_F(SdCardFontLifetimeTest, CancelledAdvanceWorkDoesNotPublishAndShortReadKeepsOnlyCompleteEntries) {
  storage_test::files["font.cpfont"] = makeWideCoverageFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("font.cpfont"));
  const std::deque<std::string> words{hangulRange(0, 3)};
  constexpr int pending = SdCardFont::AdvancePreparation::PENDING;
  auto work = font.beginAdvanceTable(words, false, 1);
  const size_t start = storage_test::reads;
  for (unsigned step = 0; storage_test::reads == start && step < 100; ++step) {
    ASSERT_EQ(work.prepareSome(1), pending);
  }
  ASSERT_GT(storage_test::reads, start);
  EXPECT_EQ(font.getAdvance(0xAC00, 0), 0);
  work = {};
  EXPECT_EQ(storage_test::openHandles, 0u);
  EXPECT_EQ(font.getAdvance(0xAC00, 0), 0);

  storage_test::shortReadAt = 64 + 3 * sizeof(EpdUnicodeInterval) + 2 * sizeof(EpdGlyph);
  storage_test::negativeRead = true;
  work = font.beginAdvanceTable(words, false, 1);
  int result = pending;
  for (unsigned step = 0; result == pending && step < 100; ++step) result = work.prepareSome(1);
  EXPECT_EQ(result, 0);  // Same successful-prefix/on-demand fallback policy as the batch API.
  EXPECT_EQ(storage_test::openHandles, 0u);
  EXPECT_EQ(font.getAdvance(0xAC00, 0), 256);
  EXPECT_EQ(font.getAdvance(0xAC01, 0), 0);
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
