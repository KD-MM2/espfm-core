/* f_coap_routes.c — CoAP route dispatch table and resource handlers */
#include "f_coap_internal.h"
#include "f_fan.h"
#include "f_source.h"
#include "f_curve.h"
#include "f_schedule.h"
#include "f_config.h"
#include "f_mdns.h"
#include "f_constraints.h"
#include "espfm_conv.h"
#include "espfm.pb.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_system.h"
#include <coap3/coap.h>
#include "pb_encode.h"
#include "pb_decode.h"
#include <string.h>
#include <stdlib.h>

__attribute__((unused)) static const char *TAG = "f_coap_routes";

#define COAP_ENC_BUF_SIZE 4096

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static void parse_uri(const coap_packet_t *pkt, coap_req_ctx_t *ctx)
{
    uint8_t count = 0;
    const coap_option_t *opt = coap_findOptions(pkt, COAP_OPTION_URI_PATH, &count);
    ctx->nseg = (count > COAP_MAX_SEG) ? COAP_MAX_SEG : (int)count;
    for (int i = 0; i < ctx->nseg; i++) {
        int l = opt[i].buf.len;
        if (l > COAP_MAX_SEG_LEN - 1) l = COAP_MAX_SEG_LEN - 1;
        memcpy(ctx->seg[i], opt[i].buf.p, l);
        ctx->seg[i][l] = '\0';
    }
    ctx->id = (ctx->nseg >= 2) ? (uint32_t)atoi(ctx->seg[1]) : 0;
}

static void save_config(struct f_coap *h)
{
    if (h->config)
        f_config_save_all(h->config, h->fan, h->source, h->curve, h->schedule);
}

/* ------------------------------------------------------------------ */
/*  Fan handlers:  /fans                                               */
/* ------------------------------------------------------------------ */

static void handle_fan_list(struct f_coap *h, coap_req_ctx_t *ctx)
{
    static FanList list;
    list = (FanList)FanList_init_default;
    for (uint8_t i = 0; i < 8; i++) {
        f_fan_info_t fi;
        if (f_fan_get_info(h->fan, i, &fi) == ESP_OK)
            f_coap_fan_to_pb(&fi, &list.fans[list.fans_count++]);
    }
    ctx->rsp_msg = &list;
    ctx->rsp_desc = &FanList_msg;
}

static void handle_fan_get(struct f_coap *h, coap_req_ctx_t *ctx)
{
    f_fan_info_t fi;
    if (f_fan_get_info(h->fan, ctx->id, &fi) != ESP_OK) {
        ctx->rsp_code = COAP_RSPCODE_NOT_FOUND;
        return;
    }
    static FanInfo pb;
    f_coap_fan_to_pb(&fi, &pb);
    ctx->rsp_msg = &pb;
    ctx->rsp_desc = &FanInfo_msg;
}

static void handle_fan_create(struct f_coap *h, coap_req_ctx_t *ctx)
{
    FanCreateRequest cr = FanCreateRequest_init_default;
    pb_istream_t stream = pb_istream_from_buffer(ctx->payload, ctx->payload_len);
    if (!pb_decode(&stream, &FanCreateRequest_msg, &cr)) {
        ctx->rsp_code = COAP_RSPCODE_BAD_REQUEST;
        return;
    }
    uint8_t nid;
    if (f_fan_add(h->fan, (uint8_t)cr.pwm_gpio, (uint8_t)cr.tach_gpio, cr.name, &nid) != ESP_OK) {
        ctx->rsp_code = COAP_RSPCODE_BAD_REQUEST;
        return;
    }
    save_config(h);
    f_fan_info_t fi;
    f_fan_get_info(h->fan, nid, &fi);
    static FanInfo pb;
    f_coap_fan_to_pb(&fi, &pb);
    ctx->rsp_msg = &pb;
    ctx->rsp_desc = &FanInfo_msg;
    ctx->rsp_code = MAKE_RSPCODE(2, 1);
}

