#pragma once
#include <stdint.h>
#include "esp_err.h"

/* Minimal ESP-IDF esp_timer.h stub for host-based unit tests of f_coap_routes.c.
 * Overrides tests/host_f_config/stubs/esp_timer.h with the timer-create API the
 * (gc'd) system-reboot handler references at compile time. */

typedef struct esp_timer_impl_ *esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void *arg);

typedef struct {
    esp_timer_cb_t callback;
    void *arg;
    const char *name;
    uint8_t dispatch_method;
} esp_timer_create_args_t;

uint64_t esp_timer_get_time(void);
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out_handle);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);
