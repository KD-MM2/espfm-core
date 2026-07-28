/* f_coap.c — CoAP+Protobuf server lifecycle (libcoap-4) */
#include "f_coap.h"
#include "f_coap_internal.h"
#include "f_constraints.h"
#include "f_mdns.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include <coap3/coap.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "f_coap";
#define COAP_TASK_STACK 8192
#define COAP_TASK_PRIO  4

/*
 * ALL libcoap operations (coap_new_endpoint, coap_free_endpoint,
 * coap_io_process) run inside coap_task.  Event handlers only set
 * volatile flags — the task checks them after each coap_io_process()
 * returns.  This avoids the thread-safety violation that corrupted
 * libcoap's global state when stop_endpoint() was called from the
 * WiFi event handler while coap_io_process() was in progress.
 */

static void start_coap(struct f_coap *h)
{
    if (h->ep) return;
    if (!h->ctx) {
        h->ctx = coap_new_context(NULL);
        if (!h->ctx) {
            ESP_LOGE(TAG, "coap_new_context failed");
            return;
        }
        f_coap_register_resources(h->ctx, h);
    }
    coap_address_t listen_addr;
    coap_address_init(&listen_addr);
    listen_addr.addr.sin.sin_family      = AF_INET;
    listen_addr.addr.sin.sin_port        = htons(COAP_PORT);
    listen_addr.addr.sin.sin_addr.s_addr = INADDR_ANY;
    h->ep                                = coap_new_endpoint(h->ctx, &listen_addr, COAP_PROTO_UDP);
    if (h->ep) {
        h->running = true;
        ESP_LOGI(TAG, "CoAP endpoint started on port %d", COAP_PORT);
    } else {
        ESP_LOGE(TAG, "coap_new_endpoint failed");
    }
}

static void stop_coap(struct f_coap *h)
{
    if (!h->ep && !h->ctx) return;
    h->running = false;
    if (h->ep) {
        coap_free_endpoint(h->ep);
        h->ep = NULL;
    }
    if (h->ctx) {
        coap_free_context(h->ctx);
        h->ctx = NULL;
    }
    ESP_LOGI(TAG, "CoAP stopped (context freed)");
}

static void coap_task(void *arg)
{
    struct f_coap *h = (struct f_coap *)arg;
    while (1) {
        if (h->stop_requested) {
            h->stop_requested = false;
            stop_coap(h);
        }
        if (h->start_requested) {
            h->start_requested = false;
            start_coap(h);
        }

        if (h->running && h->ep && h->ctx) {
            int ret = coap_io_process(h->ctx, 100);
            if (ret < 0) {
                ESP_LOGW(TAG, "coap_io_process returned %d", ret);
                stop_coap(h);
                /* Auto-restart after error (e.g. AP_STOP socket invalidation) */
                h->start_requested = true;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

static void on_wifi_connected(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    struct f_coap *h = (struct f_coap *)arg;
    if (h) h->start_requested = true;
}

static void on_wifi_disconnected(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    struct f_coap *h = (struct f_coap *)arg;
    if (h) h->stop_requested = true;
}

static void on_ap_stop(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    struct f_coap *h = (struct f_coap *)arg;
    if (!h) return;
    ESP_LOGI(TAG, "AP stopped, will restart CoAP endpoint");
    h->stop_requested  = true;
    h->start_requested = true;
}

esp_err_t f_coap_init(f_coap_handle_t *handle, f_fan_handle_t fan, f_source_handle_t source,
                      f_curve_handle_t curve, f_schedule_handle_t schedule,
                      f_config_handle_t config, f_mdns_handle_t mdns,
                      f_ds18b20_handle_t *ds18b20_ref)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    struct f_coap *h = calloc(1, sizeof(*h));
    if (!h) return ESP_ERR_NO_MEM;
    h->fan         = fan;
    h->source      = source;
    h->curve       = curve;
    h->schedule    = schedule;
    h->config      = config;
    h->mdns        = mdns;
    h->ds18b20_ref = ds18b20_ref;

    coap_startup();

    esp_event_handler_register(ESPFM_EVENT, ESPFM_EVENT_WIFI_CONNECTED, on_wifi_connected, h);
    esp_event_handler_register(ESPFM_EVENT, ESPFM_EVENT_WIFI_DISCONNECTED, on_wifi_disconnected, h);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STOP, on_ap_stop, h);

    BaseType_t ret = xTaskCreate(coap_task, "coap", COAP_TASK_STACK, h, COAP_TASK_PRIO, &h->task);
    if (ret != pdPASS) {
        free(h);
        return ESP_ERR_NO_MEM;
    }

    *handle = h;
    ESP_LOGI(TAG, "CoAP initialized (libcoap)");
    return ESP_OK;
}

esp_err_t f_coap_start(f_coap_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    handle->start_requested = true;
    return ESP_OK;
}

esp_err_t f_coap_stop(f_coap_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    handle->stop_requested = true;
    return ESP_OK;
}

esp_err_t f_coap_deinit(f_coap_handle_t handle)
{
    if (!handle) return ESP_OK;
    handle->stop_requested = true;
    /* Wait for task to process stop (up to 500ms) */
    for (int i = 0; i < 50 && (handle->ep || handle->ctx); i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (handle->task) {
        vTaskDelete(handle->task);
        handle->task = NULL;
    }
    coap_cleanup();
    free(handle);
    return ESP_OK;
}