static void handle_fan_update(struct f_coap *h, coap_req_ctx_t *ctx)
{
    FanUpdateRequest ur = FanUpdateRequest_init_default;
    pb_istream_t stream = pb_istream_from_buffer(ctx->payload, ctx->payload_len);
    if (!pb_decode(&stream, &FanUpdateRequest_msg, &ur)) {
        ctx->rsp_code = COAP_RSPCODE_BAD_REQUEST;
        return;
    }
    f_fan_info_t fi;
    if (f_fan_get_info(h->fan, ctx->id, &fi) != ESP_OK) {
        ctx->rsp_code = COAP_RSPCODE_NOT_FOUND;
        return;
    }
    /* Apply only fields that were sent (has_ flags from proto3 optional) */
    if (ur.has_mode)       f_fan_set_mode(h->fan, ctx->id, (fan_mode_t)ur.mode);
    if (ur.has_duty)       f_fan_set_duty(h->fan, ctx->id, (uint8_t)ur.duty);
    if (ur.has_source_id)  f_fan_set_source(h->fan, ctx->id, (uint8_t)ur.source_id);
    if (ur.has_curve_id)   f_fan_set_curve(h->fan, ctx->id, (uint8_t)ur.curve_id);
    if (ur.has_schedule_id) f_fan_set_schedule(h->fan, ctx->id, (uint8_t)ur.schedule_id);
    if (ur.has_group_id)   f_fan_set_group(h->fan, ctx->id, (uint8_t)ur.group_id);
    if (ur.has_inverted)   f_fan_set_inverted(h->fan, ctx->id, ur.inverted);
    save_config(h);
    f_fan_get_info(h->fan, ctx->id, &fi);
    static FanInfo pb;
    f_coap_fan_to_pb(&fi, &pb);
    ctx->rsp_msg = &pb;
    ctx->rsp_desc = &FanInfo_msg;
    ctx->rsp_code = MAKE_RSPCODE(2, 4);
}

static void handle_fan_delete(struct f_coap *h, coap_req_ctx_t *ctx)
{
    if (f_fan_remove(h->fan, ctx->id) != ESP_OK) {
        ctx->rsp_code = COAP_RSPCODE_NOT_FOUND;
        return;
    }
    save_config(h);
    static StatusResponse sr;
    sr = (StatusResponse)StatusResponse_init_default;
    sr.ok = true;
    ctx->rsp_msg = &sr;
    ctx->rsp_desc = &StatusResponse_msg;
    ctx->rsp_code = MAKE_RSPCODE(2, 2);
}

/* ------------------------------------------------------------------ */
/*  Source handlers: /sources                                          */
/* ------------------------------------------------------------------ */

static void handle_source_list(struct f_coap *h, coap_req_ctx_t *ctx)
{
    /* GET /sources – list all sources */
    static SourceList list;
    list = (SourceList)SourceList_init_default;
    for (uint8_t i = 0; i < 8; i++) {
        f_source_info_t si;
        if (f_source_get_info(h->source, i, &si) == ESP_OK)
            f_coap_source_to_pb(&si, &list.sources[list.sources_count++]);
    }
    ctx->rsp_msg = &list;
    ctx->rsp_desc = &SourceList_msg;
}

static void handle_source_get(struct f_coap *h, coap_req_ctx_t *ctx)
{
    f_source_info_t si;
    if (f_source_get_info(h->source, ctx->id, &si) != ESP_OK) {
        ctx->rsp_code = COAP_RSPCODE_NOT_FOUND;
        return;
    }
    static SourceInfo pb;
    f_coap_source_to_pb(&si, &pb);
    ctx->rsp_msg = &pb;
    ctx->rsp_desc = &SourceInfo_msg;
}

