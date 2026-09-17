#include <Logging.h>

#include "HalGPIO.h"

HalGPIO::~HalGPIO() {
  stopButtonSampling();
#if FREEINK_MCU_C3
  if (frameQueue) vQueueDelete(frameQueue);
  if (inputMutex) vSemaphoreDelete(inputMutex);
#endif
}

void HalGPIO::captureButtonFrame(ButtonFrame& frame) {
  const uint32_t capturedAtMs = millis();
  const uint32_t elapsedMs = capturedAtMs - frame.capturedAtMs;
  uint8_t down = 0;
  frame.pressed = 0;
  frame.released = 0;
  for (uint8_t button = 0; button < BUTTON_COUNT; ++button) {
    const uint8_t bit = 1u << button;
    if (inputMgr.wasPressed(button)) {
      frame.pressed |= bit;
      frame.buttonHeldMs[button] = 0;
    } else if (frame.down & bit) {
      frame.buttonHeldMs[button] += elapsedMs;
    }
    if (inputMgr.wasReleased(button)) frame.released |= bit;
    if (inputMgr.isPressed(button)) down |= bit;
  }
  frame.down = down;
  frame.capturedAtMs = capturedAtMs;
  ++frame.sequence;
  frame.heldMs = inputMgr.getHeldTime();
  frame.powerHeldMs = inputMgr.getPowerButtonHeldTime();
  frame.debouncePending = inputMgr.isDebouncePending();
}

unsigned long HalGPIO::getButtonHeldTime(uint8_t buttonIndex) const {
  return buttonIndex < BUTTON_COUNT ? buttonFrame.buttonHeldMs[buttonIndex] : 0;
}

void HalGPIO::suppressButtonReleases(uint8_t buttons) { buttonFrame.released &= ~buttons; }

bool HalGPIO::startButtonSampling() {
#if FREEINK_MCU_C3
  if (inputTask) return true;
  if (!inputMutex) inputMutex = xSemaphoreCreateMutexStatic(&inputMutexStorage);
  if (!frameQueue) {
    frameQueue = xQueueCreateStatic(INPUT_FRAME_CAPACITY, sizeof(ButtonFrame), frameQueueStorage, &frameQueueControl);
  }
  if (!inputMutex || !frameQueue) {
    LOG_ERR("GPIO", "Cannot initialize button transport");
    return false;
  }
  // The main thread is still the only SDK input owner here.
  {
    const AdcLock lock(*this);
    inputMgr.update();
    captureButtonFrame(buttonFrame);
    physicalFrame = buttonFrame;
    xQueueReset(frameQueue);
  }
  samplerState.store(SamplerState::Running);
  inputTask =
      xTaskCreateStatic(inputTaskTrampoline, "Buttons", INPUT_STACK_BYTES, this, 2, inputStack, &inputTaskControl);
  if (!inputTask) {
    samplerState.store(SamplerState::Stopped);
    LOG_ERR("GPIO", "Cannot start button sampler");
    return false;
  }
#endif
  return true;
}

void HalGPIO::stopButtonSampling() {
#if FREEINK_MCU_C3
  if (!inputTask) return;
  samplerState.store(SamplerState::StopRequested);
  xTaskNotifyGive(inputTask);
  while (samplerState.load() != SamplerState::Quiescent) vTaskDelay(1);
  vTaskDelete(inputTask);
  inputTask = nullptr;
  samplerState.store(SamplerState::Stopped);
  discardButtonInput();
#endif
}

#if FREEINK_MCU_C3
void HalGPIO::inputTaskTrampoline(void* context) { static_cast<HalGPIO*>(context)->sampleButtons(); }

