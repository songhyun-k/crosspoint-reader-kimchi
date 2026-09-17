#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <future>
#include <new>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"

namespace {
std::atomic<bool> countAllocations{false};
std::atomic<size_t> allocationCount{0};
std::atomic<unsigned> adcReads{0}, consumerAdcReads{0}, samplerUsbReads{0};
std::mutex adcGateMutex;
std::condition_variable adcGateCondition;
bool pauseAdc = false, adcEntered = false, resumeAdc = false;
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
int analogRead(int pin) {
  if (inputTest::currentTask) {
    std::unique_lock lock(adcGateMutex);
    if (pauseAdc) {
      pauseAdc = false;
      adcEntered = true;
      adcGateCondition.notify_all();
      adcGateCondition.wait(lock, [] { return resumeAdc; });
    }
  }
  ++adcReads;
  if (inputTest::task && !inputTest::currentTask) ++consumerAdcReads;
  return pin == 1 ? group1 : group2;
}
int analogReadMilliVolts(int pin) { return analogRead(pin); }
int digitalRead(int) { return power; }
bool HalGPIO::isUsbConnected() const {
  if (inputTest::currentTask) ++samplerUsbReads;
  return false;
}

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
  EXPECT_EQ(allocationCount.load(), 0u);
  EXPECT_TRUE(ordered);
  EXPECT_EQ(presses, 1000u);
  EXPECT_EQ(releases, 1000u);
}

namespace {
bool startSampler(HalGPIO& input) {
  nowMs += 100;
  group1 = group2 = 4095;
  power = HIGH;
  consumerAdcReads = samplerUsbReads = 0;
  if (!input.startButtonSampling()) return false;
  inputTest::waitForSample();
  return true;
}
void sample() {
  nowMs += 10;
  inputTest::nextSample();
}
void rawTap() {
  group2 = 5;
  for (int i = 0; i < 4; ++i) sample();
  group2 = 4095;
  for (int i = 0; i < 4; ++i) sample();
}
}  // namespace

TEST(InputFramesTest, SamplerRetainsBurstWhileConsumerIsBlockedWithoutAllocating) {
  HalGPIO input;
  ASSERT_TRUE(startSampler(input));
  allocationCount = 0;
  countAllocations = true;
  for (int tap = 0; tap < 12; ++tap) rawTap();
  countAllocations = false;
  EXPECT_EQ(allocationCount.load(), 0u);
  EXPECT_EQ(input.getPhysicalButtonFrame().droppedFrames, 0u);
  EXPECT_EQ(inputTest::queue->highWater, 24u);
  uint32_t previousSequence = 0;
  uint32_t previousAtMs = 0;
  const auto readsBeforeDrain = adcReads.load();
  nowMs += 5000;  // Only consumer time advances; the sampler is parked by the test boundary.
  for (int tap = 0; tap < 12; ++tap) {
    ASSERT_TRUE(input.hasPendingButtonFrames());
    input.update();
    const auto press = input.getButtonFrame();
    EXPECT_EQ(press.pressed, DOWN);
    EXPECT_EQ(press.released, 0u);
    EXPECT_GT(press.sequence, previousSequence);
    EXPECT_GT(press.capturedAtMs, previousAtMs);
    EXPECT_EQ(input.getButtonHeldTime(HalGPIO::BTN_DOWN), 0u);
    EXPECT_EQ(input.getHeldTime(), 0u);
    input.update();
    const auto release = input.getButtonFrame();
    EXPECT_EQ(release.released, DOWN);
    EXPECT_EQ(release.pressed, 0u);
    EXPECT_GT(release.sequence, press.sequence);
    EXPECT_EQ(release.capturedAtMs - press.capturedAtMs, 40u);
    EXPECT_EQ(input.getButtonHeldTime(HalGPIO::BTN_DOWN), 40u);
    previousSequence = release.sequence;
    previousAtMs = release.capturedAtMs;
  }
  EXPECT_FALSE(input.hasPendingButtonFrames());
  input.update();
  EXPECT_FALSE(input.wasAnyReleased());
  EXPECT_EQ(adcReads.load(), readsBeforeDrain);
  EXPECT_EQ(consumerAdcReads.load(), 0u);
  EXPECT_EQ(samplerUsbReads.load(), 0u);
  input.stopButtonSampling();
  EXPECT_EQ(inputTest::task, nullptr);
}