static void handle_source_temp(struct f_coap *h, coap_req_ctx_t *ctx)
{
    ManualTempRequest req = ManualTempRequest_init_default;
    pb_istream_t stream = pb_istream_from_buffer(ctx->payload, ctx->payload_len);
    if (!pb_decode(&stream, &ManualTempRequest_msg, &req)) {
        ctx->rsp_code = COAP_RSPCODE_BAD_REQUEST;
        return;
    }
    {
        f_source_info_t si_check;
        if (f_source_get_info(h->source, (uint8_t)req.id, &si_check) != ESP_OK) {
            ctx->rsp_code = COAP_RSPCODE_NOT_FOUND;
            return;
        }
    }
    if (f_source_update_manual(h->source, (uint8_t)req.id, req.temp_c) != ESP_OK) {
        ctx->rsp_code = COAP_RSPCODE_BAD_REQUEST;
        return;
    }
    static StatusResponse sr;
    sr = (StatusResponse)StatusResponse_init_default;
    sr.ok = true;
    ctx->rsp_msg = &sr;
    ctx->rsp_desc = &StatusResponse_msg;
    ctx->rsp_code = MAKE_RSPCODE(2, 4);
}

static void handle_source_create(struct f_coap *h, coap_req_ctx_t *ctx)
{
    SourceCreateRequest cr = SourceCreateRequest_init_default;
    pb_istream_t stream = pb_istream_from_buffer(ctx->payload, ctx->payload_len);
    if (!pb_decode(&stream, &SourceCreateRequest_msg, &cr)) {
        ctx->rsp_code = COAP_RSPCODE_BAD_REQUEST;
        return;
    }
    uint8_t nid;
    if (f_source_add(h->source, pb_to_source_type(cr.type), (uint8_t)cr.gpio,
                     cr.name, &nid) != ESP_OK) {
        ctx->rsp_code = COAP_RSPCODE_BAD_REQUEST;
        return;
    }
    save_config(h);
    f_source_info_t si;
    f_source_get_info(h->source, nid, &si);
    static SourceInfo pb;
    f_coap_source_to_pb(&si, &pb);
    ctx->rsp_msg = &pb;
    ctx->rsp_desc = &SourceInfo_msg;
    ctx->rsp_code = MAKE_RSPCODE(2, 1);
}

static void handle_source_delete(struct f_coap *h, coap_req_ctx_t *ctx)
{
    if (f_source_remove(h->source, ctx->id) != ESP_OK) {
        ctx->rsp_code = COAP_RSPCODE_NOT_FOUND;
        return;
    }
    save_config(h);
    static StatusResponse sr;
    sr = (StatusResponse)StatusResponse_init_default;
    sr.ok = true;
    ctx->rsp_msg = &sr;
    ctx->rsp_desc = &StatusResponse_msg;
    ctx->rsp_code = MAKE_RSPCODE(2, 2);
}

/* ------------------------------------------------------------------ */
/*  Curve handlers: /curves                                            */
/* ------------------------------------------------------------------ */

static void handle_curve_list(struct f_coap *h, coap_req_ctx_t *ctx)
{
    static CurveList list;
    list = (CurveList)CurveList_init_default;
    for (uint8_t i = 0; i < 16; i++) {
        f_curve_info_t ci;
        if (f_curve_get_info(h->curve, i, &ci) == ESP_OK)
            f_coap_curve_to_pb(&ci, &list.curves[list.curves_count++]);
    }
    ctx->rsp_msg = &list;
    ctx->rsp_desc = &CurveList_msg;
}

static void handle_curve_get(struct f_coap *h, coap_req_ctx_t *ctx)
{
    f_curve_info_t ci;
    if (f_curve_get_info(h->curve, ctx->id, &ci) != ESP_OK) {
        ctx->rsp_code = COAP_RSPCODE_NOT_FOUND;
        return;
    }
    static CurveInfo pb;
    f_coap_curve_to_pb(&ci, &pb);
    ctx->rsp_msg = &pb;
    ctx->rsp_desc = &CurveInfo_msg;
}

