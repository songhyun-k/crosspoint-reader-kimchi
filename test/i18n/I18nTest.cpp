#include <I18n.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>

TEST(I18nTest, InitialLanguageIsKorean) {
  EXPECT_EQ(I18n::DEFAULT_LANGUAGE, Language::KO);
  EXPECT_EQ(I18N.getLanguage(), Language::KO);
  EXPECT_STREQ(tr(STR_SETTINGS_TITLE), "설정");
}

TEST(I18nTest, SavedCodesSurviveAResetWithoutDependingOnEnumOrder) {
  for (const auto language : {Language::EN, Language::KO}) {
    I18N.setLanguage(language);
    const std::string savedCode = I18n::languageToCode(I18N.getLanguage());
    EXPECT_EQ(savedCode, language == Language::EN ? "EN" : "KO");
    I18N.setLanguage(I18n::DEFAULT_LANGUAGE);
    I18N.setLanguage(I18n::languageFromCode(savedCode.c_str()));
    EXPECT_EQ(I18N.getLanguage(), language);
    EXPECT_STREQ(tr(STR_SETTINGS_TITLE), language == Language::EN ? "Settings" : "설정");
  }
}

TEST(I18nTest, LegacyKoreanCodeAndInvalidCodesHaveSafeDefaults) {
  EXPECT_EQ(I18n::languageFromCode("KOREAN"), Language::KO);
  EXPECT_EQ(I18n::languageFromCode(nullptr), Language::KO);
  EXPECT_EQ(I18n::languageFromCode(""), Language::KO);
  EXPECT_EQ(I18n::languageFromCode("not-a-language"), Language::KO);
  EXPECT_STREQ(I18n::languageToCode(static_cast<Language>(255)), "KO");
}

TEST(I18nTest, InvalidSelectionCannotChangeChosenLanguage) {
  I18N.setLanguage(Language::EN);
  I18N.setLanguage(static_cast<Language>(255));
  EXPECT_EQ(I18N.getLanguage(), Language::EN);
  EXPECT_STREQ(I18N.get(static_cast<StrId>(65535)), "???");
  EXPECT_STREQ(I18N.getLanguageName(static_cast<Language>(255)), "???");
}

TEST(I18nTest, EveryGeneratedEnglishAndKoreanStringIsReadable) {
  for (const auto language : {Language::EN, Language::KO}) {
    I18N.setLanguage(language);
    for (unsigned key = 0; key < static_cast<unsigned>(StrId::_COUNT); ++key) {
      const char* value = I18N.get(static_cast<StrId>(key));
      ASSERT_NE(value, nullptr);
      EXPECT_GT(std::strlen(value), 0u) << key;
    }
  }
}

TEST(I18nTest, KoreanPrintfFieldsPreserveArgumentTypesAndEscapedPercent) {
  I18N.setLanguage(Language::KO);
  char text[160];
  std::snprintf(text, sizeof(text), tr(STR_NETWORKS_FOUND), size_t{3});
  EXPECT_STREQ(text, "3개의 네트워크 발견");
  std::snprintf(text, sizeof(text), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), 4, 12, 25.5);
  EXPECT_STREQ(text, "4/12 페이지, 전체 25.50%");
}
