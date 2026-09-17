#include "HalGPIO.h"

void HalGPIO::captureButtonFrame() {
  const uint32_t capturedAtMs = millis();
  const uint32_t elapsedMs = capturedAtMs - buttonFrame.capturedAtMs;
  uint8_t down = 0;
  buttonFrame.pressed = 0;
  buttonFrame.released = 0;
  for (uint8_t button = 0; button < BUTTON_COUNT; ++button) {
    const uint8_t bit = 1u << button;
    if (inputMgr.wasPressed(button)) {
      buttonFrame.pressed |= bit;
      buttonFrame.buttonHeldMs[button] = 0;
    } else if (buttonFrame.down & bit) {
      buttonFrame.buttonHeldMs[button] += elapsedMs;
    }
    if (inputMgr.wasReleased(button)) buttonFrame.released |= bit;
    if (inputMgr.isPressed(button)) down |= bit;
  }
  buttonFrame.down = down;
  buttonFrame.capturedAtMs = capturedAtMs;
  ++buttonFrame.sequence;
  buttonFrame.heldMs = inputMgr.getHeldTime();
  buttonFrame.powerHeldMs = inputMgr.getPowerButtonHeldTime();
  buttonFrame.debouncePending = inputMgr.isDebouncePending();
}

unsigned long HalGPIO::getButtonHeldTime(uint8_t buttonIndex) const {
  return buttonIndex < BUTTON_COUNT ? buttonFrame.buttonHeldMs[buttonIndex] : 0;
}

void HalGPIO::suppressButtonReleases(uint8_t buttons) { buttonFrame.released &= ~buttons; }

void HalGPIO::update() {
  inputMgr.update();
  captureButtonFrame();
  const bool connected = isUsbConnected();
  usbStateChanged = (connected != lastUsbConnected);
  lastUsbConnected = connected;
}

bool HalGPIO::isDebouncePending() const { return buttonFrame.debouncePending; }

bool HalGPIO::wasUsbStateChanged() const { return usbStateChanged; }

bool HalGPIO::isPressed(uint8_t buttonIndex) const {
  return buttonIndex < BUTTON_COUNT && (buttonFrame.down & (1u << buttonIndex));
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

bool HalGPIO::hasTouch() const { return inputMgr.hasTouch(); }

bool HalGPIO::hasHomeKey() const { return BoardConfig::hasHomeKey(); }

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

void HalGPIO::setSharedConfirmPowerShortPressEmitsPower(const bool enabled) {
  InputManager::setSharedConfirmPowerShortPressEmitsPower(enabled);
}
