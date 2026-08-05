#pragma once
#include "FreeRTOS.h"

/* Minimal FreeRTOS task.h stub for host-based unit tests. f_control.c's
 * _ctrl_task / f_control_start are dropped by --gc-sections, so the task
 * functions only need declarations here (definitions live in the test TU so
 * the whole TU type-checks). */

typedef void *TaskHandle_t;

BaseType_t xTaskCreate(void (*task_func)(void *), const char *name, uint32_t stack_size,
                       void *param, uint32_t priority, TaskHandle_t *task_handle);
void vTaskDelete(void *task);
void vTaskDelay(TickType_t ticks);
