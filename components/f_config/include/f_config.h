#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include "f_fan.h"
#include "f_source.h"
#include "f_curve.h"
#include "f_schedule.h"
#include "f_gpio.h"
#include "espfm.pb.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct f_config *f_config_handle_t;

esp_err_t f_config_init(f_config_handle_t *handle, const char *partition_label,
                        const char *mount_point);
esp_err_t f_config_save_all(f_config_handle_t handle, f_fan_handle_t fan, f_source_handle_t source,
                            f_curve_handle_t curve, f_schedule_handle_t schedule);
/* Force-persist config.pb, bypassing the 3s save debounce. Used by config import before reboot. */
esp_err_t f_config_save_all_forced(f_config_handle_t handle, f_fan_handle_t fan,
                                   f_source_handle_t source, f_curve_handle_t curve,
                                   f_schedule_handle_t schedule);
esp_err_t f_config_load_all(f_config_handle_t handle, f_fan_handle_t fan, f_source_handle_t source,
                            f_curve_handle_t curve, f_schedule_handle_t schedule);
/* Strict import: validate entire ConfigFile, then clear all registries, apply, and force-persist.
 * gpio is the live f_gpio registry used to reject chip-reserved pins and the active
 * DS18B20 bus pin before the clear; pass NULL to skip those live-registry checks.
 * Returns ESP_ERR_INVALID_ARG on validation failure (zero mutation) with *err_msg set.
 * Returns ESP_FAIL on apply failure: the partial apply is re-cleared (empty-but-consistent
 * registries) and *err_msg holds the real claim/apply error. The normalized ESP_FAIL (never
 * ESP_ERR_INVALID_ARG) lets the caller distinguish apply failure from validation failure,
 * since apply failure mutates state and needs a recovery reboot.
 * Returns a non-ESP_OK error on persist failure too (ESP_FAIL from fopen/short-write/
 * pb-encode, or ESP_ERR_NO_MEM from the export buffer calloc); *err_msg stays NULL (the
 * apply fully succeeded, only the disk write failed — no recovery reboot needed).
 * The caller keys the recovery decision on err_msg, not on the exact code. */
esp_err_t f_config_import_all(f_config_handle_t handle, f_fan_handle_t fan,
                              f_source_handle_t source, f_curve_handle_t curve,
                              f_schedule_handle_t schedule, f_gpio_handle_t gpio,
                              const ConfigFile *cfg, const char **err_msg);

/* Build a ConfigFile from the four registries (version "3.0"), nanopb-encode it into a newly
 * heap-allocated buffer, and return the buffer and byte length via buf_out/len_out. The caller
 * owns the buffer and must free() it when done. */
esp_err_t f_config_export_all(f_fan_handle_t fan, f_source_handle_t source, f_curve_handle_t curve,
                              f_schedule_handle_t schedule, uint8_t **buf_out, size_t *len_out);

esp_err_t f_config_save_ds18b20_gpio(f_config_handle_t handle, uint8_t gpio);
esp_err_t f_config_load_ds18b20_gpio(f_config_handle_t handle, uint8_t *gpio_out);

#ifdef __cplusplus
}
#endif
