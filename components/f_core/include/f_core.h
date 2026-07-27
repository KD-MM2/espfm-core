#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Version --- */
#define ESPFM_VERSION_MAJOR 2
#define ESPFM_VERSION_MINOR 0
#define ESPFM_VERSION_PATCH 0

/* --- Limits --- */
#define ESPFM_MAX_FANS      8
#define ESPFM_MAX_SOURCES   8
#define ESPFM_MAX_CURVES    16
#define ESPFM_MAX_SCHEDULES 8
#define ESPFM_NAME_MAX      16

/* --- Enums --- */
typedef enum {
    FAN_MODE_MANUAL = 0,
    FAN_MODE_AUTO,
} fan_mode_t;

typedef enum {
    SOURCE_TYPE_NTC = 0,
    SOURCE_TYPE_DS18B20,
    SOURCE_TYPE_MANUAL,
} source_type_t;

typedef enum {
    SOURCE_STATUS_VALID = 0,
    SOURCE_STATUS_STALE,
    SOURCE_STATUS_INVALID,
} source_status_t;

typedef enum {
    FAN_ALARM_NONE = 0,
    FAN_ALARM_STALL,
    FAN_ALARM_OVERTEMP,
} fan_alarm_t;

typedef enum {
    FAILSAFE_HOLD = 0,
    FAILSAFE_FULL_SPEED,
    FAILSAFE_SAFE_DUTY,
    FAILSAFE_ALT_SOURCE,
} failsafe_policy_t;

/* --- ESP Event Base --- */
ESP_EVENT_DECLARE_BASE(ESPFM_EVENT);

typedef enum {
    ESPFM_EVENT_WIFI_CONNECTED,
    ESPFM_EVENT_WIFI_DISCONNECTED,
    ESPFM_EVENT_SOURCE_INVALID,
    ESPFM_EVENT_SOURCE_VALID,
    ESPFM_EVENT_FAN_ALARM,
    ESPFM_EVENT_FAN_ALARM_CLEAR,
    ESPFM_EVENT_CONFIG_CHANGED,
    ESPFM_EVENT_WIFI_STA_FAILED,
} espfm_event_id_t;

#ifdef __cplusplus
}
#endif
