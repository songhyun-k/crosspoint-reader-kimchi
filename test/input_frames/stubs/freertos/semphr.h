#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>

#include "FreeRTOS.h"
#include "task.h"
struct StaticSemaphore_t {
  std::mutex mutex;
  std::atomic<TaskHandle_t> holder{nullptr};
};
using SemaphoreHandle_t = StaticSemaphore_t*;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new StaticSemaphore_t; }
inline TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t handle) { return handle->holder.load(); }
inline int xQueuePeek(SemaphoreHandle_t handle, void*, unsigned) {
  if (!handle->mutex.try_lock()) return pdFALSE;
  handle->mutex.unlock();
  return pdTRUE;
}
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
  handle->holder = xTaskGetCurrentTaskHandle();
  return pdTRUE;
}
inline int xSemaphoreGive(SemaphoreHandle_t handle) {
  handle->holder = nullptr;
  handle->mutex.unlock();
  return pdTRUE;
}
inline void vSemaphoreDelete(SemaphoreHandle_t) {}
