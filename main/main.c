#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "espfm";

void app_main(void) {
    ESP_LOGI(TAG, "ESPFanManager v2.0.0 starting...");
    ESP_LOGI(TAG, "Chip: %s, IDF: %s", CONFIG_IDF_TARGET, IDF_VER);
    while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); }
}
