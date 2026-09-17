#pragma once
#include <condition_variable>
#include <mutex>
#include <thread>

#include "FreeRTOS.h"
struct StaticTask_t {
  std::thread thread;
  std::mutex mutex;
  std::condition_variable condition;
  unsigned waits = 0, permits = 0;
  unsigned notifications = 0;
  bool sleeping = false, notified = false, deleted = false;
  bool dynamicStorage = false;
};
using TaskHandle_t = StaticTask_t*;
namespace inputTest {
extern thread_local TaskHandle_t currentTask;
extern TaskHandle_t task;
extern bool failTaskCreate;
void waitForSample();
void nextSample();
}  // namespace inputTest
inline int xTaskCreate(void (*)(void*), const char*, unsigned, void*, unsigned, TaskHandle_t*) { return pdFALSE; }
int xTaskCreatePinnedToCore(void (*)(void*), const char*, unsigned, void*, unsigned, TaskHandle_t*, int);
TaskHandle_t xTaskGetCurrentTaskHandle();
enum eNotifyAction { eIncrement };
inline void xTaskNotify(TaskHandle_t task, uint32_t, eNotifyAction);
TaskHandle_t xTaskCreateStatic(void (*entry)(void*), const char*, unsigned, void*, unsigned, StackType_t*,
                               StaticTask_t*);
uint32_t ulTaskNotifyTake(int, TickType_t);
void xTaskNotifyGive(TaskHandle_t);
inline void xTaskNotify(TaskHandle_t task, uint32_t, eNotifyAction) { xTaskNotifyGive(task); }
void vTaskDelete(TaskHandle_t);
void vTaskSuspend(TaskHandle_t);
inline void vTaskDelay(TickType_t) { std::this_thread::yield(); }
