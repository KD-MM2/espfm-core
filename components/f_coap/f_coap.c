/* f_coap.c — CoAP+Protobuf server lifecycle (nanopb + microcoap) */
#include "f_coap.h"
#include "f_coap_internal.h"
#include "f_constraints.h"
#include "f_mdns.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "coap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>

static const char *TAG = "f_coap";
#define COAP_TASK_STACK 8192
#define COAP_TASK_PRIO 4

/* ---- CoAP server task ---- */
static void coap_task(void *arg)
{
    struct f_coap *h = (struct f_coap *)arg;

    static uint8_t rx[COAP_MTU], tx[COAP_MTU];

    ESP_LOGI(TAG, "CoAP server listening on port %d", COAP_PORT);

    while (h->running) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(h->sock, rx, COAP_MTU, 0,
                        (struct sockaddr *)&from, &from_len);
        if (n < 0) {
            ESP_LOGW(TAG, "recvfrom error: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (n < 4) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        ESP_LOGI(TAG, "Received %d bytes from %s:%d", n,
                 inet_ntoa(from.sin_addr), ntohs(from.sin_port));

        coap_packet_t inpkt, outpkt;
        if (coap_parse(&inpkt, rx, n) != 0) {
            ESP_LOGW(TAG, "coap_parse failed (%d bytes)", n);
            continue;
        }

        size_t out_len = COAP_MTU;
        f_coap_dispatch(h, &inpkt, tx, &out_len, &outpkt);
        ESP_LOGI(TAG, "dispatch done: out_len=%d code=0x%02x",
                 (int)out_len, outpkt.hdr.code);

        out_len = COAP_MTU;  /* reset to buffer capacity for coap_build */
        if (coap_build(tx, &out_len, &outpkt) == 0) {
            ESP_LOGI(TAG, "Sending %d bytes to %s:%d",
                     (int)out_len, inet_ntoa(from.sin_addr),
                     ntohs(from.sin_port));
            int sret = sendto(h->sock, tx, out_len, 0,
                              (struct sockaddr *)&from, from_len);
            if (sret < 0)
                ESP_LOGW(TAG, "sendto failed: errno=%d", errno);
            else
                ESP_LOGI(TAG, "sendto OK (%d bytes)", sret);
        } else {
            ESP_LOGW(TAG, "coap_build failed");
        }
    }
    h->task = NULL;
    vTaskDelete(NULL);
}

/* ---- WiFi event callbacks ---- */
static void on_wifi_connected(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    struct f_coap *h = (struct f_coap *)arg;
    if (h) f_coap_start(h);
}

static void on_wifi_disconnected(void *arg, esp_event_base_t base,
                                 int32_t id, void *data)
{
    struct f_coap *h = (struct f_coap *)arg;
    if (h) f_coap_stop(h);
}

static void on_ap_stop(void *arg, esp_event_base_t base,
                       int32_t id, void *data)
{
    struct f_coap *h = (struct f_coap *)arg;
    if (!h) return;
    ESP_LOGI(TAG, "AP stopped, restarting CoAP server on STA interface");
    f_coap_stop(h);
    f_coap_start(h);
}

/* ---- Public API ---- */
esp_err_t f_coap_init(f_coap_handle_t *handle, f_fan_handle_t fan,
                      f_source_handle_t source, f_curve_handle_t curve,
                      f_schedule_handle_t schedule, f_config_handle_t config,
                      f_mdns_handle_t mdns)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    /* calloc zeros task handle to NULL — required by f_coap_start guard */
    struct f_coap *h = calloc(1, sizeof(*h));
    if (!h) return ESP_ERR_NO_MEM;

    h->fan = fan;
    h->source = source;
    h->curve = curve;
    h->schedule = schedule;
    h->config = config;
    h->mdns = mdns;
    h->sock = -1;

    esp_event_handler_register(ESPFM_EVENT, ESPFM_EVENT_WIFI_CONNECTED,
                               on_wifi_connected, h);
    esp_event_handler_register(ESPFM_EVENT, ESPFM_EVENT_WIFI_DISCONNECTED,
                               on_wifi_disconnected, h);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STOP,
                               on_ap_stop, h);

    *handle = h;
    ESP_LOGI(TAG, "CoAP initialized");
    return ESP_OK;
}

esp_err_t f_coap_start(f_coap_handle_t handle)
{
    if (!handle || handle->running) return ESP_OK;
    if (handle->task != NULL) return ESP_OK;

    handle->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle->sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        return ESP_FAIL;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(COAP_PORT),
        .sin_addr.s_addr = INADDR_ANY
    };

    if (bind(handle->sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed");
        close(handle->sock);
        handle->sock = -1;
        return ESP_FAIL;
    }

    handle->running = true;
    BaseType_t ret = xTaskCreate(coap_task, "coap", COAP_TASK_STACK, handle,
                                 COAP_TASK_PRIO, &handle->task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate() failed");
        handle->running = false;
        handle->task = NULL;
        close(handle->sock);
        handle->sock = -1;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "CoAP server started on port %d", COAP_PORT);
    return ESP_OK;
}

esp_err_t f_coap_stop(f_coap_handle_t handle)
{
    if (!handle || !handle->running) return ESP_OK;

    handle->running = false;

    if (handle->sock >= 0) {
        close(handle->sock);
        handle->sock = -1;
    }

    /* Wait for coap_task to exit (up to 500ms) */
    for (int i = 0; i < 50; i++) {
        if (handle->task == NULL) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (handle->task != NULL) {
        ESP_LOGW(TAG, "CoAP task did not exit within timeout");
        handle->task = NULL;
    }

    ESP_LOGI(TAG, "CoAP server stopped");
    return ESP_OK;
}
