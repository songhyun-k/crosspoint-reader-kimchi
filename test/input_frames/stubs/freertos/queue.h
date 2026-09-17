#pragma once
#include <cstring>
#include <mutex>

#include "FreeRTOS.h"
struct StaticQueue_t {
  std::mutex mutex;
  uint8_t* bytes = nullptr;
  unsigned capacity = 0, itemSize = 0, head = 0, count = 0, highWater = 0;
};
using QueueHandle_t = StaticQueue_t*;
namespace inputTest {
inline bool failQueueCreate = false;
inline QueueHandle_t queue = nullptr;
}  // namespace inputTest
inline QueueHandle_t xQueueCreate(unsigned, unsigned) { return nullptr; }
inline QueueHandle_t xQueueCreateStatic(unsigned capacity, unsigned size, uint8_t* bytes, StaticQueue_t* queue) {
  if (inputTest::failQueueCreate) return nullptr;
  queue->bytes = bytes;
  queue->capacity = capacity;
  queue->itemSize = size;
  queue->head = queue->count = queue->highWater = 0;
  inputTest::queue = queue;
  return queue;
}
inline int xQueueSend(QueueHandle_t queue, const void* item, unsigned) {
  std::lock_guard lock(queue->mutex);
  if (queue->count == queue->capacity) return pdFALSE;
  std::memcpy(queue->bytes + ((queue->head + queue->count) % queue->capacity) * queue->itemSize, item, queue->itemSize);
  ++queue->count;
  if (queue->count > queue->highWater) queue->highWater = queue->count;
  return pdTRUE;
}
inline int xQueueReceive(QueueHandle_t queue, void* item, unsigned) {
  std::lock_guard lock(queue->mutex);
  if (!queue->count) return pdFALSE;
  std::memcpy(item, queue->bytes + queue->head * queue->itemSize, queue->itemSize);
  queue->head = (queue->head + 1) % queue->capacity;
  --queue->count;
  return pdTRUE;
}
inline int xQueuePeek(QueueHandle_t queue, void* item, unsigned) {
  std::lock_guard lock(queue->mutex);
  if (!queue->count) return pdFALSE;
  std::memcpy(item, queue->bytes + queue->head * queue->itemSize, queue->itemSize);
  return pdTRUE;
}
inline unsigned uxQueueMessagesWaiting(QueueHandle_t queue) {
  std::lock_guard lock(queue->mutex);
  return queue->count;
}
inline void xQueueReset(QueueHandle_t queue) {
  std::lock_guard lock(queue->mutex);
  queue->head = queue->count = 0;
}
inline void vQueueDelete(QueueHandle_t queue) {
  if (inputTest::queue == queue) inputTest::queue = nullptr;
}
