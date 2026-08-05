#pragma once
#include "esp_err.h"
#include <stdint.h>

/* Minimal f_ledc.h stub for host-based unit tests of f_control.c.
 * Declares the real public signatures f_fan.h references; the driver bodies
 * are unreferenced and dropped by --gc-sections. */

typedef struct f_ledc *f_ledc_handle_t;

esp_err_t f_ledc_add_channel(f_ledc_handle_t handle, uint8_t gpio, uint8_t *channel_id_out);
esp_err_t f_ledc_set_duty(f_ledc_handle_t handle, uint8_t channel_id, float duty_percent);
esp_err_t f_ledc_stop_channel(f_ledc_handle_t handle, uint8_t channel_id, int idle_level);
esp_err_t f_ledc_remove_channel(f_ledc_handle_t handle, uint8_t channel_id);
