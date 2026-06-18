#pragma once
#include "esp_err.h"
#include "f_core.h"
#include "f_curve.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Named Limits --- */
#define F_CONSTRAINT_DUTY_MIN          0
#define F_CONSTRAINT_DUTY_MAX        100
#define F_CONSTRAINT_MODE_MIN          0
#define F_CONSTRAINT_MODE_MAX          1
#define F_CONSTRAINT_GPIO_MIN          0
#define F_CONSTRAINT_GPIO_MAX         48
#define F_CONSTRAINT_TEMP_C_MIN     -40.0f
#define F_CONSTRAINT_TEMP_C_MAX     125.0f
#define F_CONSTRAINT_SCHED_MIN         0
#define F_CONSTRAINT_SCHED_MAX      1439
#define F_CONSTRAINT_CURVE_POINTS_MIN  2
#define F_CONSTRAINT_NAME_MAX_LEN      ESPFM_NAME_MAX

/* --- Validation ---
   Each returns ESP_OK or ESP_ERR_INVALID_ARG.
   On failure, *err_msg is set to a static description of the violation. */

esp_err_t f_constraints_duty(int val, const char **err_msg);
esp_err_t f_constraints_mode(int val, const char **err_msg);
esp_err_t f_constraints_gpio(int val, const char **err_msg);
esp_err_t f_constraints_name(const char *name, const char **err_msg);
esp_err_t f_constraints_temp_c(float val, const char **err_msg);
esp_err_t f_constraints_schedule_time(int start_min, int end_min, const char **err_msg);
esp_err_t f_constraints_curve_points(const f_curve_point_t *points, uint8_t count,
                                     const char **err_msg);
esp_err_t f_constraints_fan_count(uint8_t current, const char **err_msg);
esp_err_t f_constraints_source_count(uint8_t current, const char **err_msg);
esp_err_t f_constraints_curve_count(uint8_t current, const char **err_msg);
esp_err_t f_constraints_schedule_count(uint8_t current, const char **err_msg);

#ifdef __cplusplus
}
#endif
