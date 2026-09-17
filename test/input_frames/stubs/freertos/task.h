#pragma once
#include "FreeRTOS.h"
using TaskHandle_t = void*;
inline int xTaskCreate(void (*)(void*), const char*, unsigned, void*, unsigned, TaskHandle_t*) { return pdFALSE; }
inline void vTaskDelay(TickType_t) {}
