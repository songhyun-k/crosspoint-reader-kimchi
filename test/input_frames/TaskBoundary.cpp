#include <freertos/task.h>

namespace {
struct DeletedTask {};
}  // namespace

namespace inputTest {
thread_local TaskHandle_t currentTask = nullptr;
TaskHandle_t task = nullptr;
bool failTaskCreate = false;
void waitForSample() {
  const auto handle = task;
  std::unique_lock lock(handle->mutex);
  handle->condition.wait(lock, [handle] { return handle->sleeping; });
}
void nextSample() {
  const auto handle = task;
  std::unique_lock lock(handle->mutex);
  const auto before = handle->waits;
  ++handle->permits;
  handle->condition.notify_all();
  handle->condition.wait(lock, [handle, before] { return handle->waits > before; });
}
}  // namespace inputTest

TaskHandle_t xTaskCreateStatic(void (*entry)(void*), const char*, unsigned, void* context, unsigned, StackType_t*,
                               StaticTask_t* storage) {
  if (inputTest::failTaskCreate) return nullptr;
  storage->waits = storage->permits = 0;
  storage->sleeping = storage->notified = storage->deleted = false;
  inputTest::task = storage;
  storage->thread = std::thread([entry, context, storage] {
    inputTest::currentTask = storage;
    try {
      entry(context);
    } catch (const DeletedTask&) {
    }
    inputTest::currentTask = nullptr;
  });
  return storage;
}
int xTaskCreatePinnedToCore(void (*entry)(void*), const char* name, unsigned stack, void* context, unsigned priority,
                            TaskHandle_t* handle, int) {
  auto* storage = new StaticTask_t;
  *handle = xTaskCreateStatic(entry, name, stack, context, priority, nullptr, storage);
  if (!*handle) {
    delete storage;
    return pdFALSE;
  }
  storage->dynamicStorage = true;
  return pdPASS;
}
TaskHandle_t xTaskGetCurrentTaskHandle() {
  thread_local StaticTask_t externalTask;
  return inputTest::currentTask ? inputTest::currentTask : &externalTask;
}
uint32_t ulTaskNotifyTake(int, TickType_t) {
  auto* task = xTaskGetCurrentTaskHandle();
  std::unique_lock lock(task->mutex);
  task->sleeping = true;
  ++task->waits;
  task->condition.notify_all();
  task->condition.wait(lock, [task] { return task->permits || task->notified || task->deleted; });
  if (task->deleted) throw DeletedTask{};
  task->sleeping = false;
  if (task->permits) --task->permits;
  const auto result = task->notified;
  task->notified = false;
  return result;
}
void xTaskNotifyGive(TaskHandle_t task) {
  std::lock_guard lock(task->mutex);
  task->notified = true;
  ++task->notifications;
  task->condition.notify_all();
}
void vTaskSuspend(TaskHandle_t) {
  auto* task = inputTest::currentTask;
  std::unique_lock lock(task->mutex);
  task->condition.wait(lock, [task] { return task->deleted; });
}
void vTaskDelete(TaskHandle_t task) {
  {
    std::lock_guard lock(task->mutex);
    task->deleted = true;
    task->condition.notify_all();
  }
  task->thread.join();
  inputTest::task = nullptr;
  if (task->dynamicStorage) delete task;
}
