#pragma once

#include <FontCacheManager.h>
#include <GfxRenderer.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

namespace xtc {

constexpr size_t MONO_STREAM_ROWS = 16;

// XTH keeps both planes because grayscale rendering visits them repeatedly.
// Release rebuildable font data only after failure, and retry exactly once.
inline std::unique_ptr<uint8_t[]> allocateGrayscalePage(const size_t bytes, FontCacheManager* caches) {
  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[bytes]);
  if (!buffer) {
    if (caches) caches->releaseSdFontCaches();
    buffer.reset(new (std::nothrow) uint8_t[bytes]);
  }
  return buffer;
}

// Streaming chunks contain complete, row-padded XTG rows, including a shorter
// final band. Logical coordinates keep orientation/clipping in GfxRenderer.
inline void drawMonochromeBand(const uint8_t* data, const size_t size, const size_t offset, const uint16_t width,
                               const GfxRenderer& renderer) {
  const size_t rowBytes = (static_cast<size_t>(width) + 7) / 8;
  if (!data || rowBytes == 0 || offset % rowBytes != 0 || size % rowBytes != 0) return;
  for (size_t row = 0; row < size / rowBytes; ++row) {
    for (uint16_t x = 0; x < width; ++x) {
      if ((data[row * rowBytes + x / 8] & (0x80 >> (x % 8))) == 0) {
        renderer.drawPixel(x, static_cast<uint16_t>(offset / rowBytes + row), true);
      }
    }
  }
}

}  // namespace xtc
