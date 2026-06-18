#include "f_coap.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "coap3/coap.h"
#include "pb_encode.h"
#include "pb_decode.h"
#include "espfm.pb.h"

static const char *TAG = "f_coap";

#define COAP_DEFAULT_PORT  5683

struct f_coap {
    coap_context_t     *ctx;
    coap_endpoint_t    *endpoint;
    bool                running;
    f_fan_handle_t      fan;
    f_source_handle_t   source;
    f_curve_handle_t    curve;
    f_schedule_handle_t schedule;
    f_config_handle_t   config;
};

/* ======================================================================== */
/*  Protobuf encode/decode helpers                                           */
/* ======================================================================== */

static bool encode_message(const void *msg, const pb_field_t fields[],
                           uint8_t *buf, size_t buf_len, size_t *out_len) {
    pb_ostream_t stream = pb_ostream_from_buffer(buf, buf_len);
    bool ok = pb_encode(&stream, fields, msg);
    if (ok) *out_len = stream.bytes_written;
    return ok;
}

static bool decode_message(const uint8_t *buf, size_t len,
                           void *msg, const pb_field_t fields[]) {
    pb_istream_t stream = pb_istream_from_buffer(buf, len);
    return pb_decode(&stream, fields, msg);
}

/* Convert registry structs to protobuf messages */

static void fan_to_pb(const f_fan_info_t *info, FanInfo *pb) {
    FanInfo tmp = FanInfo_init_zero;
    *pb = tmp;
    pb->id          = info->id;
    pb->mode        = (FanMode)info->mode;
    pb->duty        = info->duty;
    pb->rpm         = info->rpm;
    pb->enabled     = info->enabled;
    pb->inverted    = info->inverted;
    pb->pwm_gpio    = info->pwm_gpio;
    pb->tach_gpio   = info->tach_gpio;
    pb->source_id   = info->source_id;
    pb->curve_id    = info->curve_id;
    pb->schedule_id = info->schedule_id;
    pb->group_id    = info->group_id;
    pb->alarm       = (FanAlarm)info->alarm;
    strncpy(pb->name, info->name, sizeof(pb->name) - 1);
}

static void source_to_pb(const f_source_info_t *info, SourceInfo *pb) {
    SourceInfo tmp = SourceInfo_init_zero;
    *pb = tmp;
    pb->id     = info->id;
    pb->type   = (SourceType)info->type;
    pb->status = (SourceStatus)info->status;
    pb->temp_c = info->temp_c;
    pb->gpio   = info->gpio;
    strncpy(pb->name, info->name, sizeof(pb->name) - 1);
}

static void curve_to_pb(const f_curve_info_t *info, CurveInfo *pb) {
    CurveInfo tmp = CurveInfo_init_zero;
    *pb = tmp;
    pb->id = info->id;
    strncpy(pb->name, info->name, sizeof(pb->name) - 1);
    pb->points_count = info->num_points;
    for (int i = 0; i < info->num_points; i++) {
        pb->points[i].temp_c = info->points[i].temp_c;
        pb->points[i].duty   = info->points[i].duty;
    }
}

static void schedule_to_pb(const f_schedule_info_t *info, ScheduleInfo *pb) {
    ScheduleInfo tmp = ScheduleInfo_init_zero;
    *pb = tmp;
    pb->id         = info->id;
    pb->fan_id     = info->fan_id;
    pb->duty       = info->duty;
    pb->start_min  = info->start_min;
    pb->end_min    = info->end_min;
    pb->enabled    = info->enabled;
}

/* ======================================================================== */
/*  WiFi event handlers                                                      */
/* ======================================================================== */

static void _on_wifi_connected(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    f_coap_handle_t h = (f_coap_handle_t)arg;
    if (h) f_coap_start(h);
}

static void _on_wifi_disconnected(void *arg, esp_event_base_t base,
                                  int32_t id, void *data) {
    f_coap_handle_t h = (f_coap_handle_t)arg;
    if (h) f_coap_stop(h);
}

/* ======================================================================== */
/*  Public API                                                               */
/* ======================================================================== */

esp_err_t f_coap_init(f_coap_handle_t *handle, f_fan_handle_t fan,
                      f_source_handle_t source, f_curve_handle_t curve,
                      f_schedule_handle_t schedule, f_config_handle_t config) {
    if (!handle) return ESP_ERR_INVALID_ARG;

    f_coap_handle_t h = calloc(1, sizeof(struct f_coap));
    if (!h) return ESP_ERR_NO_MEM;

    h->fan      = fan;
    h->source   = source;
    h->curve    = curve;
    h->schedule = schedule;
    h->config   = config;

    esp_event_handler_register(ESPFM_EVENT, ESPFM_EVENT_WIFI_CONNECTED,
                               _on_wifi_connected, h);
    esp_event_handler_register(ESPFM_EVENT, ESPFM_EVENT_WIFI_DISCONNECTED,
                               _on_wifi_disconnected, h);

    *handle = h;
    ESP_LOGI(TAG, "CoAP server initialized (WiFi-aware)");
    return ESP_OK;
}

esp_err_t f_coap_start(f_coap_handle_t handle) {
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (handle->running) return ESP_OK;

    coap_startup();
    handle->ctx = coap_new_context(NULL);
    if (!handle->ctx) {
        ESP_LOGE(TAG, "Failed to create CoAP context");
        return ESP_FAIL;
    }

    coap_address_t addr;
    coap_address_init(&addr);
    addr.addr.sin.sin_family = AF_INET;
    addr.addr.sin.sin_port   = htons(COAP_DEFAULT_PORT);

    handle->endpoint = coap_new_endpoint(handle->ctx, &addr, COAP_PROTO_UDP);
    if (!handle->endpoint) {
        ESP_LOGE(TAG, "Failed to create CoAP endpoint");
        coap_free_context(handle->ctx);
        handle->ctx = NULL;
        return ESP_FAIL;
    }

    /* TODO Phase 2: Register resource handlers via coap_register_handler() */

    handle->running = true;
    ESP_LOGI(TAG, "CoAP server started on port %d", COAP_DEFAULT_PORT);
    return ESP_OK;
}

esp_err_t f_coap_stop(f_coap_handle_t handle) {
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (!handle->running) return ESP_OK;

    if (handle->endpoint) {
        coap_free_endpoint(handle->endpoint);
        handle->endpoint = NULL;
    }
    if (handle->ctx) {
        coap_free_context(handle->ctx);
        handle->ctx = NULL;
    }
    coap_cleanup();

    handle->running = false;
    ESP_LOGI(TAG, "CoAP server stopped");
    return ESP_OK;
}