TEST(InputFramesTest, FullQueueRejectsReleaseAndResynchronizesHeldButtons) {
  HalGPIO input;
  ASSERT_TRUE(startSampler(input));
  for (int tap = 0; tap < 15; ++tap) rawTap();
  group2 = 5;
  sample();
  sample();  // 31st edge: side press.
  power = LOW;
  sample();
  sample();  // 32nd edge: power joins the held side button.
  EXPECT_EQ(inputTest::queue->highWater, 32u);
  EXPECT_EQ(input.getPhysicalButtonFrame().droppedFrames, 0u);
  group2 = 4095;
  power = HIGH;
  sample();
  sample();  // Full FIFO: reject the combined release, retain its physical state.
  EXPECT_EQ(input.getPhysicalButtonFrame().droppedFrames, 1u);
  unsigned edges = 0;
  while (input.hasPendingButtonFrames()) {
    input.update();
    ++edges;
  }
  EXPECT_EQ(edges, 32u);
  EXPECT_TRUE(input.isPressed(HalGPIO::BTN_DOWN));
  EXPECT_TRUE(input.isPressed(HalGPIO::BTN_POWER));
  input.update();
  EXPECT_EQ(input.getButtonFrame().droppedFrames, 1u);
  EXPECT_FALSE(input.isPressed(HalGPIO::BTN_DOWN));
  EXPECT_FALSE(input.isPressed(HalGPIO::BTN_POWER));
  EXPECT_FALSE(input.wasAnyReleased());
  rawTap();
  input.update();
  EXPECT_TRUE(input.wasPressed(HalGPIO::BTN_DOWN));
  input.update();
  EXPECT_TRUE(input.wasReleased(HalGPIO::BTN_DOWN));
}

TEST(InputFramesTest, ContextDiscardMutesHeldAndUnsettledContactsUntilRelease) {
  for (bool unsettled : {false, true}) {
    HalGPIO input;
    ASSERT_TRUE(startSampler(input));
    group2 = 5;
    sample();
    if (!unsettled) sample();
    input.discardButtonInput();
    EXPECT_FALSE(input.hasPendingButtonFrames());
    EXPECT_FALSE(input.isPressed(HalGPIO::BTN_DOWN));
    for (int i = 0; i < 100; ++i) {
      sample();
      input.update();
    }
    EXPECT_FALSE(input.wasAnyPressed());
    EXPECT_FALSE(input.isPressed(HalGPIO::BTN_DOWN));
    group2 = 4095;
    sample();
    sample();
    input.update();
    EXPECT_FALSE(input.wasAnyReleased());
    rawTap();
    input.update();
    EXPECT_TRUE(input.wasPressed(HalGPIO::BTN_DOWN));
    input.update();
    EXPECT_TRUE(input.wasReleased(HalGPIO::BTN_DOWN));
  }
}

TEST(InputFramesTest, StartupFailuresLeaveTheSynchronousOwnerUsableAndStopJoins) {
  HalGPIO input;
  nowMs = 100;
  group1 = group2 = 4095;
  power = HIGH;
  inputTest::failQueueCreate = true;
  EXPECT_FALSE(input.startButtonSampling());
  input.discardButtonInput();
  inputTest::failQueueCreate = false;
  inputTest::failTaskCreate = true;
  EXPECT_FALSE(input.startButtonSampling());
  inputTest::failTaskCreate = false;
  const auto before = adcReads.load();
  input.update();
  EXPECT_GT(adcReads.load(), before);
  ASSERT_TRUE(startSampler(input));
  rawTap();
  input.stopButtonSampling();
  EXPECT_EQ(inputTest::task, nullptr);
  EXPECT_FALSE(input.hasPendingButtonFrames());
  const auto afterStop = adcReads.load();
  input.update();
  EXPECT_GT(adcReads.load(), afterStop);
  ASSERT_TRUE(startSampler(input));
  rawTap();
  input.update();
  EXPECT_TRUE(input.wasPressed(HalGPIO::BTN_DOWN));
  input.update();
  EXPECT_TRUE(input.wasReleased(HalGPIO::BTN_DOWN));
}

TEST(InputFramesTest, StopWaitsForAnInFlightRawReadBeforeReturningSdkOwnership) {
  HalGPIO input;
  ASSERT_TRUE(startSampler(input));
  {
    std::lock_guard lock(adcGateMutex);
    pauseAdc = true;
    adcEntered = resumeAdc = false;
  }
  nowMs += 10;
  auto pendingSample = std::async(std::launch::async, inputTest::nextSample);
  {
    std::unique_lock lock(adcGateMutex);
    adcGateCondition.wait(lock, [] { return adcEntered; });
  }
  const auto handle = inputTest::task;
  auto stopped = std::async(std::launch::async, [&] { input.stopButtonSampling(); });
  {
    std::unique_lock lock(handle->mutex);
    handle->condition.wait(lock, [handle] { return handle->notified; });
  }
  EXPECT_EQ(stopped.wait_for(std::chrono::seconds(0)), std::future_status::timeout);
  {
    std::lock_guard lock(adcGateMutex);
    resumeAdc = true;
    adcGateCondition.notify_all();
  }
  pendingSample.get();
  stopped.get();
  EXPECT_EQ(inputTest::task, nullptr);
  const auto before = adcReads.load();
  input.update();
  EXPECT_EQ(adcReads.load() - before, 2u);
}

