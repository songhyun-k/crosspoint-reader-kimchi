#include <KeyboardLayoutSet.h>
#include <gtest/gtest.h>

#include <string>

namespace fui = freeink::ui;

namespace {
std::string rowText(const fui::KeyboardRow& row) {
  std::string text;
  for (uint8_t i = 0; i < row.count; ++i) {
    if (row.keys[i].kind == fui::KeyKind::Normal && row.keys[i].output) text += row.keys[i].output;
  }
  return text;
}

bool containsKey(const fui::KeyboardLayout& layout, const int16_t value) {
  for (uint8_t r = 0; r < layout.rowCount; ++r) {
    for (uint8_t c = 0; c < layout.rows[r].count; ++c) {
      if (layout.rows[r].keys[c].value == value) return true;
    }
  }
  return false;
}
}  // namespace

TEST(QwertyKeyboardTest, HasOneStableEnglishMaskAndNoLanguageSwitch) {
  EXPECT_EQ(keyboard_layouts::COUNT, 1);
  EXPECT_EQ(keyboard_layouts::enabled(), 1);
  EXPECT_EQ(keyboard_layouts::LATIN_BITS, 1);
  EXPECT_EQ(keyboard_layouts::startingLayout(), fui::KeyboardLayoutId::QwertyEn);
  const auto& layout = fui::builtinKeyboardLayout(keyboard_layouts::startingLayout(), false, false, true, false);
  EXPECT_FALSE(containsKey(layout, fui::QWERTY_KEY_LANG));
}

TEST(QwertyKeyboardTest, StaleLayoutIdsAlwaysReturnToQwerty) {
  for (unsigned id = 0; id <= 255; ++id) {
    EXPECT_EQ(keyboard_layouts::next(static_cast<fui::KeyboardLayoutId>(id)), fui::KeyboardLayoutId::QwertyEn);
  }
}

TEST(QwertyKeyboardTest, KeepsTheSdkLowercaseLetterAndNumberRows) {
  const auto& layout = fui::builtinKeyboardLayout(keyboard_layouts::startingLayout(), false, false, true);
  ASSERT_EQ(layout.rowCount, 5);
  EXPECT_EQ(rowText(layout.rows[0]), "1234567890");
  EXPECT_EQ(rowText(layout.rows[1]), "qwertyuiop");
  EXPECT_EQ(rowText(layout.rows[2]), "asdfghjkl");
  EXPECT_EQ(rowText(layout.rows[3]), "zxcvbnm");
  EXPECT_STREQ(fui::keyboardOutputFor(layout, 'q'), "q");
  EXPECT_TRUE(containsKey(layout, fui::QWERTY_KEY_SHIFT));
  EXPECT_TRUE(containsKey(layout, fui::QWERTY_KEY_BACKSPACE));
  EXPECT_TRUE(containsKey(layout, fui::QWERTY_KEY_ENTER));
}

TEST(QwertyKeyboardTest, KeepsTheSdkShiftedLetterLayer) {
  const auto& layout = fui::builtinKeyboardLayout(keyboard_layouts::startingLayout(), true, false, true);
  ASSERT_EQ(layout.rowCount, 5);
  EXPECT_EQ(rowText(layout.rows[0]), "!@#$%^&*()");
  EXPECT_EQ(rowText(layout.rows[1]), "QWERTYUIOP");
  EXPECT_EQ(rowText(layout.rows[2]), "ASDFGHJKL");
  EXPECT_EQ(rowText(layout.rows[3]), "ZXCVBNM");
}

TEST(QwertyKeyboardTest, KeepsBothSymbolPagesAndPasswordPunctuation) {
  const auto& first = fui::builtinKeyboardLayout(keyboard_layouts::startingLayout(), false, true);
  const auto& second = fui::builtinKeyboardLayout(keyboard_layouts::startingLayout(), true, true);
  EXPECT_NE(&first, &second);
  std::string symbols;
  for (const auto* page : {&first, &second}) {
    for (uint8_t r = 0; r < page->rowCount; ++r) symbols += rowText(page->rows[r]);
    EXPECT_TRUE(containsKey(*page, fui::QWERTY_KEY_MODE));
    EXPECT_TRUE(containsKey(*page, fui::QWERTY_KEY_BACKSPACE));
    EXPECT_TRUE(containsKey(*page, fui::QWERTY_KEY_ENTER));
  }
  for (char c : std::string("0123456789!@#$%^&*()/:.")) EXPECT_NE(symbols.find(c), std::string::npos) << c;
}