static void handle_curve_create(struct f_coap *h, coap_req_ctx_t *ctx)
{
    CurveCreateRequest cr = CurveCreateRequest_init_default;
    pb_istream_t stream = pb_istream_from_buffer(ctx->payload, ctx->payload_len);
    if (!pb_decode(&stream, &CurveCreateRequest_msg, &cr)) {
        ctx->rsp_code = COAP_RSPCODE_BAD_REQUEST;
        return;
    }
    f_curve_info_t ci = { 0 };
    strncpy(ci.name, cr.name, sizeof(ci.name) - 1);
    ci.name[sizeof(ci.name) - 1] = '\0';
    ci.num_points = (uint8_t)cr.points_count;
    for (int i = 0; i < cr.points_count && i < F_CURVE_MAX_POINTS; i++) {
        ci.points[i].temp_c = cr.points[i].temp_c;
        ci.points[i].duty = (uint8_t)cr.points[i].duty;
    }
    uint8_t nid;
    if (f_curve_upsert(h->curve, &ci, &nid) != ESP_OK) {
        ctx->rsp_code = COAP_RSPCODE_BAD_REQUEST;
        return;
    }
    save_config(h);
    f_curve_get_info(h->curve, nid, &ci);
    static CurveInfo pb;
    f_coap_curve_to_pb(&ci, &pb);
    ctx->rsp_msg = &pb;
    ctx->rsp_desc = &CurveInfo_msg;
    ctx->rsp_code = MAKE_RSPCODE(2, 1);
}

static void handle_curve_update(struct f_coap *h, coap_req_ctx_t *ctx)
{
    CurveUpdateRequest ur = CurveUpdateRequest_init_default;
    pb_istream_t stream = pb_istream_from_buffer(ctx->payload, ctx->payload_len);
    if (!pb_decode(&stream, &CurveUpdateRequest_msg, &ur)) {
        ctx->rsp_code = COAP_RSPCODE_BAD_REQUEST;
        return;
    }
    f_curve_info_t ci = { 0 };
    ci.id = (uint8_t)ctx->id;
    strncpy(ci.name, ur.name, sizeof(ci.name) - 1);
    ci.name[sizeof(ci.name) - 1] = '\0';
    ci.num_points = (uint8_t)ur.points_count;
    for (int i = 0; i < ur.points_count && i < F_CURVE_MAX_POINTS; i++) {
        ci.points[i].temp_c = ur.points[i].temp_c;
        ci.points[i].duty = (uint8_t)ur.points[i].duty;
    }
    uint8_t oid;
    if (f_curve_upsert(h->curve, &ci, &oid) != ESP_OK) {
        ctx->rsp_code = COAP_RSPCODE_BAD_REQUEST;
        return;
    }
    save_config(h);
    f_curve_get_info(h->curve, oid, &ci);
    static CurveInfo pb;
    f_coap_curve_to_pb(&ci, &pb);
    ctx->rsp_msg = &pb;
    ctx->rsp_desc = &CurveInfo_msg;
    ctx->rsp_code = MAKE_RSPCODE(2, 4);
}

static void handle_curve_delete(struct f_coap *h, coap_req_ctx_t *ctx)
{
    if (f_curve_remove(h->curve, ctx->id) != ESP_OK) {
        ctx->rsp_code = COAP_RSPCODE_NOT_FOUND;
        return;
    }
    save_config(h);
    static StatusResponse sr;
    sr = (StatusResponse)StatusResponse_init_default;
    sr.ok = true;
    ctx->rsp_msg = &sr;
    ctx->rsp_desc = &StatusResponse_msg;
    ctx->rsp_code = MAKE_RSPCODE(2, 2);
}

/* ------------------------------------------------------------------ */
/*  Schedule handlers: /schedules                                      */
/* ------------------------------------------------------------------ */

