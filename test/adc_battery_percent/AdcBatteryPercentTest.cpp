#include <gtest/gtest.h>

#include "lib/hal/AdcBatteryPercent.h"

TEST(AdcBatteryPercentTest, PreservesTheLowerRangeAndCalibratesTheX4UpperRange) {
  constexpr uint16_t upper[] = {80, 82, 84, 85, 87, 89, 91, 93, 95, 96, 98, 100};
  uint16_t previous = 0;
  for (uint16_t raw = 0; raw <= 100; ++raw) {
    const auto shown = battery_percent::correctX4AdcSample(raw);
    const auto expected = raw < 80 ? raw : raw <= 91 ? upper[raw - 80] : 100;
    EXPECT_EQ(shown, expected) << raw;
    EXPECT_GE(shown, previous);
    previous = shown;
  }
}
