#pragma once
#include <condition_variable>
#include <mutex>

#include "FreeRTOS.h"
struct StaticSemaphore_t {
  std::mutex mutex;
};
using SemaphoreHandle_t = StaticSemaphore_t*;
SemaphoreHandle_t xSemaphoreCreateMutex();
namespace inputTest {
inline std::mutex contentionMutex;
inline std::condition_variable contentionChanged;
inline unsigned contendedTakes = 0;
}  // namespace inputTest
inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) { return storage; }
inline int xSemaphoreTake(SemaphoreHandle_t handle, TickType_t) {
  if (!handle->mutex.try_lock()) {
    {
      std::lock_guard lock(inputTest::contentionMutex);
      ++inputTest::contendedTakes;
      inputTest::contentionChanged.notify_all();
    }
    handle->mutex.lock();
  }
  return pdTRUE;
}
inline int xSemaphoreGive(SemaphoreHandle_t handle) {
  handle->mutex.unlock();
  return pdTRUE;
}
inline void vSemaphoreDelete(SemaphoreHandle_t) {}
