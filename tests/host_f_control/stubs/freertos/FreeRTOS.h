#pragma once
#include <stdint.h>

/* Minimal FreeRTOS kernel stub for host-based unit tests. Only the types and
 * constants the f_control.c task lifecycle references are provided. Task and
 * mutex functions are declared in task.h/semphr.h and defined in the test TU. */

typedef int BaseType_t;
typedef uint32_t TickType_t;

#define portMAX_DELAY (0xffffffffUL)
#define pdMS_TO_TICKS(ms) ((TickType_t)((ms)*1000 / 1000))
#define pdPASS 1
