#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "nvs_flash.h"
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

    /* --- Task Watchdog (ESP-IDF v6.0.1 auto-inits TWDT on ESP32-S3) --- */
    ESP_LOGI(TAG, "TWDT: managed by ESP-IDF");
    ESP_LOGI(TAG, "ESPFanManager v%d.%d.%d starting...",
             ESPFM_VERSION_MAJOR, ESPFM_VERSION_MINOR, ESPFM_VERSION_PATCH);
    ESP_LOGI(TAG, "Chip: %s, IDF: %s", CONFIG_IDF_TARGET, IDF_VER);

    /* --- LittleFS Mount (for config and static files) --- */
    f_config_handle_t config;
    ESP_ERROR_CHECK(f_config_init(&config, "storage", "/littlefs"));

    /* --- HTTP Server Init (register WiFi event handlers BEFORE WiFi starts) --- */
    f_http_handle_t http;
    ESP_ERROR_CHECK(f_http_init(&http));

    /* --- WiFi Init (APSTA — AP starts immediately, fires CONNECTED event) --- */
    f_wifi_handle_t wifi;
    ESP_ERROR_CHECK(f_wifi_init(&wifi));

    /* --- Wait for WiFi --- */
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    f_wifi_wait_connected(wifi, pdMS_TO_TICKS(30000));

    /* --- Boot complete --- */
    ESP_LOGI(TAG, "Boot complete, entering idle loop");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
