#pragma once
#include "FreeRTOS.h"
using QueueHandle_t = void*;
inline QueueHandle_t xQueueCreate(unsigned, unsigned) { return nullptr; }
inline int xQueueSend(QueueHandle_t, const void*, unsigned) { return pdFALSE; }
inline int xQueueReceive(QueueHandle_t, void*, unsigned) { return pdFALSE; }
inline void xQueueReset(QueueHandle_t) {}
