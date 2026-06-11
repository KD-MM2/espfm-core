#pragma once
#include "esp_err.h"
#include "f_fan.h"
#include "f_source.h"
#include "f_curve.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct f_config *f_config_handle_t;

esp_err_t f_config_init(f_config_handle_t *handle, const char *partition_label,
                        const char *mount_point);
esp_err_t f_config_save_all(f_config_handle_t handle, f_fan_handle_t fan,
                            f_source_handle_t source, f_curve_handle_t curve);
esp_err_t f_config_load_all(f_config_handle_t handle, f_fan_handle_t fan,
                            f_source_handle_t source, f_curve_handle_t curve);

#ifdef __cplusplus
}
#endif
