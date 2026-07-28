#include "f_ds18b20.h"
#include "onewire_bus.h"
#include "ds18b20.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "f_ds18b20";

struct f_ds18b20 {
    onewire_bus_handle_t bus;
    uint8_t bus_gpio;
    ds18b20_device_handle_t devices[F_DS18B20_MAX_DEVICES];
    uint64_t rom_codes[F_DS18B20_MAX_DEVICES];
    uint8_t device_count;
};

esp_err_t f_ds18b20_init(f_ds18b20_handle_t *handle, uint8_t gpio)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    f_ds18b20_handle_t h = calloc(1, sizeof(struct f_ds18b20));
    if (h == NULL) return ESP_ERR_NO_MEM;

    onewire_bus_config_t bus_cfg = {
        .bus_gpio_num = gpio,
    };
    onewire_bus_rmt_config_t rmt_cfg = {
        .max_rx_bytes = 10,
    };
    ESP_ERROR_CHECK(onewire_new_bus_rmt(&bus_cfg, &rmt_cfg, &h->bus));
    h->bus_gpio = gpio;

    ESP_LOGI(TAG, "1-Wire bus initialized on GPIO %d", gpio);
    *handle = h;
    return ESP_OK;
}

esp_err_t f_ds18b20_scan(f_ds18b20_handle_t handle, uint8_t *count_out)
{
    if (handle == NULL || count_out == NULL) return ESP_ERR_INVALID_ARG;

    /* Free old devices to prevent memory leak on rescan */
    for (int i = 0; i < handle->device_count; i++) {
        if (handle->devices[i]) {
            ds18b20_del_device(handle->devices[i]);
            handle->devices[i] = NULL;
        }
    }
    handle->device_count = 0;
    memset(handle->rom_codes, 0, sizeof(handle->rom_codes));

    onewire_device_iter_handle_t iter = NULL;
    onewire_device_t next_dev;
    esp_err_t err;

    err = onewire_new_device_iter(handle->bus, &iter);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create device iterator");
        return err;
    }

    do {
        err = onewire_device_iter_get_next(iter, &next_dev);
        if (err == ESP_ERR_NOT_FOUND) break;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Device iteration error: %d", err);
            break;
        }

        ds18b20_config_t ds_cfg = {};
        ds18b20_device_handle_t dev;
        if (ds18b20_new_device_from_enumeration(&next_dev, &ds_cfg, &dev) == ESP_OK) {
            onewire_device_address_t addr;
            ds18b20_get_device_address(dev, &addr);
            handle->devices[handle->device_count] = dev;
            handle->rom_codes[handle->device_count] = addr;
            handle->device_count++;
            ESP_LOGI(TAG, "DS18B20 found: sensor %d, ROM=0x%016llX",
                     handle->device_count - 1, (unsigned long long)addr);
            if (handle->device_count >= F_DS18B20_MAX_DEVICES) {
                ESP_LOGI(TAG, "Max devices reached, stopping scan");
                break;
            }
        }
    } while (true);

    onewire_del_device_iter(iter);
    *count_out = handle->device_count;
    ESP_LOGI(TAG, "Scan complete: %d DS18B20 devices found", handle->device_count);
    return ESP_OK;
}

esp_err_t f_ds18b20_read_temp(f_ds18b20_handle_t handle, uint8_t index, float *temp_c_out)
{
    if (handle == NULL || temp_c_out == NULL) return ESP_ERR_INVALID_ARG;
    if (index >= handle->device_count) return ESP_ERR_INVALID_ARG;
    if (handle->devices[index] == NULL) return ESP_ERR_INVALID_STATE;

    return ds18b20_get_temperature(handle->devices[index], temp_c_out);
}

esp_err_t f_ds18b20_trigger_all(f_ds18b20_handle_t handle)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    if (handle->device_count == 0) return ESP_OK;
    return ds18b20_trigger_temperature_conversion_for_all(handle->bus);
}

esp_err_t f_ds18b20_get_rom_code(f_ds18b20_handle_t handle, uint8_t index, uint64_t *rom_out)
{
    if (handle == NULL || rom_out == NULL) return ESP_ERR_INVALID_ARG;
    if (index >= handle->device_count) return ESP_ERR_INVALID_ARG;
    *rom_out = handle->rom_codes[index];
    return ESP_OK;
}

esp_err_t f_ds18b20_find_by_rom(f_ds18b20_handle_t handle, uint64_t rom_code, uint8_t *index_out)
{
    if (handle == NULL || index_out == NULL) return ESP_ERR_INVALID_ARG;
    for (uint8_t i = 0; i < handle->device_count; i++) {
        if (handle->rom_codes[i] == rom_code) {
            *index_out = i;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
