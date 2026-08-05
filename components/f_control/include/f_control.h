#pragma once
#include "esp_err.h"
#include "f_core.h"
#include "f_fan.h"
#include "f_source.h"
#include "f_curve.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct f_control *f_control_handle_t;

esp_err_t f_control_init(f_control_handle_t *handle, f_fan_handle_t fan, f_source_handle_t source,
                         f_curve_handle_t curve);
esp_err_t f_control_start(f_control_handle_t handle);
esp_err_t f_control_set_hysteresis(f_control_handle_t handle, uint8_t threshold_pct);
esp_err_t f_control_set_ramp_rates(f_control_handle_t handle, uint8_t max_up_pct,
                                   uint8_t max_down_pct);
esp_err_t f_control_set_failsafe(f_control_handle_t handle, failsafe_policy_t policy,
                                 uint8_t safe_duty);
esp_err_t f_control_get_tunables(f_control_handle_t handle, uint8_t *hysteresis_pct,
                                 uint8_t *ramp_up_pct, uint8_t *ramp_down_pct,
                                 failsafe_policy_t *failsafe_policy, uint8_t *failsafe_safe_duty);

#ifdef __cplusplus
}
#endif
