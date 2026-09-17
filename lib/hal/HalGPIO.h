#pragma once

#include <Arduino.h>
#include <InputManager.h>
#include <freertos/semphr.h>

#include <atomic>

// Display SPI pins (custom pins for XteinkX4, not hardware SPI defaults)
#define EPD_SCLK 8   // SPI Clock
#define EPD_MOSI 10  // SPI MOSI (Master Out Slave In)
#define EPD_CS 21    // Chip Select
#define EPD_DC 4     // Data/Command
#define EPD_RST 5    // Reset
#define EPD_BUSY 6   // Busy

#define SPI_MISO 7  // SPI MISO, shared between SD card and display (Master In Slave Out)

#define BAT_GPIO0 0  // Battery voltage

#define UART0_RXD 20  // Used for USB connection detection

// Xteink X3 Hardware
#define X3_I2C_SDA 20
#define X3_I2C_SCL 0
#define X3_I2C_FREQ 400000

// TI BQ27220 Fuel gauge I2C
#define I2C_ADDR_BQ27220 0x55  // Fuel gauge I2C address
#define BQ27220_SOC_REG 0x2C   // StateOfCharge() command code (%)
#define BQ27220_CUR_REG 0x0C   // Current() command code (signed mA)
#define BQ27220_VOLT_REG 0x08  // Voltage() command code (mV)

// Analog DS3231 RTC I2C
#define I2C_ADDR_DS3231 0x68  // RTC I2C address
#define DS3231_SEC_REG 0x00   // Seconds command code (BCD)

// QST QMI8658 IMU I2C
#define I2C_ADDR_QMI8658 0x6B        // IMU I2C address
#define I2C_ADDR_QMI8658_ALT 0x6A    // IMU I2C fallback address
#define QMI8658_WHO_AM_I_REG 0x00    // WHO_AM_I command code
#define QMI8658_WHO_AM_I_VALUE 0x05  // WHO_AM_I expected value

class HalGPIO {
 public:
  static constexpr uint8_t BUTTON_COUNT = 7;
  // One observation, owned by the producer until published, then by the main UI.
  // Durations stop at capture time: time spent waiting for the UI is not a hold.
  struct ButtonFrame {
    uint32_t sequence = 0;
    uint32_t capturedAtMs = 0;
    uint32_t droppedFrames = 0;
    uint32_t buttonHeldMs[BUTTON_COUNT] = {};
    uint32_t heldMs = 0;
    uint32_t powerHeldMs = 0;
    uint8_t down = 0;
    uint8_t pressed = 0;
    uint8_t released = 0;
    bool debouncePending = false;
  };

 private:
  ButtonFrame buttonFrame;
  uint8_t suppressedContacts = 0;
  bool waitForIdle = false;
  uint32_t reportedDroppedFrames = 0;
  void captureButtonFrame(ButtonFrame& frame);
  void reconcileButtonInput();
#if FREEINK_MCU_C3
  enum class SamplerState : uint8_t { Stopped, Running, StopRequested, Quiescent };
  std::atomic<SamplerState> samplerState{SamplerState::Stopped};
  // Only this mutex protects the physical snapshot and FIFO. Never hold it
  // across activity, renderer, SD or USB/battery I2C calls.
  StaticSemaphore_t inputMutexStorage{};
  SemaphoreHandle_t inputMutex = nullptr;
  ButtonFrame physicalFrame;
  StaticQueue_t frameQueueControl{};
  QueueHandle_t frameQueue = nullptr;
  // 40ms press/release: 26 transitions per 1s consumer stall, plus phase margin.
  static constexpr uint8_t INPUT_FRAME_CAPACITY = 32;
  alignas(ButtonFrame) uint8_t frameQueueStorage[INPUT_FRAME_CAPACITY * sizeof(ButtonFrame)]{};
  static constexpr uint32_t INPUT_STACK_BYTES = 2048;
  static constexpr uint32_t INPUT_POLL_MS = 10;
  alignas(portBYTE_ALIGNMENT) StackType_t inputStack[INPUT_STACK_BYTES / sizeof(StackType_t)]{};
  StaticTask_t inputTaskControl{};
  TaskHandle_t inputTask = nullptr;
  static void inputTaskTrampoline(void* context);
  void sampleButtons();
#endif
#if CROSSPOINT_EMULATED == 0
  InputManager inputMgr;
#endif

  bool lastUsbConnected = false;
  bool usbStateChanged = false;

 public:
  enum class DeviceType : uint8_t { X4, X3 };

 private:
  DeviceType _deviceType = DeviceType::X4;

