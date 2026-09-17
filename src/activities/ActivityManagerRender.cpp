#include <HalDisplay.h>
#include <HalPowerManager.h>

#include "Activity.h"
#include "ActivityManager.h"
#include "CrossPointSettings.h"

static portMUX_TYPE activityManagerSpinlock = portMUX_INITIALIZER_UNLOCKED;

void ActivityManager::begin() {
#if defined(configNUM_CORES) && configNUM_CORES > 1
  constexpr BaseType_t renderTaskCore = 1;
#else
  constexpr BaseType_t renderTaskCore = 0;
#endif
  xTaskCreatePinnedToCore(&renderTaskTrampoline, "ActivityManagerRender",
                          8192,               // Stack size
                          this,               // Parameters
                          1,                  // Priority
                          &renderTaskHandle,  // Task handle
                          renderTaskCore  // Keep long renders/cover decodes off CPU 0's idle watchdog when available
  );
  assert(renderTaskHandle != nullptr && "Failed to create render task");
}

void ActivityManager::renderTaskTrampoline(void* param) {
  auto* self = static_cast<ActivityManager*>(param);
  self->renderTaskLoop();
}

void ActivityManager::renderTaskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // Acquire the lock before reading currentActivity to avoid a TOCTOU race
    // where the main task deletes the activity between the null-check and render().
    RenderLock lock;
    TaskHandle_t waiter = nullptr;
    Activity* cleanupTarget = nullptr;
    taskENTER_CRITICAL(&activityManagerSpinlock);
    waiter = waitingTaskHandle;
    cleanupTarget = renderCleanupTarget;
    renderCleanupTarget = nullptr;
    taskEXIT_CRITICAL(&activityManagerSpinlock);
    if (cleanupTarget || currentActivity) {
      HalPowerManager::Lock powerLock;  // Ensure we don't go into low-power mode while rendering
      if (cleanupTarget) {
        cleanupTarget->onRenderExit();
      } else {
        // Night mode is a global output polarity applied to every activity.
        // The sleep screen forces normal polarity itself (SleepActivity).
        display.setInverted(SETTINGS.screenInverted != 0);
        currentActivity->render(std::move(lock));
      }
    }
    // A waiter arriving during this work belongs to the next notification.
    if (waiter) {
      taskENTER_CRITICAL(&activityManagerSpinlock);
      waitingTaskHandle = nullptr;
      taskEXIT_CRITICAL(&activityManagerSpinlock);
      xTaskNotify(waiter, 1, eIncrement);
    }
  }
}

void ActivityManager::replaceActivity(std::unique_ptr<Activity>&& newActivity) {
  // Note: no lock here, this is usually called by loop() and we may run into deadlock
  if (currentActivity) {
    // Defer launch if we're currently in an activity, to avoid deleting the current activity
    // leading to the "delete this" problem
    pendingActivity = std::move(newActivity);
    pendingAction = PendingAction::Replace;
  } else {
    // No current activity, safe to launch immediately
    mappedInput.discardPendingInput();
    currentActivity = std::move(newActivity);
    currentActivity->onEnter();
  }
}

void ActivityManager::requestUpdate(bool immediate) {
  if (immediate) {
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  } else {
    // Deferring the update until current loop is finished
    // This is to avoid multiple updates being requested in the same loop
    requestedUpdate = true;
  }
}
bool ActivityManager::requestUpdateAndWait() {
  if (!renderTaskHandle) {
    return false;
  }

  // Atomic section to perform checks
  taskENTER_CRITICAL(&activityManagerSpinlock);
  auto currTaskHandler = xTaskGetCurrentTaskHandle();
  auto mutexHolder = xSemaphoreGetMutexHolder(renderingMutex);
  bool isRenderTask = (currTaskHandler == renderTaskHandle);
  bool alreadyWaiting = (waitingTaskHandle != nullptr);
  bool holdingRenderLock = (mutexHolder == currTaskHandler);
  if (!alreadyWaiting && !isRenderTask && !holdingRenderLock) {
    waitingTaskHandle = currTaskHandler;
  }
  taskEXIT_CRITICAL(&activityManagerSpinlock);

  // Render task cannot call requestUpdateAndWait() or it will cause a deadlock
  assert(!isRenderTask && "Render task cannot call requestUpdateAndWait()");

  // There should never be the case where 2 tasks are waiting for a render at the same time
  assert(!alreadyWaiting && "Already waiting for a render to complete");

  // Cannot call while holding RenderLock or it will cause a deadlock
  assert(!holdingRenderLock && "Cannot call requestUpdateAndWait() while holding RenderLock");

  xTaskNotify(renderTaskHandle, 1, eIncrement);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  return true;
}

bool ActivityManager::retireActivity(Activity& activity) {
  if (!renderTaskHandle) {
    LOG_ERR("ACT", "Cannot retire render resources without the render task");
    return false;
  }
  // Main retains the activity, including a stacked reader, through the acknowledgement.
  taskENTER_CRITICAL(&activityManagerSpinlock);
  assert(renderCleanupTarget == nullptr && "Render cleanup already pending");
  renderCleanupTarget = &activity;
  taskEXIT_CRITICAL(&activityManagerSpinlock);
  requestUpdateAndWait();
  return true;
}

// RenderLock

RenderLock::RenderLock() {
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::RenderLock([[maybe_unused]] Activity&) {
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::~RenderLock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

void RenderLock::unlock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

/**
 *
 * Checks if renderingMutex is busy.
 *
 * @return true if renderingMutex is busy, otherwise false.
 *
 */
bool RenderLock::peek() { return xQueuePeek(activityManager.renderingMutex, NULL, 0) != pdTRUE; };
