#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "f_core.h"
#include "f_gpio.h"
#include "f_wifi.h"
#include "f_ledc.h"
#include "f_pcnt.h"
#include "f_adc.h"
#include "f_ds18b20.h"
#include "f_fan.h"
#include "f_source.h"
#include "f_curve.h"
#include "f_control.h"
#include "f_schedule.h"
#include "f_coap.h"
#include "f_config.h"
#include "f_provision.h"

static const char *TAG = "espfm";

void app_main(void) {
    /* --- NVS Init --- */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* --- Task Watchdog (auto-initialized by ESP-IDF) --- */
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    ESP_LOGI(TAG, "TWDT: main task subscribed");

    /* --- Event Loop --- */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "ESPFanManager v%d.%d.%d starting...",
             ESPFM_VERSION_MAJOR, ESPFM_VERSION_MINOR, ESPFM_VERSION_PATCH);
    ESP_LOGI(TAG, "Chip: %s, IDF: %s", CONFIG_IDF_TARGET, IDF_VER);

    /* --- GPIO Registry --- */
    f_gpio_handle_t gpio;
    ESP_ERROR_CHECK(f_gpio_init(&gpio));

    /* --- Hardware Drivers --- */
    f_adc_handle_t adc;
    ESP_ERROR_CHECK(f_adc_init(&adc));

    f_ds18b20_handle_t ds18b20 = NULL;
    /* DS18B20 init skipped if GPIO not configured */

    f_ledc_handle_t ledc;
    ESP_ERROR_CHECK(f_ledc_init(&ledc, 25000, 11));

    f_pcnt_handle_t pcnt;
    ESP_ERROR_CHECK(f_pcnt_init(&pcnt));

    /* --- Registries --- */
    f_fan_handle_t fan;
    ESP_ERROR_CHECK(f_fan_init(&fan, ledc, pcnt));

    f_source_handle_t source;
    ESP_ERROR_CHECK(f_source_init(&source, adc, ds18b20));

    f_curve_handle_t curve;
    ESP_ERROR_CHECK(f_curve_init(&curve));

    f_schedule_handle_t schedule;
    ESP_ERROR_CHECK(f_schedule_init(&schedule, fan));

    /* --- Persistent Config --- */
    f_config_handle_t config;
    ESP_ERROR_CHECK(f_config_init(&config, "storage", "/littlefs"));
    ESP_ERROR_CHECK(f_config_load_all(config, fan, source, curve, schedule));

    /* --- CoAP Server (UDP :5683, Protobuf, WiFi-aware lifecycle) --- */
    f_coap_handle_t coap;
    ESP_ERROR_CHECK(f_coap_init(&coap, fan, source, curve, schedule, config));

    /* --- WiFi APSTA (AP starts immediately) --- */
    f_wifi_handle_t wifi;
    ESP_ERROR_CHECK(f_wifi_init(&wifi));

    /* --- WiFi Provisioning (captive portal on STA failure) --- */
    f_provision_handle_t provision;
    ESP_ERROR_CHECK(f_provision_init(&provision, wifi));

    /* --- Wait for WiFi --- */
    ESP_LOGI(TAG, "Waiting for WiFi...");
    f_wifi_wait_connected(wifi, pdMS_TO_TICKS(30000));

    /* --- Control Engine --- */
    f_control_handle_t control;
    ESP_ERROR_CHECK(f_control_init(&control, fan, source, curve));
    ESP_ERROR_CHECK(f_control_start(control));

    /* --- Schedule Service --- */
    ESP_ERROR_CHECK(f_schedule_start(schedule));

    /* --- Unsubscribe main from TWDT --- */
    ESP_ERROR_CHECK(esp_task_wdt_delete(NULL));
    ESP_LOGI(TAG, "TWDT: main task unsubscribed");

    ESP_LOGI(TAG, "Boot complete — %d fans, %d sources, %d curves, %d schedules",
             f_fan_get_count(fan), f_source_get_count(source),
             f_curve_get_count(curve), f_schedule_get_count(schedule));
    while (1) { vTaskDelay(pdMS_TO_TICKS(60000)); }
}
