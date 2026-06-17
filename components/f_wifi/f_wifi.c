#include "f_wifi.h"
#include "f_core.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <lwip/inet.h>

static const char *TAG = "f_wifi";

#define WIFI_CONNECTED_BIT BIT0
#define AP_IP              "192.168.4.1"

struct f_wifi {
    EventGroupHandle_t event_group;
    bool connected;
    bool sta_connected;
    char ip_str[16];
    int retry_count;
};

static void _event_handler(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data) {
    f_wifi_handle_t wifi = (f_wifi_handle_t)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        /* AP is ready — set static IP */
        esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (ap_netif) {
            esp_netif_ip_info_t ip_info;
            ip_info.ip.addr = ipaddr_addr(AP_IP);
            ip_info.gw.addr = ipaddr_addr(AP_IP);
            ip_info.netmask.addr = ipaddr_addr("255.255.255.0");
            esp_netif_dhcps_stop(ap_netif);
            esp_netif_set_ip_info(ap_netif, &ip_info);
            esp_netif_dhcps_start(ap_netif);
        }
        /* Always signal connected — AP is always available */
        if (!wifi->connected) {
            strncpy(wifi->ip_str, AP_IP, sizeof(wifi->ip_str) - 1);
            wifi->connected = true;
            xEventGroupSetBits(wifi->event_group, WIFI_CONNECTED_BIT);
            esp_event_post(ESPFM_EVENT, ESPFM_EVENT_WIFI_CONNECTED, NULL, 0, pdMS_TO_TICKS(100));
            ESP_LOGI(TAG, "AP ready, IP: %s", wifi->ip_str);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (wifi->retry_count < CONFIG_ESPFM_WIFI_MAX_RETRY) {
            esp_wifi_connect();
            wifi->retry_count++;
            ESP_LOGI(TAG, "STA retry %d/%d", wifi->retry_count, CONFIG_ESPFM_WIFI_MAX_RETRY);
        } else {
            ESP_LOGW(TAG, "STA failed after %d retries — AP only mode", wifi->retry_count);
            wifi->sta_connected = false;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(wifi->ip_str, sizeof(wifi->ip_str),
                 IPSTR, IP2STR(&event->ip_info.ip));
        wifi->connected = true;
        wifi->sta_connected = true;
        wifi->retry_count = 0;
        xEventGroupSetBits(wifi->event_group, WIFI_CONNECTED_BIT);
        esp_event_post(ESPFM_EVENT, ESPFM_EVENT_WIFI_CONNECTED, NULL, 0, pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "STA connected, IP: %s", wifi->ip_str);
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
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &_event_handler, wifi, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &_event_handler, wifi, &instance_got_ip));

    /* Generate unique AP SSID from MAC */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    /* STA config */
    wifi_config_t sta_config = {
        .sta = {
            .ssid = CONFIG_ESPFM_WIFI_SSID,
            .password = CONFIG_ESPFM_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    /* AP config — open, unique SSID */
    wifi_config_t ap_config = {
        .ap = {
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid),
             "ESPFM-%02X%02X", mac[4], mac[5]);

    /* Start in APSTA mode — both AP and STA simultaneously */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi APSTA mode — STA:'%s'  AP:'%s'",
             CONFIG_ESPFM_WIFI_SSID, ap_config.ap.ssid);
    *handle = wifi;
    return ESP_OK;
}

esp_err_t f_wifi_wait_connected(f_wifi_handle_t handle, TickType_t timeout) {
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    EventBits_t bits = xEventGroupWaitBits(
        handle->event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, timeout);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "%s, IP: %s",
                 handle->sta_connected ? "STA connected" : "AP mode", handle->ip_str);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "WiFi startup timeout");
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
