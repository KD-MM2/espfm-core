#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Minimal ESP-IDF esp_event.h stub for host-based unit tests.
 * Declares esp_event_post (wrapped in the test TU) so the control-loop
 * callback's event posts compile and link against the --wrap hook. */

typedef void *esp_event_base_t;

#define ESP_EVENT_DECLARE_BASE(id) extern esp_event_base_t id

esp_err_t esp_event_post(esp_event_base_t event_base, int32_t event_id, void *event_data,
                         size_t event_data_size, uint32_t ticks_to_wait);
