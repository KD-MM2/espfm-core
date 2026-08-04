#pragma once
#include "FreeRTOS.h"

/* Minimal FreeRTOS semaphore stub for host-based unit tests. */

typedef void *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t xMutex, TickType_t xBlockTime);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t xMutex);
