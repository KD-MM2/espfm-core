#pragma once
#include "esp_err.h"
#include "hal/ledc_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define F_LEDC_MAX_CHANNELS 8

typedef struct f_ledc *f_ledc_handle_t;

esp_err_t f_ledc_init(f_ledc_handle_t *handle, uint32_t freq_hz, ledc_timer_bit_t duty_resolution);
esp_err_t f_ledc_add_channel(f_ledc_handle_t handle, uint8_t gpio, uint8_t *channel_id_out);
esp_err_t f_ledc_set_duty(f_ledc_handle_t handle, uint8_t channel_id, float duty_percent);
esp_err_t f_ledc_get_duty(f_ledc_handle_t handle, uint8_t channel_id, float *duty_out);
esp_err_t f_ledc_remove_channel(f_ledc_handle_t handle, uint8_t channel_id);

#ifdef __cplusplus
}
#endif
