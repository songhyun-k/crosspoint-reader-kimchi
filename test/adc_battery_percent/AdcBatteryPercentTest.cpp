#include <gtest/gtest.h>

#include "lib/hal/AdcBatteryPercent.h"

TEST(AdcBatteryPercentTest, LeavesZeroThroughKneeUnchanged) {
  for (uint16_t raw = 0; raw <= 80; ++raw) {
    int cached = 0;
    EXPECT_EQ(battery_percent::smoothAdcSample(raw, true, cached), raw);
    EXPECT_EQ(cached, raw * 10);
  }
}

TEST(AdcBatteryPercentTest, StretchesTheUpperRangeWithNearestIntegerRounding) {
  constexpr uint16_t expected[] = {80, 82, 84, 85, 87, 89, 91, 93, 95, 96, 98, 100};
  for (uint16_t raw = 80; raw <= 91; ++raw) {
    int cached = 0;
    EXPECT_EQ(battery_percent::smoothAdcSample(raw, true, cached), expected[raw - 80]);
  }
}

TEST(AdcBatteryPercentTest, ClampsAtObservedFullAndStaysMonotonic) {
  uint16_t previous = 0;
  for (uint16_t raw = 0; raw <= 100; ++raw) {
    int cached = 0;
    const auto shown = battery_percent::smoothAdcSample(raw, true, cached);
    EXPECT_GE(shown, previous);
    EXPECT_LE(shown, 100);
    if (raw >= 91) EXPECT_EQ(shown, 100);
    previous = shown;
  }
}

TEST(AdcBatteryPercentTest, DoesNotChangeOtherAdcOrPmicBoards) {
  for (uint16_t raw = 0; raw <= 100; ++raw) {
    int cached = 0;
    EXPECT_EQ(battery_percent::smoothAdcSample(raw, false, cached), raw);
  }
  int cached = 500;
  EXPECT_EQ(battery_percent::smoothAdcSample(91, false, cached), 54);
  EXPECT_EQ(cached, 541);
}

TEST(AdcBatteryPercentTest, CorrectsBeforeSmoothingAndPreservesTenths) {
  int cached = 500;
  EXPECT_EQ(battery_percent::smoothAdcSample(91, true, cached), 55);
  EXPECT_EQ(cached, 550);
  EXPECT_EQ(battery_percent::smoothAdcSample(91, true, cached), 59);
  EXPECT_EQ(cached, 595);
  EXPECT_EQ(battery_percent::smoothAdcSample(80, true, cached), 61);
  EXPECT_EQ(cached, 615);
}

TEST(AdcBatteryPercentTest, KeepsExistingZeroCacheInitializationBehavior) {
  int cached = 0;
  EXPECT_EQ(battery_percent::smoothAdcSample(0, true, cached), 0);
  EXPECT_EQ(battery_percent::smoothAdcSample(91, true, cached), 100);
  EXPECT_EQ(cached, 1000);
}