 public:
#if FREEINK_MCU_C3
  // Arduino's ADC wrapper discards adc_oneshot_read contention errors. The
  // sampler and X4 battery ADC must share this guard; never wrap I2C or UI work.
  class AdcLock {
    SemaphoreHandle_t mutex;

   public:
    explicit AdcLock(const HalGPIO& owner) : mutex(owner.inputMutex) {
      if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);
    }
    ~AdcLock() {
      if (mutex) xSemaphoreGive(mutex);
    }
    AdcLock(const AdcLock&) = delete;
    AdcLock& operator=(const AdcLock&) = delete;
  };
#endif

  HalGPIO() = default;
  ~HalGPIO();

  // Inline device type helpers for cleaner downstream checks
  inline bool deviceIsX3() const { return _deviceType == DeviceType::X3; }
  inline bool deviceIsX4() const { return _deviceType == DeviceType::X4; }
  bool isXteinkDevice() const;

  // True when the board's page buttons sit on the left/right screen edges
  // (X3, X4 Pro) rather than an off-screen vertical rocker. Drives side-hint
  // placement and the flipped large-step direction in selection activities.
  // Keyed off the active BoardConfig profile, not the X3/X4 runtime detection.
  bool hasEdgeSideButtons() const;

  // Start button GPIO and setup SPI for screen and SD card
  void begin();

  // Start after wake verification. C3 SDK state belongs to the sampler until
  // stopButtonSampling() has acknowledged its last use and deleted the task.
  bool startButtonSampling();
  void stopButtonSampling();
  // Main-thread consumption: one retained frame, or the latest idle/held sample.
  void update();
  bool hasPendingButtonFrames() const;
  ButtonFrame getPhysicalButtonFrame() const;
  // Cancel old-context frames and mute incomplete contacts through release.
  void discardButtonInput();
  // Latest physical raw change awaiting debounce, independent of the replayed UI frame.
  bool isDebouncePending() const;
  bool isPressed(uint8_t buttonIndex) const;
  bool wasPressed(uint8_t buttonIndex) const;
  bool wasAnyPressed() const;
  bool wasReleased(uint8_t buttonIndex) const;
  bool wasAnyReleased() const;
  // Queries never advance the frame; update() is the consumption boundary.
  const ButtonFrame& getButtonFrame() const { return buttonFrame; }
  unsigned long getButtonHeldTime(uint8_t buttonIndex) const;
  void suppressButtonReleases(uint8_t buttons);
  unsigned long getHeldTime() const;
  unsigned long getPowerButtonHeldTime() const;
  bool hasTouch() const;
  // Capacitive Home key reported by the touch controller (X4 Pro). The tap
  // event fires on release and excludes a long hold.
  bool hasHomeKey() const;
  bool wasHomeKeyTapped() const;
  bool wasHomeKeyLongPressed() const;
  bool wasTouchTap(float& nx, float& ny) const;
  bool wasTouchDown(float& nx, float& ny) const;
  // Raw release edge, reported even when the contact was not a tap (swipe end,
  // drag-off). Snapshot builders forward it so interaction routing can clear
  // pressed state.
  bool wasTouchReleased() const;
  bool isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const;
  bool isTouchHeldAt(float& nx, float& ny) const;
  // One-shot long-press, fired by the SDK classifier while the finger is still
  // down (stationary contact held past its threshold). Position = touch-down
  // point. Callers that act on it should suppressTouchContact() so the lift
  // cannot also tap.
  bool wasTouchLongPress(float& nx, float& ny) const;
  // Ignore the remainder of the current contact (its continued hold and its
  // release edge). Self-clears once the contact ends.
  void suppressTouchContact();
  unsigned long lastTouchHeldMs() const;
  bool wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const;
  bool wasTouchActivity() const;
  void setSharedConfirmPowerShortPressEmitsPower(bool enabled);

  // Verify that the physical power button remains held through input debounce.
  // Returns true if verification succeeded, false if device should return to sleep.
  // Should only be called when wakeup reason is PowerButton.
  bool verifyPowerButtonWakeup();

  // Check if USB is connected
  bool isUsbConnected() const;

  // Whether a cold boot with no USB detected can be trusted to mean a held
  // power button (Xteink-style button-energized rail with reliable USB
  // detection). When false, cold boots always proceed to a normal boot.
  bool coldBootImpliesPowerButton() const;

  // Returns true once per edge (plug or unplug) since the last update()
  bool wasUsbStateChanged() const;

  enum class WakeupReason { PowerButton, AfterFlash, AfterUSBPower, Other };

  WakeupReason getWakeupReason() const;

  // Button indices
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;
};

extern HalGPIO gpio;