void HalGPIO::sampleButtons() {
  while (samplerState.load() == SamplerState::Running) {
    // Sampling and publication share the discard boundary: a sample from the
    // old context cannot arrive in the FIFO after that context was cancelled.
    {
      const AdcLock lock(*this);
      inputMgr.update();
      captureButtonFrame(physicalFrame);
      if ((physicalFrame.pressed || physicalFrame.released) && xQueueSend(frameQueue, &physicalFrame, 0) != pdTRUE) {
        ++physicalFrame.droppedFrames;
      }
    }
    // Timed sleep also lets shutdown wake the sampler without waiting a poll.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(INPUT_POLL_MS));
  }
  // No SDK, mutex or queue use after this acknowledgement. The owner deletes
  // the parked task before reusing SDK state or releasing this object.
  samplerState.store(SamplerState::Quiescent);
  vTaskSuspend(nullptr);
}
#endif

HalGPIO::ButtonFrame HalGPIO::getPhysicalButtonFrame() const {
#if FREEINK_MCU_C3
  if (inputMutex && frameQueue) {
    xSemaphoreTake(inputMutex, portMAX_DELAY);
    const ButtonFrame frame = physicalFrame;
    xSemaphoreGive(inputMutex);
    return frame;
  }
#endif
  return buttonFrame;
}

bool HalGPIO::hasPendingButtonFrames() const {
#if FREEINK_MCU_C3
  return frameQueue && uxQueueMessagesWaiting(frameQueue) != 0;
#else
  return false;
#endif
}

void HalGPIO::reconcileButtonInput() {
  if (buttonFrame.droppedFrames != reportedDroppedFrames) {
    LOG_ERR("GPIO", "Button transport lost %lu frames (total %lu)",
            static_cast<unsigned long>(buttonFrame.droppedFrames - reportedDroppedFrames),
            static_cast<unsigned long>(buttonFrame.droppedFrames));
    reportedDroppedFrames = buttonFrame.droppedFrames;
    suppressedContacts |= buttonFrame.down;
    waitForIdle |= buttonFrame.debouncePending;
    buttonFrame.pressed = 0;
    buttonFrame.released = 0;
  }
  if (waitForIdle) {
    suppressedContacts = (1u << BUTTON_COUNT) - 1;
    if (buttonFrame.down == 0 && !buttonFrame.debouncePending) waitForIdle = false;
  }
  buttonFrame.pressed &= ~suppressedContacts;
  buttonFrame.released &= ~suppressedContacts;
  if (!waitForIdle) suppressedContacts &= buttonFrame.down;
}

void HalGPIO::discardButtonInput() {
#if FREEINK_MCU_C3
  if (inputMutex && frameQueue) {
    xSemaphoreTake(inputMutex, portMAX_DELAY);
    const auto discarded = uxQueueMessagesWaiting(frameQueue);
    xQueueReset(frameQueue);
    buttonFrame = physicalFrame;
    xSemaphoreGive(inputMutex);
    if (discarded) LOG_DBG("GPIO", "Cancelled %u old-context button frames", static_cast<unsigned>(discarded));
  }
#endif
  suppressedContacts |= buttonFrame.down;
  waitForIdle |= buttonFrame.debouncePending;
  buttonFrame.pressed = 0;
  buttonFrame.released = 0;
  reconcileButtonInput();
}

void HalGPIO::update() {
#if FREEINK_MCU_C3
  if (inputTask) {
    xSemaphoreTake(inputMutex, portMAX_DELAY);
    if (xQueueReceive(frameQueue, &buttonFrame, 0) != pdTRUE) {
      buttonFrame = physicalFrame;
      buttonFrame.pressed = 0;
      buttonFrame.released = 0;
    }
    xSemaphoreGive(inputMutex);
  } else
#endif
  {
#if FREEINK_MCU_C3
    const AdcLock lock(*this);
#endif
    inputMgr.update();
    captureButtonFrame(buttonFrame);
#if FREEINK_MCU_C3
    if (inputMutex) physicalFrame = buttonFrame;
#endif
  }
  reconcileButtonInput();
  const bool connected = isUsbConnected();
  usbStateChanged = (connected != lastUsbConnected);
  lastUsbConnected = connected;
}

