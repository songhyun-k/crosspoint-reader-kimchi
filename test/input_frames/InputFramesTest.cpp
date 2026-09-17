#include <HalGPIO.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <new>

namespace {
bool countAllocations = false;
size_t allocationCount = 0;
uint32_t nowMs;
int group1 = 4095, group2 = 4095, power = HIGH;
constexpr uint8_t DOWN = 1u << HalGPIO::BTN_DOWN;
constexpr uint8_t POWER = 1u << HalGPIO::BTN_POWER;
}  // namespace
void* operator new(size_t size) {
  if (countAllocations) ++allocationCount;
  if (void* memory = std::malloc(size)) return memory;
  throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }

unsigned long millis() { return nowMs; }
int analogRead(int pin) { return pin == 1 ? group1 : group2; }
int analogReadMilliVolts(int pin) { return analogRead(pin); }
int digitalRead(int) { return power; }
bool HalGPIO::isUsbConnected() const { return false; }

TEST(InputFramesTest, RawAdcDebounceAndDelayedQueriesKeepOneObservation) {
  for (const auto board : {BoardConfig::Board::XteinkX4, BoardConfig::Board::XteinkX3}) {
    BoardConfig::selectDevice(board);
    HalGPIO input;
    nowMs = 100;
    group1 = group2 = 4095;
    power = HIGH;
    input.update();
    nowMs = 110;
    group2 = 5;
    input.update();
    EXPECT_TRUE(input.isDebouncePending());
    nowMs = 115;
    input.update();
    EXPECT_FALSE(input.wasAnyPressed());  // SDK uses strictly more than 5ms.
    nowMs = 116;
    input.update();
    EXPECT_TRUE(input.wasPressed(HalGPIO::BTN_DOWN));
    const auto press = input.getButtonFrame();
    nowMs = 1116;
    EXPECT_EQ(input.getButtonFrame().sequence, press.sequence);
    EXPECT_TRUE(input.wasPressed(HalGPIO::BTN_DOWN));
    EXPECT_TRUE(input.wasPressed(HalGPIO::BTN_DOWN));
    EXPECT_EQ(input.getHeldTime(), 0u);
    EXPECT_EQ(input.getButtonHeldTime(HalGPIO::BTN_DOWN), 0u);
    EXPECT_EQ(input.getButtonFrame().capturedAtMs, 116u);
    nowMs = 1156;
    group2 = 4095;
    input.update();
    nowMs = 1162;
    input.update();
    EXPECT_EQ(input.getButtonFrame().released, DOWN);
    EXPECT_EQ(input.getHeldTime(), 1046u);
    nowMs = 2162;
    EXPECT_EQ(input.getHeldTime(), 1046u);
    EXPECT_EQ(input.getButtonHeldTime(HalGPIO::BTN_DOWN), 1046u);
    input.update();
    EXPECT_FALSE(input.wasAnyReleased());
  }
}

TEST(InputFramesTest, SimultaneousEdgesKeepIndependentHoldsAndScopedRelease) {
  HalGPIO input;
  nowMs = 100;
  group1 = group2 = 4095;
  power = HIGH;
  input.update();
  nowMs = 110;
  power = LOW;
  input.update();
  nowMs = 116;
  input.update();
  nowMs = 700;
  group2 = 5;
  input.update();
  nowMs = 706;
  input.update();
  EXPECT_EQ(input.getButtonHeldTime(HalGPIO::BTN_POWER), 590u);
  EXPECT_EQ(input.getButtonHeldTime(HalGPIO::BTN_DOWN), 0u);
  nowMs = 750;
  group2 = 4095;
  power = HIGH;
  input.update();
  nowMs = 756;
  input.update();
  EXPECT_EQ(input.getButtonFrame().released, POWER | DOWN);
  EXPECT_EQ(input.getButtonHeldTime(HalGPIO::BTN_DOWN), 50u);
  input.suppressButtonReleases(POWER);
  EXPECT_FALSE(input.wasReleased(HalGPIO::BTN_POWER));
  EXPECT_TRUE(input.wasReleased(HalGPIO::BTN_DOWN));
  EXPECT_TRUE(input.wasAnyReleased());
  nowMs = 770;
  power = LOW;
  input.update();
  nowMs = 776;
  input.update();
  EXPECT_EQ(input.getButtonHeldTime(HalGPIO::BTN_POWER), 0u);
  nowMs = 800;
  power = HIGH;
  input.update();
  nowMs = 806;
  input.update();
  EXPECT_TRUE(input.wasReleased(HalGPIO::BTN_POWER));
}

TEST(InputFramesTest, CapturedButtonDurationWrapsAtTargetMillisWidth) {
  HalGPIO input;
  nowMs = UINT32_MAX - 30;
  group1 = group2 = 4095;
  power = HIGH;
  input.update();
  nowMs = UINT32_MAX - 20;
  group2 = 5;
  input.update();
  nowMs = UINT32_MAX - 14;
  input.update();
  nowMs = 6;
  input.update();
  EXPECT_EQ(input.getButtonHeldTime(HalGPIO::BTN_DOWN), 21u);
}

TEST(InputFramesTest, RepeatedFramesDoNotAllocateOrRetainAnotherContactsDuration) {
  HalGPIO input;
  nowMs = 100;
  group1 = group2 = 4095;
  power = HIGH;
  input.update();
  allocationCount = 0;
  countAllocations = true;
  uint32_t presses = 0, releases = 0;
  bool ordered = true;
  auto sequence = input.getButtonFrame().sequence;
  for (int contact = 0; contact < 1000; ++contact) {
    nowMs += 10;
    group2 = 5;
    input.update();
    nowMs += 6;
    input.update();
    ordered &= input.getButtonFrame().sequence == sequence + 2 && input.getButtonHeldTime(HalGPIO::BTN_DOWN) == 0;
    presses += input.wasPressed(HalGPIO::BTN_DOWN);
    nowMs += 30;
    group2 = 4095;
    input.update();
    nowMs += 6;
    input.update();
    ordered &= input.getButtonFrame().sequence == sequence + 4 && input.getButtonHeldTime(HalGPIO::BTN_DOWN) == 36;
    releases += input.wasReleased(HalGPIO::BTN_DOWN);
    sequence += 4;
  }
  countAllocations = false;
  EXPECT_EQ(allocationCount, 0u);
  EXPECT_TRUE(ordered);
  EXPECT_EQ(presses, 1000u);
  EXPECT_EQ(releases, 1000u);
}
