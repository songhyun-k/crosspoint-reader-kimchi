#include <EpdFont.h>
#include <FontDecompressor.h>
#include <HalStorage.h>
#include <I18n.h>
#include <SdCardFont.h>
#include <Utf8.h>
#include <builtinFonts/kimchi_batang_14_regular.h>
#include <builtinFonts/kimchi_ui_10_regular.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

#include "ReaderFontSizes.h"

namespace {
size_t coverage(const EpdFont& font, const uint32_t first, const uint32_t last) {
  size_t count = 0;
  for (uint32_t cp = first; cp <= last; ++cp) count += font.hasCodepoint(cp);
  return count;
}
}  // namespace

TEST(KoreanFontsTest, UiIsRawAndBodyRemainsCompressed) {
  EXPECT_EQ(kimchi_ui_10_regular.groups, nullptr);
  EXPECT_EQ(kimchi_ui_10_regular.groupCount, 0u);
  EXPECT_NE(kimchi_batang_14_regular.groups, nullptr);
  EXPECT_GT(kimchi_batang_14_regular.groupCount, 0u);
}

TEST(KoreanFontsTest, RetainsAllAgreedSyllablesAndHanja) {
  const EpdFont ui(&kimchi_ui_10_regular), body(&kimchi_batang_14_regular);
  EXPECT_EQ(coverage(ui, 0xAC00, 0xD7A3), 2350u);
  EXPECT_EQ(coverage(body, 0xAC00, 0xD7A3), 11172u);
  EXPECT_EQ(coverage(body, 0x4E00, 0x9FFF), 4620u);
  EXPECT_TRUE(body.hasCodepoint(0x3000));
  EXPECT_GT(coverage(body, 0x3130, 0x318F), 0u);
}

TEST(KoreanFontsTest, CompiledUiTranslationsHaveRealBuiltInGlyphs) {
  const EpdFont ui(&kimchi_ui_10_regular);
  for (const Language language : {Language::EN, Language::KO}) {
    const auto* p = reinterpret_cast<const uint8_t*>(I18n::getCharacterSet(language));
    while (const uint32_t cp = utf8NextCodepoint(&p)) {
      if (cp < 0x20) continue;
      EXPECT_TRUE(ui.hasCodepoint(cp)) << "missing U+" << std::hex << cp;
    }
  }
}

TEST(KoreanFontsTest, EveryGroupDecodesWithTheActualFirmwareInflater) {
  FontDecompressor decompressor;
  ASSERT_TRUE(decompressor.init());
  for (const EpdFontData* data : {&kimchi_ui_10_regular, &kimchi_batang_14_regular}) {
    for (uint16_t groupId = 0; groupId < data->groupCount; ++groupId) {
      const auto& group = data->groups[groupId];
      ASSERT_LE(group.uncompressedSize, 8192u);
      for (uint32_t i = group.firstGlyphIndex; i < group.firstGlyphIndex + group.glyphCount; ++i) {
        const auto* glyph = &data->glyph[i];
        if (glyph->dataLength == 0) continue;
        ASSERT_NE(decompressor.getBitmap(data, glyph, i), nullptr) << groupId << ":" << i;
      }
    }
  }
}

TEST(KoreanFontsTest, PrewarmAndCacheReleaseReproduceTheSamePackedRaster) {
  const EpdFont font(&kimchi_batang_14_regular);
  FontDecompressor decompressor;
  const auto* glyph = font.getGlyph(0xD55C);
  ASSERT_NE(glyph, nullptr);
  const auto index = static_cast<uint32_t>(glyph - font.data->glyph);
  const auto* bitmap = decompressor.getBitmap(font.data, glyph, index);
  ASSERT_NE(bitmap, nullptr);
  const std::vector<uint8_t> reference(bitmap, bitmap + glyph->dataLength);
  for (int cycle = 0; cycle < 8; ++cycle) {
    decompressor.clearCache();
    ASSERT_EQ(decompressor.prewarmCache(font.data, "한글 漢字 Hello 0123"), 0);
    bitmap = decompressor.getBitmap(font.data, glyph, index);
    ASSERT_NE(bitmap, nullptr);
    EXPECT_EQ(std::vector<uint8_t>(bitmap, bitmap + glyph->dataLength), reference);
    EXPECT_LE(decompressor.getStats().peakTempBytes, 8192u);
  }
}

TEST(KoreanFontsTest, PointSizeAndLegacyFamilyPoliciesStayConsistent) {
  EXPECT_EQ(readerFontPointSizes(nullptr, "", KOPUB_READER_FAMILY), std::vector<uint8_t>({14}));
  EXPECT_EQ(readerFontPointSizes(nullptr, "", 0), std::vector<uint8_t>({12, 14, 16, 18}));
  EXPECT_EQ(snapToBuiltinPointSize(18, KOPUB_READER_FAMILY), 14);
  SdCardFontRegistry registry;
  registry.families.push_back({"KoPubBatang", {12, 14, 16, 18}});
  EXPECT_EQ(readerFontPointSizes(&registry, "KoPubBatang", KOPUB_READER_FAMILY),
            std::vector<uint8_t>({12, 14, 16, 18}));
  EXPECT_EQ(readerFontPointSizes(&registry, "missing", KOPUB_READER_FAMILY), std::vector<uint8_t>({14}));
}

TEST(KoreanFontsTest, LocallyGeneratedCpfontsLoadWithTheUnchangedV4Reader) {
  const char* directory = std::getenv("KIMCHI_SD_FONT_FIXTURES");
  if (!directory) GTEST_SKIP() << "Run build-sd-fonts.py, then set KIMCHI_SD_FONT_FIXTURES to its output directory";
  storage_test::reset();
  for (const int size : {12, 14, 16, 18}) {
    const std::string name = "KoPubBatang_" + std::to_string(size) + ".cpfont";
    std::ifstream input(std::string(directory) + "/KoPubBatang/" + name, std::ios::binary);
    ASSERT_TRUE(input.good());
    storage_test::files[name] = std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
    SdCardFont sd;
    ASSERT_TRUE(sd.load(name.c_str()));
    ASSERT_EQ(sd.styleCount(), 1);
    EXPECT_FALSE(sd.hasStyle(1));
    const auto* font = sd.getEpdFont();
    ASSERT_NE(font, nullptr);
    EXPECT_EQ(coverage(*font, 0xAC00, 0xD7A3), 11172u);
    EXPECT_EQ(coverage(*font, 0x4E00, 0x9FFF), 4620u);
    ASSERT_EQ(sd.prewarm("한글 漢字 Hello 0123", 1), 0);
    ASSERT_NE(font->getGlyph(0xD55C), nullptr);
    sd.releaseResidentCaches();
    ASSERT_EQ(sd.prewarm("한글", 1), 0);
  }
}