bool HalGPIO::isDebouncePending() const { return getPhysicalButtonFrame().debouncePending; }

bool HalGPIO::wasUsbStateChanged() const { return usbStateChanged; }

bool HalGPIO::isPressed(uint8_t buttonIndex) const {
  return buttonIndex < BUTTON_COUNT && (buttonFrame.down & ~suppressedContacts & (1u << buttonIndex));
}

bool HalGPIO::wasPressed(uint8_t buttonIndex) const {
  return buttonIndex < BUTTON_COUNT && (buttonFrame.pressed & (1u << buttonIndex));
}

bool HalGPIO::wasAnyPressed() const { return buttonFrame.pressed != 0; }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const {
  return buttonIndex < BUTTON_COUNT && (buttonFrame.released & (1u << buttonIndex));
}

bool HalGPIO::wasAnyReleased() const { return buttonFrame.released != 0; }

unsigned long HalGPIO::getHeldTime() const { return buttonFrame.heldMs; }

unsigned long HalGPIO::getPowerButtonHeldTime() const { return buttonFrame.powerHeldMs; }

bool HalGPIO::hasTouch() const { return BoardConfig::hasTouch(); }

bool HalGPIO::hasHomeKey() const { return BoardConfig::hasHomeKey(); }

#if FREEINK_CAP_TOUCH
bool HalGPIO::wasHomeKeyTapped() const { return inputMgr.wasHomeKeyTapped(); }

bool HalGPIO::wasHomeKeyLongPressed() const { return inputMgr.wasHomeKeyLongPressed(); }

bool HalGPIO::wasTouchTap(float& nx, float& ny) const { return inputMgr.wasTouchTap(nx, ny); }

bool HalGPIO::wasTouchDown(float& nx, float& ny) const { return inputMgr.wasTouchPressedAt(nx, ny); }

bool HalGPIO::wasTouchReleased() const { return inputMgr.wasTouchReleased(); }

bool HalGPIO::isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const {
  return inputMgr.isTouchTapCandidate(nx, ny, heldMs);
}

bool HalGPIO::isTouchHeldAt(float& nx, float& ny) const { return inputMgr.isTouchHeldAt(nx, ny); }

bool HalGPIO::wasTouchLongPress(float& nx, float& ny) const { return inputMgr.wasTouchLongPress(nx, ny); }

void HalGPIO::suppressTouchContact() { inputMgr.suppressTouchContact(); }

unsigned long HalGPIO::lastTouchHeldMs() const { return inputMgr.lastTouchHeldMs(); }

bool HalGPIO::wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const {
  return inputMgr.wasSwipe(nxStart, nyStart, nxEnd, nyEnd);
}

bool HalGPIO::wasTouchActivity() const { return inputMgr.wasTouchActivity(); }

#else
bool HalGPIO::wasHomeKeyTapped() const { return false; }
bool HalGPIO::wasHomeKeyLongPressed() const { return false; }
bool HalGPIO::wasTouchTap(float&, float&) const { return false; }
bool HalGPIO::wasTouchDown(float&, float&) const { return false; }
bool HalGPIO::wasTouchReleased() const { return false; }
bool HalGPIO::isTouchTapCandidate(float&, float&, unsigned long&) const { return false; }
bool HalGPIO::isTouchHeldAt(float&, float&) const { return false; }
bool HalGPIO::wasTouchLongPress(float&, float&) const { return false; }
void HalGPIO::suppressTouchContact() {}
unsigned long HalGPIO::lastTouchHeldMs() const { return 0; }
bool HalGPIO::wasSwipe(float&, float&, float&, float&) const { return false; }
bool HalGPIO::wasTouchActivity() const { return false; }
#endif

void HalGPIO::setSharedConfirmPowerShortPressEmitsPower(const bool enabled) {
#if !FREEINK_MCU_C3
  InputManager::setSharedConfirmPowerShortPressEmitsPower(enabled);
#else
  (void)enabled;
#endif
}