static void handle_schedule_list(struct f_coap *h, coap_req_ctx_t *ctx)
{
    static ScheduleList list;
    list = (ScheduleList)ScheduleList_init_default;
    for (uint8_t i = 0; i < 8; i++) {
        f_schedule_info_t si;
        if (f_schedule_get_info(h->schedule, i, &si) == ESP_OK)
            f_coap_schedule_to_pb(&si, &list.schedules[list.schedules_count++]);
    }
    ctx->rsp_msg = &list;
    ctx->rsp_desc = &ScheduleList_msg;
}

static void handle_schedule_create(struct f_coap *h, coap_req_ctx_t *ctx)
{
    ScheduleCreateRequest cr = ScheduleCreateRequest_init_default;
    pb_istream_t stream = pb_istream_from_buffer(ctx->payload, ctx->payload_len);
    if (!pb_decode(&stream, &ScheduleCreateRequest_msg, &cr)) {
        ctx->rsp_code = COAP_RSPCODE_BAD_REQUEST;
        return;
    }
    f_schedule_info_t si = {
        .fan_id    = (uint8_t)cr.fan_id,
        .duty      = (uint8_t)cr.duty,
        .start_min = (uint16_t)cr.start_min,
        .end_min   = (uint16_t)cr.end_min,
        .enabled   = cr.enabled
    };
    uint8_t nid;
    if (f_schedule_add(h->schedule, &si, &nid) != ESP_OK) {
        ctx->rsp_code = COAP_RSPCODE_BAD_REQUEST;
        return;
    }
    save_config(h);
    f_schedule_get_info(h->schedule, nid, &si);
    static ScheduleInfo pb;
    f_coap_schedule_to_pb(&si, &pb);
    ctx->rsp_msg = &pb;
    ctx->rsp_desc = &ScheduleInfo_msg;
    ctx->rsp_code = MAKE_RSPCODE(2, 1);
}

static void handle_schedule_update(struct f_coap *h, coap_req_ctx_t *ctx)
{
    ScheduleUpdateRequest ur = ScheduleUpdateRequest_init_default;
    pb_istream_t stream = pb_istream_from_buffer(ctx->payload, ctx->payload_len);
    if (!pb_decode(&stream, &ScheduleUpdateRequest_msg, &ur)) {
        ctx->rsp_code = COAP_RSPCODE_BAD_REQUEST;
        return;
    }
    f_schedule_info_t si;
    if (f_schedule_get_info(h->schedule, ctx->id, &si) != ESP_OK) {
        ctx->rsp_code = COAP_RSPCODE_NOT_FOUND;
        return;
    }
    /* Apply only fields that were sent */
    if (ur.has_fan_id)    si.fan_id    = (uint8_t)ur.fan_id;
    if (ur.has_duty)      si.duty      = (uint8_t)ur.duty;
    if (ur.has_start_min) si.start_min = (uint16_t)ur.start_min;
    if (ur.has_end_min)   si.end_min   = (uint16_t)ur.end_min;
    if (ur.has_enabled)   si.enabled   = ur.enabled;
    if (f_schedule_update(h->schedule, ctx->id, &si) != ESP_OK) {
        ctx->rsp_code = COAP_RSPCODE_NOT_FOUND;
        return;
    }
    save_config(h);
    f_schedule_get_info(h->schedule, ctx->id, &si);
    static ScheduleInfo pb;
    f_coap_schedule_to_pb(&si, &pb);
    ctx->rsp_msg = &pb;
    ctx->rsp_desc = &ScheduleInfo_msg;
    ctx->rsp_code = MAKE_RSPCODE(2, 4);
}

