#include <EpdFontFamily.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <SdCardFont.h>
#include <builtinFonts/kimchi_batang_14_regular.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "test/xtc_memory/BinaryFixture.h"

namespace {
using Style = EpdFontFamily::Style;
constexpr Style R = EpdFontFamily::REGULAR;
constexpr Style B = EpdFontFamily::BOLD;
constexpr Style I = EpdFontFamily::ITALIC;
constexpr Style BI = EpdFontFamily::BOLD_ITALIC;
constexpr uint8_t MONO[] = {0xFC};        // six on pixels, 3 x 2
constexpr uint8_t GRAY[] = {0xD8, 0x6C};  // [3,1,2], [0,1,2]; last two padding samples unused
constexpr EpdGlyph GLYPHS[] = {{0, 0, 24, 0, 0, 0, 0},
                               {3, 2, 56, 0, 2, 1, 0},
                               {3, 2, 56, 0, 2, 1, 0},
                               {3, 2, 56, 0, 2, 1, 0},
                               {1, 1, 0, 0, 4, 1, 0}};
constexpr EpdUnicodeInterval INTERVALS[] = {{' ', ' ', 0}, {'A', 'C', 1}, {0x301, 0x301, 4}};
constexpr auto data(bool gray = false) {
  EpdFontData d{};
  d.bitmap = gray ? GRAY : MONO;
  d.glyph = GLYPHS;
  d.intervals = INTERVALS;
  d.intervalCount = std::size(INTERVALS);
  d.advanceY = 6;
  d.ascender = 4;
  d.is2Bit = gray;
  return d;
}

bool pixel(const HalDisplay& display, GfxRenderer::Orientation orientation, int x, int y) {
  int px = x, py = y;
  switch (orientation) {
    case GfxRenderer::Portrait:
      px = y;
      py = display.getDisplayHeight() - 1 - x;
      break;
    case GfxRenderer::PortraitInverted:
      px = display.getDisplayWidth() - 1 - y;
      py = x;
      break;
    case GfxRenderer::LandscapeClockwise:
      px = display.getDisplayWidth() - 1 - x;
      py = display.getDisplayHeight() - 1 - y;
      break;
    case GfxRenderer::LandscapeCounterClockwise:
      break;
  }
  return (display.pixels[py * display.getDisplayWidthBytes() + px / 8] & (0x80 >> (px % 8))) != 0;
}

std::vector<uint8_t> cpfont(const uint8_t mask) {
  using binary_fixture::append;
  using binary_fixture::put;
  uint8_t count = 0;
  for (unsigned i = 0; i < 4; ++i) count += (mask >> i) & 1;
  std::vector<uint8_t> bytes(32 + count * 32, 0);
  std::memcpy(bytes.data(), "CPFONT\0\0", 8);
  put(bytes, 8, CPFONT_VERSION, 2);
  bytes[12] = count;
  uint8_t toc = 0;
  for (uint8_t style = 0; style < 4; ++style) {
    if (!(mask & (1u << style))) continue;
    const size_t offset = 32 + toc++ * 32;
    put(bytes, offset, style, 1);
    put(bytes, offset + 4, std::size(INTERVALS), 4);
    put(bytes, offset + 8, std::size(GLYPHS), 4);
    put(bytes, offset + 12, 6, 1);
    put(bytes, offset + 13, 4, 2);
    put(bytes, offset + 24, bytes.size(), 4);
    for (const auto& interval : INTERVALS) append(bytes, interval);
    for (const auto& glyph : GLYPHS) append(bytes, glyph);
    bytes.push_back(MONO[0]);
  }
  return bytes;
}

class SyntheticBoldTest : public testing::Test {
 protected:
  HalDisplay panel;
  GfxRenderer renderer{panel};
  EpdFontData regularData = data();
  EpdFontData boldData = data();
  EpdFont regular{&regularData}, bold{&boldData};
  void SetUp() override {
    storage_test::reset();
    renderer.begin();
    renderer.insertFont(1, EpdFontFamily(&regular));
    renderer.insertFont(2, EpdFontFamily(&regular, &bold));
    renderer.insertFont(3, EpdFontFamily(&regular, nullptr, &regular));
    renderer.insertFont(4, EpdFontFamily(&regular, &regular));
  }
};
}  // namespace

