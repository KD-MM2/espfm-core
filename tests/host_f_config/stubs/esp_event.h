#pragma once

/* Minimal ESP-IDF esp_event.h stub for host-based unit tests. */

typedef void *esp_event_base_t;

#define ESP_EVENT_DECLARE_BASE(id) extern esp_event_base_t id
