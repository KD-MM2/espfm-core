#pragma once
#include <stdint.h>

/* Minimal FreeRTOS kernel stub for host-based unit tests. Only the types and
 * constants the f_* sources reference are provided. Mutex functions are defined
 * in the test TU. */

typedef int BaseType_t;
typedef uint32_t TickType_t;

#define portMAX_DELAY (0xffffffffUL)
#define pdMS_TO_TICKS(ms) ((TickType_t)((ms)*1000 / 1000))
