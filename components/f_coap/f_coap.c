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
#define COAP_TASK_PRIO 4

static void coap_task(void *arg)
{
    struct f_coap *h = (struct f_coap *)arg;
    while (1) {
        xSemaphoreTake(h->start_sem, portMAX_DELAY);
        ESP_LOGI(TAG, "CoAP server task running");
        while (h->running) {
            int ret = coap_io_process(h->ctx, 1000);
            if (ret < 0) {
                ESP_LOGW(TAG, "coap_io_process returned %d", ret);
                break;
            }
        }
        ESP_LOGI(TAG, "CoAP server task paused");
    }
}

static void start_endpoint(struct f_coap *h)
{
    if (h->ep) return;
    coap_address_t listen_addr;
    coap_address_init(&listen_addr);
    listen_addr.addr.sin.sin_family = AF_INET;
    listen_addr.addr.sin.sin_port = htons(COAP_PORT);
    listen_addr.addr.sin.sin_addr.s_addr = INADDR_ANY;
    h->ep = coap_new_endpoint(h->ctx, &listen_addr, COAP_PROTO_UDP);
    if (!h->ep) { ESP_LOGE(TAG, "coap_new_endpoint failed"); return; }
    h->running = true;
    xSemaphoreGive(h->start_sem);
    ESP_LOGI(TAG, "CoAP endpoint started on port %d", COAP_PORT);
}

static void stop_endpoint(struct f_coap *h)
{
    if (!h->ep) return;
    h->running = false;
    coap_free_endpoint(h->ep);
    h->ep = NULL;
    ESP_LOGI(TAG, "CoAP endpoint stopped");
}

static void on_wifi_connected(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    struct f_coap *h = (struct f_coap *)arg;
    if (h) start_endpoint(h);
}

static void on_wifi_disconnected(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    struct f_coap *h = (struct f_coap *)arg;
    if (h) stop_endpoint(h);
}

static void on_ap_stop(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    struct f_coap *h = (struct f_coap *)arg;
    if (!h) return;
    ESP_LOGI(TAG, "AP stopped, restarting CoAP endpoint");
    stop_endpoint(h);
    start_endpoint(h);
}

esp_err_t f_coap_init(f_coap_handle_t *handle, f_fan_handle_t fan,
                      f_source_handle_t source, f_curve_handle_t curve,
                      f_schedule_handle_t schedule, f_config_handle_t config,
                      f_mdns_handle_t mdns)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    struct f_coap *h = calloc(1, sizeof(*h));
    if (!h) return ESP_ERR_NO_MEM;
    h->fan = fan; h->source = source; h->curve = curve;
    h->schedule = schedule; h->config = config; h->mdns = mdns;

    coap_startup();
    h->ctx = coap_new_context(NULL);
    if (!h->ctx) { free(h); return ESP_FAIL; }

    f_coap_register_resources(h->ctx, h);

    h->start_sem = xSemaphoreCreateBinary();
    if (!h->start_sem) { coap_free_context(h->ctx); free(h); return ESP_ERR_NO_MEM; }

    esp_event_handler_register(ESPFM_EVENT, ESPFM_EVENT_WIFI_CONNECTED, on_wifi_connected, h);
    esp_event_handler_register(ESPFM_EVENT, ESPFM_EVENT_WIFI_DISCONNECTED, on_wifi_disconnected, h);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STOP, on_ap_stop, h);

    BaseType_t ret = xTaskCreate(coap_task, "coap", COAP_TASK_STACK, h, COAP_TASK_PRIO, &h->task);
    if (ret != pdPASS) {
        vSemaphoreDelete(h->start_sem);
        coap_free_context(h->ctx);
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
    start_endpoint(handle);
    return ESP_OK;
}

esp_err_t f_coap_stop(f_coap_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    stop_endpoint(handle);
    return ESP_OK;
}

esp_err_t f_coap_deinit(f_coap_handle_t handle)
{
    if (!handle) return ESP_OK;
    stop_endpoint(handle);
    for (int i = 0; i < 50 && handle->running; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (handle->task) { vTaskDelete(handle->task); handle->task = NULL; }
    if (handle->start_sem) vSemaphoreDelete(handle->start_sem);
    if (handle->ctx) { coap_free_context(handle->ctx); handle->ctx = NULL; }
    coap_cleanup();
    free(handle);
    return ESP_OK;
}
