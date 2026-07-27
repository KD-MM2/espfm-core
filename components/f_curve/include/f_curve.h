#pragma once
#include "esp_err.h"
#include "f_core.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define F_CURVE_MAX_POINTS 10
#define F_CURVE_MAX_COUNT  16

typedef struct {
    float temp_c;
    uint8_t duty;
} f_curve_point_t;

typedef struct {
    uint8_t id;
    char name[ESPFM_NAME_MAX];
    f_curve_point_t points[F_CURVE_MAX_POINTS];
    uint8_t num_points;
} f_curve_info_t;

typedef struct f_curve *f_curve_handle_t;

esp_err_t f_curve_init(f_curve_handle_t *handle);
esp_err_t f_curve_upsert(f_curve_handle_t handle, const f_curve_info_t *info, uint8_t *id_out);
esp_err_t f_curve_lookup(f_curve_handle_t handle, uint8_t id, float temp_c, uint8_t *duty_out);
esp_err_t f_curve_remove(f_curve_handle_t handle, uint8_t id);
uint8_t f_curve_get_count(f_curve_handle_t handle);
esp_err_t f_curve_get_info(f_curve_handle_t handle, uint8_t id, f_curve_info_t *info_out);

#ifdef __cplusplus
}
#endif