TEST_F(SyntheticBoldTest, RealBoldAndAliasesAndOverlayBitsUseTheCorrectPolicy) {
  const auto& fonts = renderer.getFontMap();
  for (uint8_t overlay : {0u, 4u, 8u, 16u, 32u, 64u}) {
    EXPECT_TRUE(fonts.at(1).needsSyntheticBold(static_cast<Style>(B | overlay)));
    EXPECT_TRUE(fonts.at(3).needsSyntheticBold(static_cast<Style>(BI | overlay)));
    EXPECT_TRUE(fonts.at(4).needsSyntheticBold(static_cast<Style>(B | overlay)));
    EXPECT_FALSE(fonts.at(2).needsSyntheticBold(static_cast<Style>(B | overlay)));
    EXPECT_FALSE(fonts.at(2).needsSyntheticBold(static_cast<Style>(BI | overlay)));
  }
  EXPECT_FALSE(fonts.at(1).needsSyntheticBold(R));
  EXPECT_FALSE(fonts.at(1).needsSyntheticBold(I));
}

TEST_F(SyntheticBoldTest, AdvancesAndBoundsFollowActualMonochromeRenderingInEveryOrientation) {
  EXPECT_EQ(renderer.getTextAdvanceX(1, "ABC", R), 12);
  EXPECT_EQ(renderer.getTextAdvanceX(1, "ABC", B), 15);
  EXPECT_EQ(renderer.getTextWidth(1, "ABC", R), 11);
  EXPECT_EQ(renderer.getTextWidth(1, "ABC", B), 14);
  EXPECT_EQ(renderer.getTextAdvanceX(1,
                                     "A\xCC\x81"
                                     "B",
                                     B),
            10);
  EXPECT_EQ(renderer.getTextAdvanceX(1, "", B), 0);
  EXPECT_EQ(renderer.getTextAdvanceX(1, nullptr, B), 0);
  for (int o = 0; o < 4; ++o) {
    const auto orientation = static_cast<GfxRenderer::Orientation>(o);
    renderer.setOrientation(orientation);
    renderer.clearScreen();
    renderer.drawText(1, 10, 10, "ABC", true, B);
    for (int x = 9; x < 27; ++x) {
      const bool ink = (x >= 10 && x < 14) || (x >= 15 && x < 19) || (x >= 20 && x < 24);
      EXPECT_EQ(!pixel(panel, orientation, x, 12), ink) << o << ":" << x;
    }
  }
}

TEST_F(SyntheticBoldTest, RotatedTextExpandsAndAdvancesAlongItsOwnBaseline) {
  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  renderer.clearScreen();
  renderer.drawTextRotated90CW(1, 10, 40, "AB", true, B);
  for (int y = 28; y < 43; ++y) {
    const bool ink = (y >= 37 && y <= 40) || (y >= 32 && y <= 35);
    EXPECT_EQ(!pixel(panel, renderer.getOrientation(), 12, y), ink) << y;
  }
}

TEST_F(SyntheticBoldTest, RealBoldIsNotExpandedAndMixedStyleCursorUsesMeasuredAdvance) {
  EXPECT_EQ(renderer.getTextAdvanceX(2, "AB", B), renderer.getTextAdvanceX(1, "AB", R));
  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  renderer.clearScreen();
  renderer.drawText(1, 10, 10, "AB", true, B);
  const int next = 10 + renderer.getTextAdvanceX(1, "AB", B);
  renderer.drawText(2, next, 10, "C", true, B);
  EXPECT_EQ(next, 20);
  EXPECT_FALSE(pixel(panel, renderer.getOrientation(), 22, 12));
  EXPECT_TRUE(pixel(panel, renderer.getOrientation(), 23, 12));
}

TEST_F(SyntheticBoldTest, StripCullingIncludesTheSyntheticColumnAtABandBoundary) {
  renderer.setOrientation(GfxRenderer::Portrait);
  // Glyph at logical x10..12 occupies physical rows469..467. Only its
  // synthetic x13 column reaches physical row466.
  std::array<uint8_t, 100> strip;
  strip.fill(0xFF);
  renderer.beginStripTarget(strip.data(), 466, 1);
  renderer.drawText(1, 10, 10, "A", true, B);
  renderer.endStripTarget();
  EXPECT_EQ(strip[12 / 8] & (0x80 >> (12 % 8)), 0);
  EXPECT_EQ(strip[13 / 8] & (0x80 >> (13 % 8)), 0);
}

