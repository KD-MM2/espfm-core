#pragma once
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct f_adc *f_adc_handle_t;

esp_err_t f_adc_init(f_adc_handle_t *handle);
esp_err_t f_adc_read_raw(f_adc_handle_t handle, uint8_t gpio, int *raw_out);
esp_err_t f_adc_raw_to_voltage(f_adc_handle_t handle, int raw, uint16_t vref_mv,
                               float *voltage_out);
esp_err_t f_adc_ntc_temp(float voltage, float vcc, float r_divider, float beta, float r0,
                         float t0_k, float *temp_c_out);

#ifdef __cplusplus
}
#endif
