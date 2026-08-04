#pragma once
#include "esp_err.h"
#include <stdint.h>

/* Minimal f_ledc.h stub for host-based unit tests of f_fan.c.
 * Declares the real public signatures f_fan.c calls; the test TU
 * supplies the --wrap definitions for add/remove channel and the
 * set_duty / stop_channel no-ops (referenced only by gc'd functions). */

typedef struct f_ledc *f_ledc_handle_t;

esp_err_t f_ledc_add_channel(f_ledc_handle_t handle, uint8_t gpio, uint8_t *channel_id_out);
esp_err_t f_ledc_set_duty(f_ledc_handle_t handle, uint8_t channel_id, float duty_percent);
esp_err_t f_ledc_stop_channel(f_ledc_handle_t handle, uint8_t channel_id, int idle_level);
esp_err_t f_ledc_remove_channel(f_ledc_handle_t handle, uint8_t channel_id);
