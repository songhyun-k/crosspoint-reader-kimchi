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
  bool factorySupported = true;
  std::array<uint8_t, BUFFER_SIZE> lsb{}, msb{}, baseline{};
  int factoryActivations = 0, bwActivations = 0, grayActivations = 0;
  size_t planeBytesWritten = 0, cleanupBytesWritten = 0;
  RefreshMode lastRefresh = FAST_REFRESH;
  uint16_t width = DISPLAY_WIDTH, height = DISPLAY_HEIGHT, stride = DISPLAY_WIDTH_BYTES;

  uint8_t* getFrameBuffer() const { return pixels.data(); }
  uint16_t getDisplayWidth() const { return width; }
  uint16_t getDisplayHeight() const { return height; }
  uint16_t getDisplayWidthBytes() const { return stride; }
  uint32_t getBufferSize() const { return static_cast<uint32_t>(stride) * height; }
  void clearScreen(uint8_t color = 0xFF) const { std::memset(pixels.data(), color, getBufferSize()); }
  bool isInverted() const { return inverted; }
  void drawImage(const uint8_t*, uint16_t, uint16_t, uint16_t, uint16_t, bool = false) const {}
  void displayBuffer(RefreshMode mode = FAST_REFRESH, bool = false) {
    ++bwActivations;
    lastRefresh = mode;
  }
  void displayBufferAsync(RefreshMode mode = FAST_REFRESH) { displayBuffer(mode); }
  void waitRefreshComplete() {}
  bool supportsAsyncRefresh() const { return false; }
  uint8_t* lendFrameBufferStorage(uint32_t* size) {
    *size = getBufferSize();
    return pixels.data();
  }
  void returnFrameBufferStorage() {}
  void displayGrayscaleBase(RefreshMode mode = HALF_REFRESH, bool = false) { displayBuffer(mode); }
  void preconditionGrayscale() {}
  void preconditionGrayscale(uint16_t, uint16_t, uint16_t, uint16_t) {}
  void copyGrayscaleLsbBuffers(const uint8_t*) {}
  void copyGrayscaleMsbBuffers(const uint8_t*) {}
  void displayGrayBuffer(bool = false) { ++grayActivations; }
  bool supportsFactoryGrayscale() const { return factorySupported && !inverted; }
  void displayFactoryGrayscale(bool = false) {
    ++factoryActivations;
    ++grayActivations;
  }
  void cleanupGrayscaleBuffers(const uint8_t* bw) {
    std::memcpy(baseline.data(), bw, getBufferSize());
    cleanupBytesWritten += getBufferSize();
  }
  void writeGrayscalePlaneStrip(bool low, const uint8_t* rows, uint16_t y, uint16_t count) {
    const size_t n = count * stride;
    std::memcpy((low ? lsb : msb).data() + y * stride, rows, n);
    planeBytesWritten += n;
  }
  bool supportsStripGrayscale() const { return true; }
  bool combinesGrayscaleBase() const { return false; }
};
