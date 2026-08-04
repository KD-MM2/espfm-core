#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "onewire_bus.h"

/* Minimal ds18b20.h stub for host-based unit tests of f_ds18b20.c. Only the
 * device-handle typedef and the APIs f_ds18b20.c references are declared; the
 * scan/read/trigger functions are dropped by --gc-sections, so only the types
 * must parse. */

typedef struct ds18b20_device_t *ds18b20_device_handle_t;

typedef struct {
    int _dummy;
} ds18b20_config_t;

esp_err_t ds18b20_new_device_from_enumeration(onewire_device_t *device,
                                              const ds18b20_config_t *config,
                                              ds18b20_device_handle_t *ret_ds18b20);
esp_err_t ds18b20_del_device(ds18b20_device_handle_t ds18b20);
esp_err_t ds18b20_trigger_temperature_conversion_for_all(onewire_bus_handle_t bus);
esp_err_t ds18b20_get_temperature(ds18b20_device_handle_t ds18b20, float *temperature);
esp_err_t ds18b20_get_device_address(ds18b20_device_handle_t ds18b20,
                                     onewire_device_address_t *ret_address);
