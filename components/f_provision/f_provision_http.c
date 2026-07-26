#include "f_provision_http.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "f_provision_http";

#define HTTP_RESP_BUF_SIZE  4096
#define HTTP_BODY_MAX_SIZE  256
#define WIFI_SSID_MAX_LEN   32
#define WIFI_PASS_MAX_LEN   64

/* ------------------------------------------------------------------ */
/*  GET /  — serve captive portal HTML from LittleFS                   */
/* ------------------------------------------------------------------ */

static esp_err_t _handle_root(httpd_req_t *req) {
    FILE *f = fopen("/littlefs/www/index.html", "r");
    if (f == NULL) {
        ESP_LOGW(TAG, "index.html not found");
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");

    char buf[1024];
    size_t bytes_read;
    while ((bytes_read = fread(buf, 1, sizeof(buf), f)) > 0) {
        esp_err_t err = httpd_resp_send_chunk(req, buf, (ssize_t)bytes_read);
        if (err != ESP_OK) {
            fclose(f);
            return err;
        }
    }
    fclose(f);

    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ------------------------------------------------------------------ */
/*  GET /scan  — blocking scan in APSTA mode, return JSON              */
/* ------------------------------------------------------------------ */

static const char *_auth_to_str(wifi_auth_mode_t auth) {
    switch (auth) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA2";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA3";
    default:                        return "UNKNOWN";
    }
}

static esp_err_t _handle_scan(httpd_req_t *req) {
    ESP_LOGI(TAG, "GET /scan — blocking scan in APSTA mode");

    wifi_scan_config_t scan_cfg = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true); /* blocking */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "[]", 2);
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    ESP_LOGI(TAG, "Scan complete, found %d networks", ap_count);

    if (ap_count == 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "[]", 2);
    }

    wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (ap_records == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    esp_wifi_scan_get_ap_records(&ap_count, ap_records);

    for (uint16_t i = 0; i < ap_count; i++) {
        ESP_LOGI(TAG, "  [%d] ssid='%s' ch=%d rssi=%d auth=%s",
                 i, ap_records[i].ssid, ap_records[i].primary,
                 ap_records[i].rssi, _auth_to_str(ap_records[i].authmode));
    }

    char *json = calloc(1, HTTP_RESP_BUF_SIZE);
    if (json == NULL) {
        free(ap_records);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    size_t pos = 0;
    pos += (size_t)snprintf(json + pos, HTTP_RESP_BUF_SIZE - pos, "[");

    for (uint16_t i = 0; i < ap_count && pos < HTTP_RESP_BUF_SIZE - 64; i++) {
        if (i > 0) {
            json[pos++] = ',';
        }
        pos += (size_t)snprintf(json + pos, HTTP_RESP_BUF_SIZE - pos,
                                "{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":\"%s\",\"ch\":%d}",
                                (const char *)ap_records[i].ssid,
                                ap_records[i].rssi,
                                _auth_to_str(ap_records[i].authmode),
                                ap_records[i].primary);
    }

    json[pos++] = ']';
    json[pos] = '\0';

    free(ap_records);

    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_send(req, json, (ssize_t)pos);
    free(json);
    return send_err;
}

/* ------------------------------------------------------------------ */
/*  POST /connect  — save WiFi credentials, restart device             */
/* ------------------------------------------------------------------ */

static const char *_json_get_str(const char *body, const char *key,
                                  size_t *len_out) {
    size_t key_len = strlen(key);
    const char *p = body;
    while ((p = strstr(p, "\"")) != NULL) {
        p++;
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '"') {
            const char *colon = strchr(p + key_len, ':');
            if (colon == NULL) return NULL;
            colon++;
            while (*colon == ' ' || *colon == '\t') colon++;
            if (*colon != '"') return NULL;
            colon++;
            const char *end = strchr(colon, '"');
            if (end == NULL) return NULL;
            *len_out = (size_t)(end - colon);
            return colon;
        }
    }
    return NULL;
}

static esp_err_t _handle_connect(httpd_req_t *req) {
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > HTTP_BODY_MAX_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_ERR_INVALID_ARG;
    }

    char *body = calloc(1, (size_t)total_len + 1);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, body + received, (size_t)(total_len - received));
        if (ret <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read error");
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    size_t ssid_len = 0;
    const char *ssid_val = _json_get_str(body, "ssid", &ssid_len);
    if (ssid_val == NULL || ssid_len == 0) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid");
        return ESP_ERR_INVALID_ARG;
    }

    size_t pass_len = 0;
    const char *pass_val = _json_get_str(body, "password", &pass_len);
    if (pass_val == NULL) {
        pass_val = "";
        pass_len = 0;
    }

    if (ssid_len > WIFI_SSID_MAX_LEN) ssid_len = WIFI_SSID_MAX_LEN;
    if (pass_len > WIFI_PASS_MAX_LEN) pass_len = WIFI_PASS_MAX_LEN;

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid_val, ssid_len);
    sta_config.sta.ssid[ssid_len] = '\0';
    if (pass_len > 0) {
        strncpy((char *)sta_config.sta.password, pass_val, pass_len);
        sta_config.sta.password[pass_len] = '\0';
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save WiFi config: %s", esp_err_to_name(err));
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Config save failed");
        return err;
    }

    ESP_LOGI(TAG, "WiFi credentials saved for SSID: %s", sta_config.sta.ssid);
    free(body);

    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_send(req, "{\"status\":\"ok\"}", 14);

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return send_err;
}

/* ------------------------------------------------------------------ */
/*  Registration                                                       */
/* ------------------------------------------------------------------ */

esp_err_t f_provision_register_http_handlers(httpd_handle_t server) {
    if (server == NULL) return ESP_ERR_INVALID_ARG;

    static const httpd_uri_t uri_root = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = _handle_root,
        .user_ctx = NULL,
    };

    static const httpd_uri_t uri_scan = {
        .uri      = "/scan",
        .method   = HTTP_GET,
        .handler  = _handle_scan,
        .user_ctx = NULL,
    };

    static const httpd_uri_t uri_connect = {
        .uri      = "/connect",
        .method   = HTTP_POST,
        .handler  = _handle_connect,
        .user_ctx = NULL,
    };

    esp_err_t err;

    err = httpd_register_uri_handler(server, &uri_root);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register GET /: %s", esp_err_to_name(err));
        return err;
    }

    err = httpd_register_uri_handler(server, &uri_scan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register GET /scan: %s", esp_err_to_name(err));
        return err;
    }

    err = httpd_register_uri_handler(server, &uri_connect);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register POST /connect: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "HTTP handlers registered (/, /scan, /connect)");
    return ESP_OK;
}
