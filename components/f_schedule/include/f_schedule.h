#pragma once
#include "esp_err.h"
#include "f_core.h"
#include "f_fan.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define F_SCHEDULE_MAX_COUNT 8

typedef struct {
    uint8_t  id;
    uint8_t  fan_id;
    uint16_t start_min;
    uint16_t end_min;
    uint8_t  duty;
    bool     enabled;
} f_schedule_info_t;

typedef struct f_schedule *f_schedule_handle_t;

esp_err_t f_schedule_init(f_schedule_handle_t *handle, f_fan_handle_t fan);
esp_err_t f_schedule_add(f_schedule_handle_t handle, const f_schedule_info_t *info, uint8_t *id_out);
esp_err_t f_schedule_remove(f_schedule_handle_t handle, uint8_t id);
esp_err_t f_schedule_start(f_schedule_handle_t handle);
uint8_t f_schedule_get_count(f_schedule_handle_t handle);
esp_err_t f_schedule_get_info(f_schedule_handle_t handle, uint8_t id, f_schedule_info_t *info_out);

#ifdef __cplusplus
}
#endif
