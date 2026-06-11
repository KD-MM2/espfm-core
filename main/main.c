#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "f_core.h"

static const char *TAG = "espfm";

void app_main(void) {
    /* --- NVS Init --- */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* --- Task Watchdog --- */
    const esp_task_wdt_config_t twdt_config = {
        .timeout_ms = 5000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    ESP_ERROR_CHECK(esp_task_wdt_init(&twdt_config));
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));  /* subscribe main task */
    ESP_LOGI(TAG, "TWDT initialized (5s timeout)");

    ESP_LOGI(TAG, "ESPFanManager v%d.%d.%d starting...",
             ESPFM_VERSION_MAJOR, ESPFM_VERSION_MINOR, ESPFM_VERSION_PATCH);
    ESP_LOGI(TAG, "Chip: %s, IDF: %s", CONFIG_IDF_TARGET, IDF_VER);

    /* --- Boot complete, unsubscribe main from TWDT --- */
    ESP_ERROR_CHECK(esp_task_wdt_delete(NULL));
    ESP_LOGI(TAG, "TWDT: main task unsubscribed, entering idle");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
