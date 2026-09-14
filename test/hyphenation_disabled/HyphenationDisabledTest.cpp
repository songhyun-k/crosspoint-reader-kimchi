#include <Utf8.h>
#include <gtest/gtest.h>

#include "lib/Epub/Epub/TokenBoundary.h"
#include "lib/Epub/Epub/hyphenation/HyphenationConfig.h"
#include "lib/Epub/Epub/hyphenation/Hyphenator.h"
#include "lib/Epub/Epub/hyphenation/LanguageRegistry.h"

TEST(HyphenationDisabledTest, RegistryContainsNoLanguages) {
  const auto entries = getLanguageEntries();
  EXPECT_EQ(entries.size, 0u);
  EXPECT_EQ(entries.begin(), entries.end());
  for (const auto& entry : entries) {
    ADD_FAILURE() << "Unexpected pattern: " << entry.primaryTag;
  }
  for (const char* tag : {"en", "fr", "de", "ru", "es", "it", "pl", "sv", "uk", "fi", "ko"}) {
    EXPECT_EQ(getLanguageHyphenatorForPrimaryTag(tag), nullptr) << tag;
  }
}

TEST(HyphenationDisabledTest, StaleEnabledSettingCannotEnablePatterns) {
  EXPECT_FALSE(effectiveHyphenationEnabled(false));
  EXPECT_FALSE(effectiveHyphenationEnabled(true));
}

TEST(HyphenationDisabledTest, DoesNotProduceLanguagePatternBreaks) {
  for (const char* language : {"en", "ENG", "de-DE", "fra", "ko", "", "unknown"}) {
    Hyphenator::setPreferredLanguage(language);
    EXPECT_TRUE(Hyphenator::breakOffsets("hyphenation", false).empty());
    EXPECT_TRUE(Hyphenator::breakOffsets("Quadratkilometer", false).empty());
  }
}

TEST(HyphenationDisabledTest, KeepsVisibleExplicitHyphens) {
  Hyphenator::setPreferredLanguage("de");
  const auto breaks = Hyphenator::breakOffsets("US-Satellitensystems", false);
  ASSERT_EQ(breaks.size(), 1u);
  EXPECT_EQ(breaks.front().byteOffset, 3u);
  EXPECT_FALSE(breaks.front().requiresInsertedHyphen);
  EXPECT_TRUE(TokenBoundary::allowsBreakAfterExplicitHyphen('-'));
}

TEST(HyphenationDisabledTest, KeepsSoftHyphenConditionalAndNonbreakingHyphenUnbreakable) {
  const auto breaks = Hyphenator::breakOffsets("hy\u00ADphenation", false);
  ASSERT_EQ(breaks.size(), 1u);
  EXPECT_EQ(breaks.front().byteOffset, 4u);
  EXPECT_TRUE(breaks.front().requiresInsertedHyphen);
  EXPECT_FALSE(TokenBoundary::allowsBreakAfterExplicitHyphen(0x00AD));
  EXPECT_FALSE(TokenBoundary::allowsBreakAfterExplicitHyphen(0x2011));
  EXPECT_TRUE(Hyphenator::breakOffsets("non\u2011breaking", false).empty());
}

TEST(HyphenationDisabledTest, RetainsEmergencyBreaksForOversizedWords) {
  EXPECT_FALSE(Hyphenator::breakOffsets("oversizedword", true).empty());
  EXPECT_TRUE(Hyphenator::breakOffsets("", true).empty());
}

TEST(HyphenationDisabledTest, KoreanAndCjkBreaksDoNotInsertHyphens) {
  EXPECT_TRUE(utf8IsCjkBreakable(0xAC00));
  EXPECT_TRUE(TokenBoundary::allowsBreak(false, true));
  const auto breaks = Hyphenator::breakOffsets("한글문장이이어집니다", true);
  ASSERT_FALSE(breaks.empty());
  for (const auto& split : breaks) {
    EXPECT_FALSE(split.requiresInsertedHyphen);
    EXPECT_EQ(split.byteOffset % 3, 0u);
  }
}

TEST(HyphenationDisabledTest, KeepsApostropheCompoundBreaksWithoutPatterns) {
  const auto breaks = Hyphenator::breakOffsets("all'improvviso", false);
  ASSERT_EQ(breaks.size(), 1u);
  EXPECT_EQ(breaks.front().byteOffset, 4u);
  EXPECT_FALSE(breaks.front().requiresInsertedHyphen);
}