static void handle_schedule_delete(struct f_coap *h, coap_req_ctx_t *ctx)
{
    if (f_schedule_remove(h->schedule, ctx->id) != ESP_OK) {
        ctx->rsp_code = COAP_RSPCODE_NOT_FOUND;
        return;
    }
    save_config(h);
    static StatusResponse sr;
    sr = (StatusResponse)StatusResponse_init_default;
    sr.ok = true;
    ctx->rsp_msg = &sr;
    ctx->rsp_desc = &StatusResponse_msg;
    ctx->rsp_code = MAKE_RSPCODE(2, 2);
}

/* ------------------------------------------------------------------ */
/*  WiFi handler: /wifi/{scan,connect,status}                          */
/* ------------------------------------------------------------------ */

static void handle_wifi(struct f_coap *h, coap_req_ctx_t *ctx)
{
    if (ctx->nseg < 2) {
        ctx->rsp_code = COAP_RSPCODE_NOT_FOUND;
        return;
    }

    /* GET /wifi/scan */
    if (strcmp(ctx->seg[1], "scan") == 0) {
        wifi_scan_config_t sc = {
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
            .scan_time.active.min = 100,
            .scan_time.active.max = 300
        };
        if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
            ctx->rsp_code = MAKE_RSPCODE(5, 3);
            return;
        }
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        static WifiScanResult sr;
        sr = (WifiScanResult)WifiScanResult_init_default;
        if (n > 0) {
            wifi_ap_record_t *aps = calloc(n, sizeof(wifi_ap_record_t));
            if (aps) {
                if (esp_wifi_scan_get_ap_records(&n, aps) == ESP_OK) {
                    for (uint16_t i = 0; i < n && i < 16; i++) {
                        WifiApRecord *ap = &sr.aps[sr.aps_count++];
                        strncpy(ap->ssid, (const char *)aps[i].ssid, sizeof(ap->ssid) - 1);
                        ap->ssid[sizeof(ap->ssid) - 1] = '\0';
                        ap->rssi = aps[i].rssi;
                        ap->channel = aps[i].primary;
                        ap->authmode = aps[i].authmode;
                    }
                }
                free(aps);
            }
        }
        ctx->rsp_msg = &sr;
        ctx->rsp_desc = &WifiScanResult_msg;
        return;
    }

    /* POST /wifi/connect */
    if (strcmp(ctx->seg[1], "connect") == 0) {
        WifiConnectRequest cr = WifiConnectRequest_init_default;
        pb_istream_t stream = pb_istream_from_buffer(ctx->payload, ctx->payload_len);
        if (!pb_decode(&stream, &WifiConnectRequest_msg, &cr)) {
            ctx->rsp_code = COAP_RSPCODE_BAD_REQUEST;
            return;
        }
        wifi_config_t wc = { 0 };
        strncpy((char *)wc.sta.ssid, cr.ssid, sizeof(wc.sta.ssid) - 1);
        strncpy((char *)wc.sta.password, cr.password, sizeof(wc.sta.password) - 1);
        wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) {
            ctx->rsp_code = MAKE_RSPCODE(5, 3);
            return;
        }
        esp_wifi_disconnect();
        esp_wifi_connect();
        static StatusResponse sr;
        sr = (StatusResponse)StatusResponse_init_default;
        sr.ok = true;
        ctx->rsp_msg = &sr;
        ctx->rsp_desc = &StatusResponse_msg;
        ctx->rsp_code = MAKE_RSPCODE(2, 4);
        return;
    }

    /* GET /wifi/status */
    if (strcmp(ctx->seg[1], "status") == 0) {
        static WifiStatus ws;
        ws = (WifiStatus)WifiStatus_init_default;
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta) {
            esp_netif_ip_info_t ip;
            if (esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) {
                ws.sta_connected = true;
                snprintf(ws.sta_ip, sizeof(ws.sta_ip), IPSTR, IP2STR(&ip.ip));
            }
        }
        strncpy(ws.ap_ip, "192.168.4.1", sizeof(ws.ap_ip) - 1);
        ws.ap_ip[sizeof(ws.ap_ip) - 1] = '\0';
        ctx->rsp_msg = &ws;
        ctx->rsp_desc = &WifiStatus_msg;
        return;
    }

    ctx->rsp_code = COAP_RSPCODE_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/*  System handler: /system/info                                       */
