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
    ds18b20_device_handle_t devices[F_DS18B20_MAX_DEVICES];
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

    ESP_LOGI(TAG, "1-Wire bus initialized on GPIO %d", gpio);
    *handle = h;
    return ESP_OK;
}

esp_err_t f_ds18b20_scan(f_ds18b20_handle_t handle, uint8_t *count_out)
{
    if (handle == NULL || count_out == NULL) return ESP_ERR_INVALID_ARG;

    onewire_device_iter_handle_t iter = NULL;
    onewire_device_t next_dev;
    esp_err_t err;

    err = onewire_new_device_iter(handle->bus, &iter);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create device iterator");
        return err;
    }

    handle->device_count = 0;
    do {
        err = onewire_device_iter_get_next(iter, &next_dev);
        if (err == ESP_ERR_NOT_FOUND) break;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Device iteration error: %d", err);
            break;
        }

        ds18b20_config_t ds_cfg = {};
        if (ds18b20_new_device_from_enumeration(&next_dev, &ds_cfg,
                                                &handle->devices[handle->device_count]) == ESP_OK) {
            handle->device_count++;
            ESP_LOGI(TAG, "DS18B20 found: sensor %d", handle->device_count - 1);
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

    ESP_ERROR_CHECK(ds18b20_trigger_temperature_conversion(handle->devices[index]));
    /* DS18B20 needs up to 750ms for 12-bit conversion */
    vTaskDelay(pdMS_TO_TICKS(750));
    ESP_ERROR_CHECK(ds18b20_get_temperature(handle->devices[index], temp_c_out));
    return ESP_OK;
}