TEST_F(SyntheticBoldTest, SdAdvanceTableMeasurementDoesNotReloadBitmapsForSyntheticBold) {
  storage_test::files["font"] = cpfont(1);
  SdCardFont sd;
  ASSERT_TRUE(sd.load("font"));
  renderer.registerSdCardFont(100, &sd);
  renderer.insertFont(100, EpdFontFamily(sd.getEpdFont()));
  ASSERT_EQ(sd.buildAdvanceTable("AB", 2), 0);
  const size_t reads = storage_test::reads;
  EXPECT_EQ(renderer.getTextAdvanceX(100, "AB", B), 10);
  EXPECT_EQ(storage_test::reads, reads);
  renderer.removeFont(100);
}

TEST_F(SyntheticBoldTest, GrayCoverageIsMergedBeforeSelectingTheTwoPlanes) {
  EpdFontData grayData = data(true);
  EpdFont font(&grayData);
  renderer.insertFont(5, EpdFontFamily(&font));
  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  // First source row [black, light, dark] becomes [black, black, dark, dark].
  for (auto mode : {GfxRenderer::BW, GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
    renderer.setRenderMode(mode);
    renderer.clearScreen(mode == GfxRenderer::BW ? 0xFF : 0);
    renderer.drawText(5, 10, 10, "A", true, B);
    for (int x = 0; x < 4; ++x) {
      const bool bit = pixel(panel, renderer.getOrientation(), 10 + x, 12);
      EXPECT_EQ(bit, mode == GfxRenderer::BW ? false : x >= 2) << mode << ":" << x;
    }
  }
}

TEST_F(SyntheticBoldTest, TwoBitRasterMatchesPixelPathWithRuntimeGeometryAndStripClipping) {
  EpdFontData grayData = data(true);
  EpdFont font(&grayData);
  renderer.insertFont(5, EpdFontFamily(&font));
  constexpr uint8_t RAW[] = {3, 1, 2, 0, 1, 2};
  for (bool alternateGeometry : {false, true}) {
    panel.width = alternateGeometry ? 641 : HalDisplay::DISPLAY_WIDTH;
    panel.height = alternateGeometry ? 383 : HalDisplay::DISPLAY_HEIGHT;
    panel.stride = alternateGeometry ? 84 : HalDisplay::DISPLAY_WIDTH_BYTES;
    renderer.begin();
    for (int o = 0; o < 4; ++o) {
      renderer.setOrientation(static_cast<GfxRenderer::Orientation>(o));
      const int xs[] = {-2, 0, 10, renderer.getScreenWidth() - 4, renderer.getScreenWidth() - 2};
      const int ys[] = {-3, 0, 17, renderer.getScreenHeight() - 3};
      for (bool bold : {false, true}) {
        for (bool black : {false, true}) {
          for (auto mode : {GfxRenderer::BW, GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
            SCOPED_TRACE(testing::Message()
                         << alternateGeometry << ":" << o << ":" << bold << ":" << black << ":" << mode);
            renderer.setRenderMode(mode);
            const uint8_t initial = mode == GfxRenderer::BW && black ? 0xFF : 0;
            const auto renderText = [&]() {
              for (int x : xs)
                for (int y : ys) renderer.drawText(5, x, y, "A", black, bold ? B : R);
            };
            panel.pixels.fill(0xA5);
            renderer.clearScreen(initial);
            // Independent reference through the unchanged general pixel path.
            for (int x : xs)
              for (int y : ys)
                for (int gy = 0; gy < 2; ++gy) {
                  uint8_t previous = 0;
                  for (int gx = 0; gx < (bold ? 4 : 3); ++gx) {
                    const uint8_t raw = gx < 3 ? RAW[gy * 3 + gx] : 0;
                    const uint8_t merged = bold ? std::max(raw, previous) : raw;
                    previous = raw;
                    const bool paint =
                        mode == GfxRenderer::BW
                            ? merged != 0
                            : (mode == GfxRenderer::GRAYSCALE_LSB ? merged == 2 : merged == 1 || merged == 2);
                    if (paint) renderer.drawPixel(x + gx, y + 2 + gy, mode == GfxRenderer::BW ? black : false);
                  }
                }
            const auto expected = panel.pixels;
            panel.pixels.fill(0xA5);
            renderer.clearScreen(initial);
            renderText();
            EXPECT_EQ(panel.pixels, expected);

            int bandY = 19;
            if (o == GfxRenderer::Portrait) bandY = panel.height - 1 - 12;
            if (o == GfxRenderer::PortraitInverted) bandY = 12;
            if (o == GfxRenderer::LandscapeClockwise) bandY = panel.height - 1 - 19;
            for (int rows : {static_cast<int>(panel.height), 1}) {
              const int y0 = rows == 1 ? bandY : 0;
              const size_t size = static_cast<size_t>(panel.stride) * rows;
              std::vector<uint8_t> strip(size + 32, 0xA5);
              panel.pixels.fill(0xA5);
              const auto bw = panel.pixels;
              renderer.beginStripTarget(strip.data() + 16, y0, rows);
              renderer.clearScreen(initial);
              renderText();
              renderer.endStripTarget();
              EXPECT_TRUE(std::equal(strip.begin() + 16, strip.begin() + 16 + size,
                                     expected.begin() + static_cast<size_t>(y0) * panel.stride));
              EXPECT_EQ(panel.pixels, bw);
              EXPECT_TRUE(std::all_of(strip.begin(), strip.begin() + 16, [](uint8_t v) { return v == 0xA5; }));
              EXPECT_TRUE(std::all_of(strip.end() - 16, strip.end(), [](uint8_t v) { return v == 0xA5; }));
            }
          }
        }
      }
    }
  }
}

TEST_F(SyntheticBoldTest, HalfSizeBoldUsesOneOutputPixelAndMatchingAdvance) {
  const Style style = static_cast<Style>(B | EpdFontFamily::SUP);
  EXPECT_EQ(renderer.getTextAdvanceX(1, "AB", style), 6);
  EXPECT_EQ(renderer.getTextWidth(1, "AB", style), 6);
  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  renderer.clearScreen();
  renderer.drawText(1, 10, 10, "AB", true, style);
  for (int x = 9; x <= 16; ++x) {
    EXPECT_EQ(!pixel(panel, renderer.getOrientation(), x, 13), x >= 10 && x < 16);
  }
}

TEST_F(SyntheticBoldTest, SdPhysicalStyleAndCachedAdvancesMatchTheRasterForEveryMask) {
  for (uint8_t mask = 1; mask < 16; ++mask) {
    storage_test::files["font"] = cpfont(mask);
    SdCardFont sd;
    ASSERT_TRUE(sd.load("font"));
    const int id = 100 + mask;
    EpdFontFamily family(sd.getEpdFont(0), sd.getEpdFont(1), sd.getEpdFont(2), sd.getEpdFont(3));
    renderer.registerSdCardFont(id, &sd);
    renderer.insertFont(id, family);
    for (uint8_t style = 0; style < 4; ++style) {
      const auto request = static_cast<Style>(style);
      EXPECT_EQ(family.getData(request), sd.getEpdFont(sd.resolveStyle(style))->data);
      sd.buildAdvanceTable("AB ", static_cast<uint8_t>(1u << style));
      const int advance = family.needsSyntheticBold(request) ? 10 : 8;
      EXPECT_EQ(renderer.getTextAdvanceX(id, "AB", request), advance) << mask << ":" << style;
      EXPECT_EQ(renderer.getSpaceWidth(id, request), family.needsSyntheticBold(request) ? 3 : 2);
      EXPECT_EQ(renderer.getSpaceAdvance(id, 'A', 'B', request), renderer.getSpaceWidth(id, request));
    }
    renderer.removeFont(id);
  }
}

TEST_F(SyntheticBoldTest, RegularOnlyBuiltInKoreanUsesSameDecompressedBitmapForBold) {
  EpdFont body(&kimchi_batang_14_regular);
  renderer.insertFont(6, EpdFontFamily(&body));
  FontDecompressor decompressor;
  FontCacheManager cache(renderer.getFontMap(), renderer.getSdCardFonts());
  cache.setFontDecompressor(&decompressor);
  renderer.setFontCacheManager(&cache);
  EXPECT_EQ(renderer.getTextAdvanceX(6, "한글", B), renderer.getTextAdvanceX(6, "한글", R) + 2);
  EXPECT_EQ(renderer.getTextAdvanceX(6, "한글", BI), renderer.getTextAdvanceX(6, "한글", B));
  for (int cycle = 0; cycle < 3; ++cycle) {
    cache.releaseSdFontCaches();
    renderer.clearScreen();
    renderer.drawText(6, 10, 10, "한글", true, B);
    EXPECT_TRUE(std::any_of(panel.pixels.begin(), panel.pixels.end(), [](uint8_t p) { return p != 0xFF; }));
  }
  renderer.setFontCacheManager(nullptr);
}

TEST_F(SyntheticBoldTest, PrewarmScopeReusesKoreanBitmapsAcrossStylesAndScopes) {
  EpdFont body(&kimchi_batang_14_regular);
  renderer.insertFont(6, EpdFontFamily(&body));
  FontDecompressor decompressor;
  FontCacheManager cache(renderer.getFontMap(), renderer.getSdCardFonts());
  cache.setFontDecompressor(&decompressor);
  renderer.setFontCacheManager(&cache);
  {
    auto scope = cache.createPrewarmScope();
    renderer.drawText(6, 10, 10, "한글 본문", true, R);
    renderer.drawText(6, 10, 50, "한글", true, B);
    scope.endScanAndPrewarm();
    EXPECT_GT(decompressor.getStats().pageBufferBytes, 0u);
  }
  const auto* glyph = body.getGlyph(0xD55C);
  ASSERT_NE(glyph, nullptr);
  const auto index = static_cast<uint32_t>(glyph - body.data->glyph);
  const auto* bitmap = decompressor.getBitmap(body.data, glyph, index);
  ASSERT_NE(bitmap, nullptr);
  for (const char* text : {"한글 본문", "한글"}) {
    auto scope = cache.createPrewarmScope();
    renderer.drawText(6, 10, 10, text, true, BI);
    scope.endScanAndPrewarm();
    EXPECT_EQ(decompressor.getStats().pageBufferBytes, 0u);
    EXPECT_EQ(decompressor.getStats().pageGlyphsBytes, 0u);
    EXPECT_EQ(decompressor.getStats().peakTempBytes, 0u);
    EXPECT_EQ(decompressor.getBitmap(body.data, glyph, index), bitmap);
    for (auto mode : {GfxRenderer::BW, GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
      renderer.setRenderMode(mode);
      renderer.clearScreen(mode == GfxRenderer::BW ? 0xFF : 0);
      renderer.drawText(6, 10, 10, text, true, BI);
    }
    EXPECT_EQ(decompressor.getStats().cacheMisses, 0u);
  }
  cache.clearCache();
  decompressor.resetStats();
  EXPECT_EQ(decompressor.prewarmCache(body.data, "한글"), 0);
  EXPECT_GT(decompressor.getStats().pageBufferBytes, 0u);
  renderer.setFontCacheManager(nullptr);
}

TEST_F(SyntheticBoldTest, RetainedBitmapsAreReleasedForFontRemovalAndFramebufferLoan) {
  EpdFont body(&kimchi_batang_14_regular);
  renderer.insertFont(6, EpdFontFamily(&body));
  FontDecompressor decompressor;
  FontCacheManager cache(renderer.getFontMap(), renderer.getSdCardFonts());
  cache.setFontDecompressor(&decompressor);
  renderer.setFontCacheManager(&cache);
  auto prewarm = [&] {
    auto scope = cache.createPrewarmScope();
    renderer.drawText(6, 10, 10, "한글", true, R);
    scope.endScanAndPrewarm();
  };
  prewarm();
  {
    GfxRenderer::FrameBufferLoan loan(renderer);
    EXPECT_FALSE(renderer.hasFrameBuffer());
  }
  EXPECT_TRUE(renderer.hasFrameBuffer());
  prewarm();
  EXPECT_GT(decompressor.getStats().pageBufferBytes, 0u);
  renderer.removeFont(6);
  renderer.insertFont(6, EpdFontFamily(&body));
  prewarm();
  EXPECT_GT(decompressor.getStats().pageBufferBytes, 0u);
  renderer.setFontCacheManager(nullptr);
}

namespace {
void expectRowAlignedCompaction(const bool prewarm) {
  struct Case {
    uint8_t width, height;
    std::vector<uint8_t> aligned, packed;
  };
  const Case cases[] = {
      {0, 3, {}, {}},
      {3, 0, {}, {}},
      {1, 1, {0xBF}, {0x80}},
      {2, 1, {0x6F}, {0x60}},
      {3, 1, {0x9F}, {0x9C}},
      {1, 3, {0xFF, 0x7F, 0xBF}, {0xD8}},
      {2, 3, {0x1F, 0xBF, 0x4F}, {0x1B, 0x40}},
      {3, 3, {0x1B, 0xE7, 0x4B}, {0x1B, 0x94, 0x80}},
      {4, 3, {0xE4, 0x1B, 0x93}, {0xE4, 0x1B, 0x93}},
      {5, 2, {0xE4, 0xFF, 0x1B, 0x7F}, {0xE4, 0xC6, 0xD0}},
      {6, 2, {0xE4, 0x6F, 0x1B, 0xBF}, {0xE4, 0x61, 0xBB}},
      {7, 2, {0xE4, 0x6F, 0x1B, 0xBB}, {0xE4, 0x6C, 0x6E, 0xE0}},
      {1, 4, {0xFF, 0x7F, 0xBF, 0x3F}, {0xD8}},
  };
  for (const auto& c : cases) {
    SCOPED_TRACE(testing::Message() << static_cast<int>(c.width) << "x" << static_cast<int>(c.height));
    // One raw DEFLATE stored block; B also checks the following glyph's offset.
    const auto size = static_cast<uint16_t>(c.aligned.size() + 1);
    std::vector<uint8_t> compressed{0x01, static_cast<uint8_t>(size), static_cast<uint8_t>(size >> 8),
                                    static_cast<uint8_t>(size ^ 0xFFu), static_cast<uint8_t>((size ^ 0xFFFFu) >> 8)};
    compressed.reserve(size + 5);
    compressed.insert(compressed.end(), c.aligned.begin(), c.aligned.end());
    compressed.push_back(0x6C);
    const EpdGlyph glyphs[] = {
        {c.width, c.height, 16, 0, 0, static_cast<uint16_t>(c.packed.size()), 0},
        {4, 1, 64, 0, 0, 1, static_cast<uint32_t>(c.packed.size())},
    };
    const EpdFontGroup group{0, static_cast<uint32_t>(compressed.size()), size, 2, 0};
    const EpdUnicodeInterval interval{'A', 'B', 0};
    EpdFontData font{};
    font.bitmap = compressed.data();
    font.glyph = glyphs;
    font.intervals = &interval;
    font.intervalCount = 1;
    font.groups = &group;
    font.groupCount = 1;
    font.is2Bit = true;
    FontDecompressor decompressor;
    if (prewarm) {
      ASSERT_EQ(decompressor.prewarmCache(&font, "AB"), 0);
    }
    for (int pass = 0; pass < 2; ++pass) {
      const auto* a = decompressor.getBitmap(&font, &glyphs[0], 0);
      if (!c.packed.empty()) {
        ASSERT_NE(a, nullptr);
        EXPECT_TRUE(std::equal(c.packed.begin(), c.packed.end(), a));
      }
      const auto* b = decompressor.getBitmap(&font, &glyphs[1], 1);
      ASSERT_NE(b, nullptr);
      EXPECT_EQ(*b, 0x6C);
    }
    if (prewarm) {
      EXPECT_EQ(decompressor.getStats().cacheMisses, 0u);
    }
  }
}
}  // namespace

TEST_F(SyntheticBoldTest, RowAlignedFallbackDiscardsPaddingAndPreservesFollowingGlyph) {
  expectRowAlignedCompaction(false);
}

TEST_F(SyntheticBoldTest, RowAlignedPrewarmMatchesPackedBytesAcrossRowBoundaries) { expectRowAlignedCompaction(true); }