/* ------------------------------------------------------------------ */

static void handle_system_put(struct f_coap *h, coap_req_ctx_t *ctx)
{
    if (ctx->nseg < 2 || strcmp(ctx->seg[1], "hostname") != 0) {
        ctx->rsp_code = COAP_RSPCODE_NOT_FOUND;
        return;
    }
    HostnameRequest req = HostnameRequest_init_default;
    pb_istream_t stream = pb_istream_from_buffer(ctx->payload, ctx->payload_len);
    if (!pb_decode(&stream, &HostnameRequest_msg, &req)) {
        ctx->rsp_code = COAP_RSPCODE_BAD_REQUEST;
        return;
    }
    esp_err_t err = f_mdns_set_hostname(req.hostname);
    if (err != ESP_OK) {
        ctx->rsp_code = COAP_RSPCODE_BAD_REQUEST;
        return;
    }
    static StatusResponse sr;
    sr = (StatusResponse)StatusResponse_init_default;
    sr.ok = true;
    ctx->rsp_msg = &sr;
    ctx->rsp_desc = &StatusResponse_msg;
    ctx->rsp_code = MAKE_RSPCODE(2, 4);  /* 2.04 Changed */
}

static void handle_system(struct f_coap *h, coap_req_ctx_t *ctx)
{
    if (ctx->nseg < 2 || strcmp(ctx->seg[1], "info") != 0) {
        ctx->rsp_code = COAP_RSPCODE_NOT_FOUND;
        return;
    }
    static SystemInfo si;
    si = (SystemInfo)SystemInfo_init_default;
    snprintf(si.version, sizeof(si.version), "%d.%d.%d",
             ESPFM_VERSION_MAJOR, ESPFM_VERSION_MINOR, ESPFM_VERSION_PATCH);
    si.uptime_s    = (uint32_t)(esp_timer_get_time() / 1000000);
    si.heap_free   = esp_get_free_heap_size();
    si.fan_count     = h->fan     ? f_fan_get_count(h->fan)         : 0;
    si.source_count  = h->source  ? f_source_get_count(h->source)   : 0;
    si.curve_count   = h->curve   ? f_curve_get_count(h->curve)     : 0;
    si.schedule_count = h->schedule ? f_schedule_get_count(h->schedule) : 0;
    const char *hostname = f_mdns_get_hostname(h->mdns);
    if (hostname) {
        strncpy(si.hostname, hostname, sizeof(si.hostname) - 1);
        si.hostname[sizeof(si.hostname) - 1] = '\0';
    }
    ctx->rsp_msg = &si;
    ctx->rsp_desc = &SystemInfo_msg;
}

/* ------------------------------------------------------------------ */
/*  Route table                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    const char      *resource;
    uint8_t          method;
    uint8_t          min_segments;
    coap_handler_t   handler;
} coap_route_entry_t;

/*
 * Walk order matters: more-specific entries (higher min_segments) must
 * appear first so they match before less-specific entries with the same
 * method + resource.  Example: POST /sources/temp (nseg=2) must match
 * the sources POST nseg=2 entry before the POST nseg=1 entry.
 */
