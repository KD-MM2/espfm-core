#pragma once
#include "esp_err.h"
#include "f_core.h"
#include "f_ledc.h"
#include "f_pcnt.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define F_FAN_MAX_COUNT 8
#define F_FAN_TACH_NONE 0xFF

typedef struct {
    uint8_t   id;
    char      name[ESPFM_NAME_MAX];
    fan_mode_t mode;
    uint8_t   duty;
    uint16_t  rpm;
    fan_alarm_t alarm;
    bool      enabled;
    bool      inverted;
    uint8_t   pwm_gpio;
    uint8_t   tach_gpio;
    uint8_t   source_id;
    uint8_t   curve_id;
    uint8_t   schedule_id;
    uint8_t   group_id;
} f_fan_info_t;

typedef struct f_fan *f_fan_handle_t;

esp_err_t f_fan_init(f_fan_handle_t *handle, f_ledc_handle_t ledc, f_pcnt_handle_t pcnt);
esp_err_t f_fan_add(f_fan_handle_t handle, uint8_t pwm_gpio, uint8_t tach_gpio,
                    const char *name, uint8_t *id_out);
esp_err_t f_fan_remove(f_fan_handle_t handle, uint8_t id);
esp_err_t f_fan_set_duty(f_fan_handle_t handle, uint8_t id, uint8_t duty);
esp_err_t f_fan_set_mode(f_fan_handle_t handle, uint8_t id, fan_mode_t mode);
esp_err_t f_fan_set_source(f_fan_handle_t handle, uint8_t id, uint8_t source_id);
esp_err_t f_fan_set_curve(f_fan_handle_t handle, uint8_t id, uint8_t curve_id);
esp_err_t f_fan_set_schedule(f_fan_handle_t handle, uint8_t id, uint8_t schedule_id);
esp_err_t f_fan_set_enabled(f_fan_handle_t handle, uint8_t id, bool enabled);
esp_err_t f_fan_set_inverted(f_fan_handle_t handle, uint8_t id, bool inverted);
esp_err_t f_fan_set_group(f_fan_handle_t handle, uint8_t id, uint8_t group_id);
esp_err_t f_fan_update_rpm(f_fan_handle_t handle, uint8_t id);
esp_err_t f_fan_get_info(f_fan_handle_t handle, uint8_t id, f_fan_info_t *info_out);
uint8_t f_fan_get_count(f_fan_handle_t handle);
esp_err_t f_fan_for_each(f_fan_handle_t handle,
                          void (*callback)(const f_fan_info_t *, void *), void *ctx);

#ifdef __cplusplus
}
#endif