TEST(InputFramesTest, MapperClassifiesAQueuedLongReleaseOnceAndKeepsTheNextShortTap) {
  HalGPIO input;
  HalDisplay display;
  GfxRenderer renderer(display);
  MappedInputManager mapped(input, renderer);
  SETTINGS.frontButtonConfirm = HalGPIO::BTN_CONFIRM;
  ASSERT_TRUE(startSampler(input));
  group1 = 2694;
  for (int i = 0; i < 80; ++i) sample();
  group1 = 4095;
  sample();
  sample();
  nowMs += 1000;
  mapped.update();
  EXPECT_FALSE(mapped.wasLongPressed(MappedInputManager::Button::Confirm, 600));
  mapped.update();
  EXPECT_TRUE(mapped.wasLongPressed(MappedInputManager::Button::Confirm, 600));
  EXPECT_FALSE(mapped.wasLongPressed(MappedInputManager::Button::Confirm, 600));
  EXPECT_FALSE(mapped.wasReleased(MappedInputManager::Button::Confirm));
  group1 = 2694;
  for (int i = 0; i < 4; ++i) sample();
  group1 = 4095;
  for (int i = 0; i < 4; ++i) sample();
  mapped.update();
  EXPECT_FALSE(mapped.wasLongPressed(MappedInputManager::Button::Confirm, 600));
  mapped.update();
  EXPECT_FALSE(mapped.wasLongPressed(MappedInputManager::Button::Confirm, 600));
  EXPECT_TRUE(mapped.wasReleased(MappedInputManager::Button::Confirm));
}

TEST(InputFramesTest, MapperSuppressesOnlyOnePhysicalContactAcrossAliasesAndContextChanges) {
  HalGPIO input;
  HalDisplay display;
  GfxRenderer renderer(display);
  MappedInputManager mapped(input, renderer);
  SETTINGS.sideButtonLayout = CrossPointSettings::PREV_NEXT;
  SETTINGS.frontButtonConfirm = HalGPIO::BTN_CONFIRM;
  ASSERT_TRUE(startSampler(input));
  group2 = 5;
  sample();
  EXPECT_TRUE(input.isDebouncePending());
  EXPECT_FALSE(input.wasAnyPressed());
  for (int i = 0; i < 79; ++i) sample();
  mapped.update();  // Retained press, then latest held state.
  EXPECT_FALSE(mapped.wasLongPressed(MappedInputManager::Button::PageForward, 600));
  mapped.update();
  EXPECT_TRUE(mapped.wasLongPressed(MappedInputManager::Button::PageForward, 600));
  EXPECT_FALSE(mapped.wasLongPressed(MappedInputManager::Button::Down, 600));
  group1 = 2694;
  for (int i = 0; i < 4; ++i) sample();
  mapped.update();
  group1 = group2 = 4095;
  sample();
  sample();
  mapped.update();
  EXPECT_FALSE(mapped.wasReleased(MappedInputManager::Button::PageForward));
  EXPECT_TRUE(mapped.wasReleased(MappedInputManager::Button::Confirm));
  EXPECT_EQ(input.getButtonHeldTime(HalGPIO::BTN_CONFIRM), 40u);

  group2 = 5;
  sample();
  sample();
  mapped.discardPendingInput();
  EXPECT_FALSE(input.hasPendingButtonFrames());
  EXPECT_FALSE(mapped.isPressed(MappedInputManager::Button::PageForward));
  group1 = 2694;
  for (int i = 0; i < 4; ++i) sample();
  group1 = 4095;
  sample();
  sample();
  mapped.update();
  EXPECT_TRUE(mapped.wasPressed(MappedInputManager::Button::Confirm));
  mapped.update();
  EXPECT_TRUE(mapped.wasReleased(MappedInputManager::Button::Confirm));
  EXPECT_FALSE(mapped.isPressed(MappedInputManager::Button::PageForward));
}

TEST(InputFramesTest, BatteryAdcGuardPreventsSamplerReadsUntilTheSharedAdcIsReleased) {
  HalGPIO input;
  ASSERT_TRUE(startSampler(input));
  const auto readsBefore = adcReads.load();
  std::future<void> pendingSample;
  {
    const HalGPIO::AdcLock batteryRead(input);
    unsigned priorContention;
    {
      std::lock_guard lock(inputTest::contentionMutex);
      priorContention = inputTest::contendedTakes;
    }
    nowMs += 10;
    pendingSample = std::async(std::launch::async, inputTest::nextSample);
    {
      std::unique_lock lock(inputTest::contentionMutex);
      inputTest::contentionChanged.wait(lock,
                                        [priorContention] { return inputTest::contendedTakes > priorContention; });
    }
    EXPECT_EQ(adcReads.load(), readsBefore);
  }
  pendingSample.get();
  EXPECT_EQ(adcReads.load() - readsBefore, 2u);
}