static const coap_route_entry_t routes[] = {
    /* More-specific (higher min_segments) first within each resource */
    {"fans",      COAP_METHOD_GET,    2, handle_fan_get},
    {"fans",      COAP_METHOD_PUT,    2, handle_fan_update},
    {"fans",      COAP_METHOD_DELETE, 2, handle_fan_delete},
    {"fans",      COAP_METHOD_POST,   1, handle_fan_create},
    {"fans",      COAP_METHOD_GET,    1, handle_fan_list},
    {"sources",   COAP_METHOD_POST,   2, handle_source_temp},
    {"sources",   COAP_METHOD_DELETE, 2, handle_source_delete},
    {"sources",   COAP_METHOD_GET,    2, handle_source_get},
    {"sources",   COAP_METHOD_POST,   1, handle_source_create},
    {"sources",   COAP_METHOD_GET,    1, handle_source_list},
    {"curves",    COAP_METHOD_GET,    2, handle_curve_get},
    {"curves",    COAP_METHOD_PUT,    2, handle_curve_update},
    {"curves",    COAP_METHOD_DELETE, 2, handle_curve_delete},
    {"curves",    COAP_METHOD_POST,   1, handle_curve_create},
    {"curves",    COAP_METHOD_GET,    1, handle_curve_list},
    {"schedules", COAP_METHOD_PUT,    2, handle_schedule_update},
    {"schedules", COAP_METHOD_DELETE, 2, handle_schedule_delete},
    {"schedules", COAP_METHOD_POST,   1, handle_schedule_create},
    {"schedules", COAP_METHOD_GET,    1, handle_schedule_list},
    {"wifi",      COAP_METHOD_GET,    2, handle_wifi},
    {"wifi",      COAP_METHOD_POST,   2, handle_wifi},
    {"system",    COAP_METHOD_PUT,    2, handle_system_put},
    {"system",    COAP_METHOD_GET,    2, handle_system},
};
#define NUM_ROUTES (sizeof(routes) / sizeof(routes[0]))

/* ------------------------------------------------------------------ */
/*  Dispatch                                                           */
/* ------------------------------------------------------------------ */

void f_coap_dispatch(struct f_coap *h, const coap_packet_t *inpkt,
                     uint8_t *tx_buf, size_t *out_len, coap_packet_t *outpkt)
{
    coap_req_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.rsp_code = COAP_RSPCODE_CONTENT;

    parse_uri(inpkt, &ctx);
    ctx.payload     = inpkt->payload.p;
    ctx.payload_len = inpkt->payload.len;

    /* Walk route table – first match wins */
    bool matched = false;
    for (size_t i = 0; i < NUM_ROUTES; i++) {
        const coap_route_entry_t *r = &routes[i];
        if (ctx.nseg >= r->min_segments &&
            inpkt->hdr.code == r->method &&
            strcmp(ctx.seg[0], r->resource) == 0) {
            r->handler(h, &ctx);
            matched = true;
            break;
        }
    }

    if (!matched) {
        ctx.rsp_code = COAP_RSPCODE_NOT_FOUND;
    }

    /* Encode protobuf response */
    static uint8_t enc_buf[COAP_ENC_BUF_SIZE];
    size_t enc_len = 0;
    const uint8_t *content = NULL;

    if (ctx.rsp_msg && ctx.rsp_desc) {
        pb_ostream_t os = pb_ostream_from_buffer(enc_buf, sizeof(enc_buf));
        if (pb_encode(&os, ctx.rsp_desc, ctx.rsp_msg)) {
            enc_len = os.bytes_written;
            content = enc_buf;
        }
    }

    /* Build CoAP response packet (scratch area = tx_buf, sized to COAP_MTU) */
    coap_rw_buffer_t scratch = { tx_buf, COAP_MTU };
    coap_responsecode_t code = (coap_responsecode_t)ctx.rsp_code;
    coap_content_type_t ct = content
        ? COAP_CONTENTTYPE_APPLICATION_OCTECT_STREAM
        : COAP_CONTENTTYPE_NONE;

    int ret = coap_make_response(&scratch, outpkt, content, enc_len,
                                  inpkt->hdr.id[0], inpkt->hdr.id[1],
                                  &inpkt->tok, code, ct);
    if (ret != 0) {
        *out_len = 0;
        return;
    }

    /* Response packet built — caller (coap_task) handles serialization via coap_build */
    *out_len = 1;  /* non-zero signals packet ready */
}
