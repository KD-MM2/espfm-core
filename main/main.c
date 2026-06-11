#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "f_core.h"
#include "f_wifi.h"
#include "f_http.h"
#include "f_config.h"

static const char *TAG = "espfm";

void app_main(void) {
    /* --- NVS Init --- */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* --- Event Loop --- */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* --- Task Watchdog --- */
    const esp_task_wdt_config_t twdt_config = {
        .timeout_ms = 5000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    ESP_ERROR_CHECK(esp_task_wdt_init(&twdt_config));
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    ESP_LOGI(TAG, "ESPFanManager v%d.%d.%d starting...",
             ESPFM_VERSION_MAJOR, ESPFM_VERSION_MINOR, ESPFM_VERSION_PATCH);
    ESP_LOGI(TAG, "Chip: %s, IDF: %s", CONFIG_IDF_TARGET, IDF_VER);

    /* --- SPIFFS Mount (for static file serving) --- */
    f_config_handle_t config;
    ESP_ERROR_CHECK(f_config_init(&config, "storage", "/spiffs"));

    /* --- WiFi Init --- */
    f_wifi_handle_t wifi;
    ESP_ERROR_CHECK(f_wifi_init(&wifi));

    /* --- HTTP Server Init (starts when WiFi connects) --- */
    f_http_handle_t http;
    ESP_ERROR_CHECK(f_http_init(&http));

    /* --- Wait for WiFi --- */
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    f_wifi_wait_connected(wifi, pdMS_TO_TICKS(30000));

    /* --- Boot complete, unsubscribe main from TWDT --- */
    ESP_ERROR_CHECK(esp_task_wdt_delete(NULL));
    ESP_LOGI(TAG, "Boot complete, entering idle loop");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
