#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Minimal ESP-IDF esp_wifi.h stub for host-based unit tests of f_coap_routes.c.
 * Only the types/constants/functions referenced by the (gc'd) WiFi handlers
 * are declared; nothing here needs a definition at link time. */

typedef struct {
    uint8_t scan_type;
    struct {
        struct {
            uint16_t min;
            uint16_t max;
        } active;
        struct {
            uint16_t min;
            uint16_t max;
        } passive;
    } scan_time;
} wifi_scan_config_t;

typedef struct {
    uint8_t ssid[32];
    int8_t rssi;
    uint8_t primary;
    uint8_t authmode;
} wifi_ap_record_t;

typedef struct {
    struct {
        uint8_t ssid[32];
        uint8_t password[64];
        struct {
            uint8_t authmode;
        } threshold;
    } sta;
} wifi_config_t;

#define WIFI_SCAN_TYPE_ACTIVE 0
#define WIFI_IF_STA           0
#define WIFI_AUTH_WPA2_PSK    2

esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block);
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *number);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *number, wifi_ap_record_t *ap_records);
esp_err_t esp_wifi_set_config(int interface, const wifi_config_t *conf);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_connect(void);
