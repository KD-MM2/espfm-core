#pragma once
#include <stdint.h>

/* Minimal ESP-IDF esp_timer.h stub for host-based unit tests.
 * f_source.c references esp_timer_get_time; the test TU wraps it. */

uint64_t esp_timer_get_time(void);
