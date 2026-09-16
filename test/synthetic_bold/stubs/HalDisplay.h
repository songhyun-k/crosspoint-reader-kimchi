#pragma once

#include <Arduino.h>

#include <array>
#include <cstring>

// A memory-only panel: the production renderer still transforms coordinates,
// clips strips and writes every framebuffer bit. No physical I/O occurs.
class HalDisplay {
 public:
  enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH };
  static constexpr uint16_t DISPLAY_WIDTH = 800;
  static constexpr uint16_t DISPLAY_HEIGHT = 480;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;
  mutable std::array<uint8_t, BUFFER_SIZE> pixels{};
  bool inverted = false;
  uint16_t width = DISPLAY_WIDTH, height = DISPLAY_HEIGHT, stride = DISPLAY_WIDTH_BYTES;

  uint8_t* getFrameBuffer() const { return pixels.data(); }
  uint16_t getDisplayWidth() const { return width; }
  uint16_t getDisplayHeight() const { return height; }
  uint16_t getDisplayWidthBytes() const { return stride; }
  uint32_t getBufferSize() const { return static_cast<uint32_t>(stride) * height; }
  void clearScreen(uint8_t color = 0xFF) const { std::memset(pixels.data(), color, getBufferSize()); }
  bool isInverted() const { return inverted; }
  void drawImage(const uint8_t*, uint16_t, uint16_t, uint16_t, uint16_t, bool = false) const {}
  void displayBuffer(RefreshMode = FAST_REFRESH, bool = false) {}
  void displayBufferAsync(RefreshMode = FAST_REFRESH) {}
  void waitRefreshComplete() {}
  bool supportsAsyncRefresh() const { return false; }
  uint8_t* lendFrameBufferStorage(uint32_t* size) {
    *size = getBufferSize();
    return pixels.data();
  }
  void returnFrameBufferStorage() {}
  void displayGrayscaleBase(RefreshMode = HALF_REFRESH, bool = false) {}
  void preconditionGrayscale() {}
  void preconditionGrayscale(uint16_t, uint16_t, uint16_t, uint16_t) {}
  void copyGrayscaleLsbBuffers(const uint8_t*) {}
  void copyGrayscaleMsbBuffers(const uint8_t*) {}
  void displayGrayBuffer(bool = false) {}
  void cleanupGrayscaleBuffers(const uint8_t*) {}
  void writeGrayscalePlaneStrip(bool, const uint8_t*, uint16_t, uint16_t) {}
  bool supportsStripGrayscale() const { return true; }
  bool combinesGrayscaleBase() const { return false; }
};
