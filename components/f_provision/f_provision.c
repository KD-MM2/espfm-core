#include "f_provision.h"
#include "f_provision_http.h"
#include "f_provision_dns.h"
#include "f_core.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include <stdlib.h>
#include <stdbool.h>

static const char *TAG = "f_provision";

typedef enum {
    PROV_STATE_IDLE = 0,
    PROV_STATE_AP_ACTIVE,
    PROV_STATE_PROVISIONING,
} prov_state_t;

struct f_provision {
    prov_state_t state;
    f_wifi_handle_t wifi;
    esp_event_handler_instance_t event_instance;
    httpd_handle_t http_server;
    bool dns_running;
};

static esp_err_t _start_http_server(f_provision_handle_t handle) {
    if (handle->http_server != NULL) {
        return ESP_OK;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    esp_err_t err = httpd_start(&handle->http_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);

    err = f_provision_register_http_handlers(handle->http_server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP handler registration failed: %s", esp_err_to_name(err));
        httpd_stop(handle->http_server);
        handle->http_server = NULL;
        return err;
    }

    return ESP_OK;
}

static void _stop_http_server(f_provision_handle_t handle) {
    if (handle->http_server != NULL) {
        httpd_stop(handle->http_server);
        handle->http_server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}

static esp_err_t _start_dns_server(f_provision_handle_t handle) {
    esp_err_t err = f_provision_dns_start();
    if (err != ESP_OK) {
        return err;
    }
    handle->dns_running = true;
    return ESP_OK;
}

static void _stop_dns_server(f_provision_handle_t handle) {
    if (handle->dns_running) {
        f_provision_dns_stop();
        handle->dns_running = false;
    }
}

static esp_err_t _start_provisioning(f_provision_handle_t handle) {
    if (handle->state == PROV_STATE_PROVISIONING) {
        ESP_LOGD(TAG, "Already provisioning, skipping");
        return ESP_OK;
    }

    esp_err_t err = _start_http_server(handle);
    if (err != ESP_OK) {
        return err;
    }

    err = _start_dns_server(handle);
    if (err != ESP_OK) {
        _stop_http_server(handle);
        return err;
    }

    handle->state = PROV_STATE_PROVISIONING;
    ESP_LOGI(TAG, "Provisioning started");
    return ESP_OK;
}

static void _stop_provisioning(f_provision_handle_t handle) {
    _stop_http_server(handle);
    _stop_dns_server(handle);
    handle->state = PROV_STATE_IDLE;
    ESP_LOGI(TAG, "Provisioning stopped");
}

static void _event_handler(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data) {
    f_provision_handle_t handle = (f_provision_handle_t)arg;

    if (event_base == ESPFM_EVENT) {
        switch (event_id) {
        case ESPFM_EVENT_WIFI_STA_FAILED:
            ESP_LOGI(TAG, "STA connection failed, starting provisioning");
            {
                esp_err_t prov_err = _start_provisioning(handle);
                if (prov_err != ESP_OK) {
                    ESP_LOGE(TAG, "Auto-provisioning failed: %s", esp_err_to_name(prov_err));
                }
            }
            break;

        case ESPFM_EVENT_WIFI_CONNECTED:
            if (handle->state == PROV_STATE_PROVISIONING) {
                ESP_LOGI(TAG, "STA recovered, stopping provisioning");
                _stop_provisioning(handle);
            }
            break;

        default:
            break;
        }
    }
}

esp_err_t f_provision_init(f_provision_handle_t *handle, f_wifi_handle_t wifi) {
    if (handle == NULL || wifi == NULL) return ESP_ERR_INVALID_ARG;

    f_provision_handle_t h = calloc(1, sizeof(struct f_provision));
    if (h == NULL) return ESP_ERR_NO_MEM;

    h->wifi = wifi;
    h->state = PROV_STATE_IDLE;

    esp_err_t err = esp_event_handler_instance_register(
        ESPFM_EVENT, ESP_EVENT_ANY_ID, &_event_handler, h, &h->event_instance);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Event handler register failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    *handle = h;
    ESP_LOGI(TAG, "Provision module initialized");
    return ESP_OK;

cleanup:
    free(h);
    return err;
}

esp_err_t f_provision_start(f_provision_handle_t handle) {
    if (handle == NULL) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Manual provisioning trigger");
    return _start_provisioning(handle);
}

esp_err_t f_provision_stop(f_provision_handle_t handle) {
    if (handle == NULL) return ESP_ERR_INVALID_ARG;

    if (handle->state != PROV_STATE_PROVISIONING) {
        return ESP_OK;
    }

    _stop_provisioning(handle);
    return ESP_OK;
}
