#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* Minimal onewire_bus.h stub for host-based unit tests of f_ds18b20.c.
 * Declares the API surface f_ds18b20.c references at compile time. The test
 * TU supplies --wrap definitions for onewire_new_bus_rmt / onewire_bus_del;
 * the scan/iter APIs are only parsed (the functions using them are dropped by
 * --gc-sections), so they need no link-time definitions. */

typedef struct onewire_bus *onewire_bus_handle_t;
typedef uint64_t onewire_device_address_t;
typedef struct onewire_device_iter_t *onewire_device_iter_handle_t;

typedef struct {
    int bus_gpio_num;
} onewire_bus_config_t;

typedef struct {
    uint32_t max_rx_bytes;
} onewire_bus_rmt_config_t;

typedef struct onewire_device_t {
    onewire_device_address_t address;
} onewire_device_t;

esp_err_t onewire_new_bus_rmt(const onewire_bus_config_t *bus_config,
                              const onewire_bus_rmt_config_t *rmt_config,
                              onewire_bus_handle_t *ret_bus);
esp_err_t onewire_bus_del(onewire_bus_handle_t bus);
esp_err_t onewire_new_device_iter(onewire_bus_handle_t bus, onewire_device_iter_handle_t *ret_iter);
esp_err_t onewire_device_iter_get_next(onewire_device_iter_handle_t iter, onewire_device_t *dev);
esp_err_t onewire_del_device_iter(onewire_device_iter_handle_t iter);
