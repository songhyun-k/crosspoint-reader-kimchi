#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
using BaseType_t = int;
using UBaseType_t = unsigned;
using TickType_t = uint32_t;
using StackType_t = uint8_t;
constexpr int pdTRUE = 1;
constexpr int pdFALSE = 0;
constexpr int pdPASS = 1;
constexpr unsigned portMAX_DELAY = UINT32_MAX;
#define pdMS_TO_TICKS(ms) (ms)

#define portBYTE_ALIGNMENT 16
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED \
  {                                  \
  }
inline void taskENTER_CRITICAL(portMUX_TYPE* mux) { mux->lock(); }
inline void taskEXIT_CRITICAL(portMUX_TYPE* mux) { mux->unlock(); }
