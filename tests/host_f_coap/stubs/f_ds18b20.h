#pragma once
#include "esp_err.h"
#include "f_gpio.h"
#include <stdint.h>

/* Minimal f_ds18b20.h stub for host-based unit tests of f_coap_routes.c.
 * Full API surface referenced by the (gc'd) DS18B20 handlers; only declared,
 * never defined at link time. */

#define F_DS18B20_MAX_DEVICES 4

typedef struct f_ds18b20 *f_ds18b20_handle_t;

esp_err_t f_ds18b20_init(f_ds18b20_handle_t *handle, uint8_t gpio, f_gpio_handle_t gpio_registry);
esp_err_t f_ds18b20_scan(f_ds18b20_handle_t handle, uint8_t *count_out);
esp_err_t f_ds18b20_read_temp(f_ds18b20_handle_t handle, uint8_t index, float *temp_c_out);
esp_err_t f_ds18b20_trigger_all(f_ds18b20_handle_t handle);
esp_err_t f_ds18b20_get_rom_code(f_ds18b20_handle_t handle, uint8_t index, uint64_t *rom_out);
esp_err_t f_ds18b20_find_by_rom(f_ds18b20_handle_t handle, uint64_t rom_code, uint8_t *index_out);
