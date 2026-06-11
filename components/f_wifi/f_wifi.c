#include "f_wifi.h"
#include "f_core.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <lwip/inet.h>

static const char *TAG = "f_wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

struct f_wifi {
    EventGroupHandle_t event_group;
    bool connected;
    char ip_str[16];
    int retry_count;
};

static void _event_handler(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data) {
    f_wifi_handle_t wifi = (f_wifi_handle_t)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi->connected = false;
        if (wifi->retry_count < CONFIG_ESPFM_WIFI_MAX_RETRY) {
            esp_wifi_connect();
            wifi->retry_count++;
            ESP_LOGI(TAG, "retry %d/%d to connect to AP",
                     wifi->retry_count, CONFIG_ESPFM_WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(wifi->event_group, WIFI_FAIL_BIT);
        }
        esp_event_post(ESPFM_EVENT, ESPFM_EVENT_WIFI_DISCONNECTED, NULL, 0, pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "disconnected from AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(wifi->ip_str, sizeof(wifi->ip_str),
                 IPSTR, IP2STR(&event->ip_info.ip));
        wifi->connected = true;
        wifi->retry_count = 0;
        xEventGroupSetBits(wifi->event_group, WIFI_CONNECTED_BIT);
        esp_event_post(ESPFM_EVENT, ESPFM_EVENT_WIFI_CONNECTED, NULL, 0, pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "connected, IP: %s", wifi->ip_str);
    }
}

esp_err_t f_wifi_init(f_wifi_handle_t *handle) {
    if (handle == NULL) return ESP_ERR_INVALID_ARG;

    f_wifi_handle_t wifi = calloc(1, sizeof(struct f_wifi));
    if (wifi == NULL) return ESP_ERR_NO_MEM;

    wifi->event_group = xEventGroupCreate();
    if (wifi->event_group == NULL) {
        free(wifi);
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &_event_handler, wifi, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &_event_handler, wifi, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_ESPFM_WIFI_SSID,
            .password = CONFIG_ESPFM_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi station init complete (SSID: %s)", CONFIG_ESPFM_WIFI_SSID);
    *handle = wifi;
    return ESP_OK;
}

esp_err_t f_wifi_wait_connected(f_wifi_handle_t handle, TickType_t timeout) {
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    EventBits_t bits = xEventGroupWaitBits(
        handle->event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, timeout);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected successfully");
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "WiFi connection failed after max retries");
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGE(TAG, "WiFi connection timeout");
    return ESP_ERR_TIMEOUT;
}

bool f_wifi_is_connected(f_wifi_handle_t handle) {
    return handle != NULL && handle->connected;
}

esp_err_t f_wifi_get_ip_str(f_wifi_handle_t handle, char *buf, size_t len) {
    if (handle == NULL || buf == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    strncpy(buf, handle->ip_str, len - 1);
    buf[len - 1] = '\0';
    return ESP_OK;
}
