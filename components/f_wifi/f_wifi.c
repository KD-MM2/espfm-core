#include "f_wifi.h"
#include "f_core.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include <string.h>
#include <time.h>
#include <lwip/inet.h>
#include "esp_sntp.h"

static const char *TAG = "f_wifi";

#define WIFI_CONNECTED_BIT BIT0
#define AP_IP              "192.168.4.1"

struct f_wifi {
    EventGroupHandle_t event_group;
    bool connected;
    bool sta_connected;
    bool ap_active;
    char ip_str[16];
    int retry_count;
    TimerHandle_t ap_stop_timer;
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
};

/* Deferred AP stop — called from timer context (safe to call WiFi APIs) */
static void _ap_stop_timer_cb(TimerHandle_t timer)
{
    f_wifi_handle_t wifi = (f_wifi_handle_t)pvTimerGetTimerID(timer);
    if (!wifi || !wifi->ap_active) return;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        wifi->ap_active = false;
        ESP_LOGI(TAG, "AP disabled (STA connected)");
    } else {
        ESP_LOGW(TAG, "Failed to disable AP: %s", esp_err_to_name(err));
    }
}

static void _start_sntp(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started (non-blocking)");
}

static void _event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                           void *event_data)
{
    f_wifi_handle_t wifi = (f_wifi_handle_t)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        wifi->ap_active       = true;
        esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (ap_netif) {
            esp_netif_ip_info_t ip_info;
            ip_info.ip.addr      = ipaddr_addr(AP_IP);
            ip_info.gw.addr      = ipaddr_addr(AP_IP);
            ip_info.netmask.addr = ipaddr_addr("255.255.255.0");
            esp_netif_dhcps_stop(ap_netif);
            esp_netif_set_ip_info(ap_netif, &ip_info);
            esp_netif_dhcps_start(ap_netif);
        }
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
        } else if (wifi->retry_count == CONFIG_ESPFM_WIFI_MAX_RETRY) {
            /* First time hitting max — signal failure and stop STA */
            wifi->retry_count++; /* Prevent re-entry on subsequent disconnects */
            ESP_LOGW(TAG, "STA failed after %d retries", wifi->retry_count - 1);
            wifi->sta_connected = false;
            esp_wifi_disconnect(); /* Stop STA retry loop — frees driver for AP+scan */
            esp_event_post(ESPFM_EVENT, ESPFM_EVENT_WIFI_STA_FAILED, NULL, 0, pdMS_TO_TICKS(100));
        } /* else: already failed, ignore subsequent disconnects */
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(wifi->ip_str, sizeof(wifi->ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        wifi->connected     = true;
        wifi->sta_connected = true;
        wifi->retry_count   = 0;
        xEventGroupSetBits(wifi->event_group, WIFI_CONNECTED_BIT);
        esp_event_post(ESPFM_EVENT, ESPFM_EVENT_WIFI_CONNECTED, NULL, 0, pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "STA connected, IP: %s", wifi->ip_str);

        /* Defer AP stop to timer (can't call esp_wifi_set_mode from event handler) */
        if (wifi->ap_active && wifi->ap_stop_timer) {
            xTimerStart(wifi->ap_stop_timer, 0);
        }

        _start_sntp();
    }
}

esp_err_t f_wifi_init(f_wifi_handle_t *handle)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;

    f_wifi_handle_t wifi = calloc(1, sizeof(struct f_wifi));
    if (wifi == NULL) return ESP_ERR_NO_MEM;

    wifi->event_group = xEventGroupCreate();
    if (wifi->event_group == NULL) {
        free(wifi);
        return ESP_ERR_NO_MEM;
    }

    /* Create one-shot timer for deferred AP stop */
    wifi->ap_stop_timer =
        xTimerCreate("ap_stop", pdMS_TO_TICKS(2000), pdFALSE, wifi, _ap_stop_timer_cb);

    esp_err_t netif_err = esp_netif_init();
    if (netif_err != ESP_OK && netif_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(netif_err);
    }
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));

    /* Try loading saved STA config from NVS, fall back to Kconfig defaults */
    wifi_config_t sta_config;
    esp_err_t load_err = esp_wifi_get_config(WIFI_IF_STA, &sta_config);
    if (load_err != ESP_OK || sta_config.sta.ssid[0] == '\0') {
        /* No saved config — use Kconfig defaults */
        memset(&sta_config, 0, sizeof(sta_config));
        strncpy((char *)sta_config.sta.ssid, CONFIG_ESPFM_WIFI_SSID,
                sizeof(sta_config.sta.ssid) - 1);
        strncpy((char *)sta_config.sta.password, CONFIG_ESPFM_WIFI_PASSWORD,
                sizeof(sta_config.sta.password) - 1);
        sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_LOGI(TAG, "WiFi STA using Kconfig defaults: %s", CONFIG_ESPFM_WIFI_SSID);
    } else {
        ESP_LOGI(TAG, "WiFi STA using saved config: %s", sta_config.sta.ssid);
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &_event_handler, wifi, &wifi->instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &_event_handler, wifi, &wifi->instance_got_ip));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    wifi_config_t ap_config = {
        .ap =
            {
                .max_connection = 4,
                .authmode       = WIFI_AUTH_WPA2_PSK,
                .pmf_cfg =
                    {
                        .capable  = true,
                        .required = false,
                    },
            },
    };
    strncpy((char *)ap_config.ap.password, CONFIG_ESPFM_AP_PASSWORD,
            sizeof(ap_config.ap.password) - 1);
    ESP_LOGI(TAG, "AP password set from Kconfig (WPA2-PSK)");
    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "ESPFM-%02X%02X", mac[4],
             mac[5]);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi APSTA mode — STA:'%s'  AP:'%s' (WPA2-PSK)", CONFIG_ESPFM_WIFI_SSID,
             ap_config.ap.ssid);
    *handle = wifi;
    return ESP_OK;
}

esp_err_t f_wifi_wait_connected(f_wifi_handle_t handle, TickType_t timeout)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    EventBits_t bits =
        xEventGroupWaitBits(handle->event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, timeout);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "%s, IP: %s", handle->sta_connected ? "STA connected" : "AP mode",
                 handle->ip_str);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "WiFi startup timeout");
    return ESP_ERR_TIMEOUT;
}

bool f_wifi_is_connected(f_wifi_handle_t handle)
{
    return handle != NULL && handle->connected;
}

esp_err_t f_wifi_get_ip_str(f_wifi_handle_t handle, char *buf, size_t len)
{
    if (handle == NULL || buf == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    strncpy(buf, handle->ip_str, len - 1);
    buf[len - 1] = '\0';
    return ESP_OK;
}
