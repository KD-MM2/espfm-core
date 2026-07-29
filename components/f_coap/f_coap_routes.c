/* f_coap_routes.c — CoAP resource handlers and registration (libcoap-4) */
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

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static int parse_segments(const coap_pdu_t *req, char seg[][COAP_MAX_SEG_LEN], int max_seg)
{
    coap_string_t *path = coap_get_uri_path(req);
    if (!path || !path->s) {
        if (path) coap_delete_string(path);
        return 0;
    }
    int nseg           = 0;
    const uint8_t *p   = path->s;
    const uint8_t *end = path->s + path->length;
    while (p < end && nseg < max_seg) {
        if (*p == '/') {
            p++;
            continue;
        }
        const uint8_t *start = p;
        while (p < end && *p != '/') p++;
        int len = (int)(p - start);
        if (len > COAP_MAX_SEG_LEN - 1) len = COAP_MAX_SEG_LEN - 1;
        memcpy(seg[nseg], start, len);
        seg[nseg][len] = '\0';
        nseg++;
    }
    coap_delete_string(path);
    return nseg;
}

static void save_config(struct f_coap *h)
{
    if (h->config) f_config_save_all(h->config, h->fan, h->source, h->curve, h->schedule);
}

static bool encode_response(coap_pdu_t *resp, coap_pdu_code_t code, const void *msg,
                            const pb_msgdesc_t *desc)
{
    coap_pdu_set_code(resp, code);
    if (!msg || !desc) return true;
    static uint8_t enc_buf[4096];
    pb_ostream_t os = pb_ostream_from_buffer(enc_buf, sizeof(enc_buf));
    if (!pb_encode(&os, desc, msg)) return false;
    coap_add_data(resp, os.bytes_written, enc_buf);
    return true;
}

static bool decode_request(const coap_pdu_t *req, void *msg, const pb_msgdesc_t *desc)
{
    const uint8_t *data;
    size_t len;
    if (!coap_get_data(req, &len, &data) || len == 0) return false;
    pb_istream_t stream = pb_istream_from_buffer(data, len);
    return pb_decode(&stream, desc, msg);
}

/* ------------------------------------------------------------------ */
/*  Fan handlers:  /fans, /fans/{id}                                   */
/* ------------------------------------------------------------------ */

static void handle_fan_get(coap_resource_t *resource, coap_session_t *session,
                           const coap_pdu_t *req, const coap_string_t *query, coap_pdu_t *resp)
{
    struct f_coap *h = (struct f_coap *)coap_resource_get_userdata(resource);
    char seg[COAP_MAX_SEG][COAP_MAX_SEG_LEN];
    int nseg = parse_segments(req, seg, COAP_MAX_SEG);

    if (nseg >= 2) {
        /* GET /fans/{id} */
        uint32_t id = (uint32_t)atoi(seg[1]);
        f_fan_info_t fi;
        if (f_fan_get_info(h->fan, (uint8_t)id, &fi) != ESP_OK) {
            coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
            return;
        }
        static FanInfo pb;
        espfm_fan_to_pb(&fi, &pb);
        encode_response(resp, COAP_RESPONSE_CODE_CONTENT, &pb, &FanInfo_msg);
    } else {
        /* GET /fans — list all */
        static FanList list;
        list = (FanList)FanList_init_default;
        for (uint8_t i = 0; i < F_FAN_MAX_COUNT; i++) {
            f_fan_info_t fi;
            if (f_fan_get_info(h->fan, i, &fi) == ESP_OK)
                espfm_fan_to_pb(&fi, &list.fans[list.fans_count++]);
        }
        encode_response(resp, COAP_RESPONSE_CODE_CONTENT, &list, &FanList_msg);
    }
}

static void handle_fan_post(coap_resource_t *resource, coap_session_t *session,
                            const coap_pdu_t *req, const coap_string_t *query, coap_pdu_t *resp)
{
    struct f_coap *h    = (struct f_coap *)coap_resource_get_userdata(resource);
    FanCreateRequest cr = FanCreateRequest_init_default;
    if (!decode_request(req, &cr, &FanCreateRequest_msg)) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_BAD_REQUEST);
        return;
    }
    uint8_t nid;
    if (f_fan_add(h->fan, (uint8_t)cr.pwm_gpio, (uint8_t)cr.tach_gpio, cr.name, &nid) != ESP_OK) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_BAD_REQUEST);
        return;
    }
    save_config(h);
    f_fan_info_t fi;
    f_fan_get_info(h->fan, nid, &fi);
    static FanInfo pb;
    espfm_fan_to_pb(&fi, &pb);
    encode_response(resp, COAP_RESPONSE_CODE_CREATED, &pb, &FanInfo_msg);
}

static void handle_fan_put(coap_resource_t *resource, coap_session_t *session,
                           const coap_pdu_t *req, const coap_string_t *query, coap_pdu_t *resp)
{
    struct f_coap *h = (struct f_coap *)coap_resource_get_userdata(resource);
    char seg[COAP_MAX_SEG][COAP_MAX_SEG_LEN];
    int nseg = parse_segments(req, seg, COAP_MAX_SEG);
    if (nseg < 2) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
        return;
    }
    uint32_t id         = (uint32_t)atoi(seg[1]);

    FanUpdateRequest ur = FanUpdateRequest_init_default;
    if (!decode_request(req, &ur, &FanUpdateRequest_msg)) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_BAD_REQUEST);
        return;
    }
    f_fan_info_t fi;
    if (f_fan_get_info(h->fan, (uint8_t)id, &fi) != ESP_OK) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
        return;
    }
    if (ur.has_mode) f_fan_set_mode(h->fan, (uint8_t)id, (fan_mode_t)ur.mode);
    if (ur.has_duty) f_fan_set_duty(h->fan, (uint8_t)id, (uint8_t)ur.duty);
    if (ur.has_source_id) f_fan_set_source(h->fan, (uint8_t)id, (uint8_t)ur.source_id);
    if (ur.has_curve_id) f_fan_set_curve(h->fan, (uint8_t)id, (uint8_t)ur.curve_id);
    if (ur.has_schedule_id) f_fan_set_schedule(h->fan, (uint8_t)id, (uint8_t)ur.schedule_id);
    if (ur.has_group_id) f_fan_set_group(h->fan, (uint8_t)id, (uint8_t)ur.group_id);
    if (ur.has_inverted) f_fan_set_inverted(h->fan, (uint8_t)id, ur.inverted);
    if (ur.has_enabled) f_fan_set_enabled(h->fan, (uint8_t)id, ur.enabled);
    save_config(h);
    f_fan_get_info(h->fan, (uint8_t)id, &fi);
    static FanInfo pb;
    espfm_fan_to_pb(&fi, &pb);
    encode_response(resp, COAP_RESPONSE_CODE_CHANGED, &pb, &FanInfo_msg);
}

static void handle_fan_delete(coap_resource_t *resource, coap_session_t *session,
                              const coap_pdu_t *req, const coap_string_t *query, coap_pdu_t *resp)
{
    struct f_coap *h = (struct f_coap *)coap_resource_get_userdata(resource);
    char seg[COAP_MAX_SEG][COAP_MAX_SEG_LEN];
    int nseg = parse_segments(req, seg, COAP_MAX_SEG);
    if (nseg < 2) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
        return;
    }
    uint32_t id = (uint32_t)atoi(seg[1]);
    if (f_fan_remove(h->fan, (uint8_t)id) != ESP_OK) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
        return;
    }
    save_config(h);
    static StatusResponse sr;
    sr    = (StatusResponse)StatusResponse_init_default;
    sr.ok = true;
    encode_response(resp, COAP_RESPONSE_CODE_DELETED, &sr, &StatusResponse_msg);
}

/* ------------------------------------------------------------------ */
/*  DS18B20 handlers: /ds18b20/scan                                    */
/* ------------------------------------------------------------------ */

/* Context for the Ds18b20ScanResponse devices callback encoding */
typedef struct {
    const Ds18b20Device *devs;
    uint8_t count;
} ds18b20_cb_ctx_t;

static bool ds18b20_encode_cb(pb_ostream_t *stream, const pb_field_t *field, void *const *arg)
{
    const ds18b20_cb_ctx_t *ctx = (const ds18b20_cb_ctx_t *)(*arg);
    for (uint8_t i = 0; i < ctx->count; i++) {
        if (!pb_encode_tag_for_field(stream, field)) return false;
        if (!pb_encode_submessage(stream, Ds18b20Device_fields, &ctx->devs[i])) return false;
    }
    return true;
}

static void handle_ds18b20_scan(coap_resource_t *resource, coap_session_t *session,
                                const coap_pdu_t *req, const coap_string_t *query, coap_pdu_t *resp)
{
    struct f_coap *h = (struct f_coap *)coap_resource_get_userdata(resource);
    if (h->ds18b20_ref == NULL || *h->ds18b20_ref == NULL) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE);
        return;
    }
    f_ds18b20_handle_t ds = *h->ds18b20_ref;

    uint8_t count         = 0;
    if (f_ds18b20_scan(ds, &count) != ESP_OK) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_INTERNAL_ERROR);
        return;
    }

    /* Trigger batch conversion for all discovered sensors */
    f_ds18b20_trigger_all(ds);

    static Ds18b20Device devices[F_DS18B20_MAX_DEVICES];
    for (uint8_t i = 0; i < count; i++) {
        devices[i]       = (Ds18b20Device)Ds18b20Device_init_default;
        devices[i].index = i;
        f_ds18b20_get_rom_code(ds, i, &devices[i].rom_code);
        float temp;
        if (f_ds18b20_read_temp(ds, i, &temp) == ESP_OK) {
            devices[i].temp_c = temp;
        }
    }

    ds18b20_cb_ctx_t cb_ctx = {.devs = devices, .count = count};

    static Ds18b20ScanResponse sr;
    sr                      = (Ds18b20ScanResponse)Ds18b20ScanResponse_init_default;
    sr.devices.funcs.encode = ds18b20_encode_cb;
    sr.devices.arg          = &cb_ctx;
    sr.device_count         = count;

    encode_response(resp, COAP_RESPONSE_CODE_CONTENT, &sr, &Ds18b20ScanResponse_msg);
}

static void handle_ds18b20_config(coap_resource_t *resource, coap_session_t *session,
                                  const coap_pdu_t *req, const coap_string_t *query,
                                  coap_pdu_t *resp)
{
    struct f_coap *h        = (struct f_coap *)coap_resource_get_userdata(resource);
    Ds18b20ConfigRequest cr = Ds18b20ConfigRequest_init_default;
    if (!decode_request(req, &cr, &Ds18b20ConfigRequest_msg)) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_BAD_REQUEST);
        return;
    }

    uint8_t gpio = (uint8_t)cr.gpio;

    /* Initialize or re-initialize the DS18B20 bus */
    if (h->ds18b20_ref && *h->ds18b20_ref != NULL) {
        /* Bus already initialized — return success with current device count */
        uint8_t count = 0;
        f_ds18b20_scan(*h->ds18b20_ref, &count);
        static StatusResponse sr;
        sr    = (StatusResponse)StatusResponse_init_default;
        sr.ok = true;
        encode_response(resp, COAP_RESPONSE_CODE_CHANGED, &sr, &StatusResponse_msg);
        return;
    }

    f_ds18b20_handle_t ds18b20 = NULL;
    esp_err_t err              = f_ds18b20_init(&ds18b20, gpio);
    if (err != ESP_OK) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_BAD_REQUEST);
        return;
    }

    if (h->ds18b20_ref) *h->ds18b20_ref = ds18b20;
    if (h->config) f_config_save_ds18b20_gpio(h->config, gpio);

    static StatusResponse sr;
    sr    = (StatusResponse)StatusResponse_init_default;
    sr.ok = true;
    encode_response(resp, COAP_RESPONSE_CODE_CHANGED, &sr, &StatusResponse_msg);
}

/* ------------------------------------------------------------------ */
/*  Source handlers: /sources, /sources/{id}, /sources/temp             */
/* ------------------------------------------------------------------ */

static void handle_source_get(coap_resource_t *resource, coap_session_t *session,
                              const coap_pdu_t *req, const coap_string_t *query, coap_pdu_t *resp)
{
    struct f_coap *h = (struct f_coap *)coap_resource_get_userdata(resource);
    char seg[COAP_MAX_SEG][COAP_MAX_SEG_LEN];
    int nseg = parse_segments(req, seg, COAP_MAX_SEG);

    if (nseg >= 2) {
        /* GET /sources/{id} */
        uint32_t id = (uint32_t)atoi(seg[1]);
        f_source_info_t si;
        if (f_source_get_info(h->source, (uint8_t)id, &si) != ESP_OK) {
            coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
            return;
        }
        static SourceInfo pb;
        espfm_source_to_pb(&si, &pb);
        encode_response(resp, COAP_RESPONSE_CODE_CONTENT, &pb, &SourceInfo_msg);
    } else {
        /* GET /sources — list all */
        static SourceList list;
        list = (SourceList)SourceList_init_default;
        for (uint8_t i = 0; i < F_SOURCE_MAX_COUNT; i++) {
            f_source_info_t si;
            if (f_source_get_info(h->source, i, &si) == ESP_OK)
                espfm_source_to_pb(&si, &list.sources[list.sources_count++]);
        }
        encode_response(resp, COAP_RESPONSE_CODE_CONTENT, &list, &SourceList_msg);
    }
}

static void handle_source_post(coap_resource_t *resource, coap_session_t *session,
                               const coap_pdu_t *req, const coap_string_t *query, coap_pdu_t *resp)
{
    struct f_coap *h = (struct f_coap *)coap_resource_get_userdata(resource);
    char seg[COAP_MAX_SEG][COAP_MAX_SEG_LEN];
    int nseg = parse_segments(req, seg, COAP_MAX_SEG);

    if (nseg >= 2 && strcmp(seg[1], "temp") == 0) {
        /* POST /sources/temp — manual temperature update */
        ManualTempRequest mtr = ManualTempRequest_init_default;
        if (!decode_request(req, &mtr, &ManualTempRequest_msg)) {
            coap_pdu_set_code(resp, COAP_RESPONSE_CODE_BAD_REQUEST);
            return;
        }
        f_source_info_t si_check;
        if (f_source_get_info(h->source, (uint8_t)mtr.id, &si_check) != ESP_OK) {
            coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
            return;
        }
        if (f_source_update_manual(h->source, (uint8_t)mtr.id, mtr.temp_c) != ESP_OK) {
            coap_pdu_set_code(resp, COAP_RESPONSE_CODE_BAD_REQUEST);
            return;
        }
        static StatusResponse sr;
        sr    = (StatusResponse)StatusResponse_init_default;
        sr.ok = true;
        encode_response(resp, COAP_RESPONSE_CODE_CHANGED, &sr, &StatusResponse_msg);
    } else {
        /* POST /sources — create */
        SourceCreateRequest cr = SourceCreateRequest_init_default;
        if (!decode_request(req, &cr, &SourceCreateRequest_msg)) {
            coap_pdu_set_code(resp, COAP_RESPONSE_CODE_BAD_REQUEST);
            return;
        }
        uint8_t nid;
        source_type_t stype = pb_to_source_type(cr.type);

        if (stype == SOURCE_TYPE_DS18B20) {
            if (f_source_add_ds18b20(h->source, cr.ds18b20_rom_code, cr.name, &nid) != ESP_OK) {
                coap_pdu_set_code(resp, COAP_RESPONSE_CODE_BAD_REQUEST);
                return;
            }
        } else {
            if (f_source_add(h->source, stype, (uint8_t)cr.gpio, cr.name, &nid) != ESP_OK) {
                coap_pdu_set_code(resp, COAP_RESPONSE_CODE_BAD_REQUEST);
                return;
            }
        }
        save_config(h);
        f_source_info_t si;
        f_source_get_info(h->source, nid, &si);
        static SourceInfo pb;
        espfm_source_to_pb(&si, &pb);
        encode_response(resp, COAP_RESPONSE_CODE_CREATED, &pb, &SourceInfo_msg);
    }
}

static void handle_source_put(coap_resource_t *resource, coap_session_t *session,
                              const coap_pdu_t *req, const coap_string_t *query, coap_pdu_t *resp)
{
    struct f_coap *h = (struct f_coap *)coap_resource_get_userdata(resource);
    char seg[COAP_MAX_SEG][COAP_MAX_SEG_LEN];
    int nseg = parse_segments(req, seg, COAP_MAX_SEG);
    if (nseg < 2) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
        return;
    }
    uint32_t id            = (uint32_t)atoi(seg[1]);

    SourceUpdateRequest ur = SourceUpdateRequest_init_default;
    if (!decode_request(req, &ur, &SourceUpdateRequest_msg)) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_BAD_REQUEST);
        return;
    }
    f_source_info_t si;
    if (f_source_get_info(h->source, (uint8_t)id, &si) != ESP_OK) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
        return;
    }
    if (f_source_set_name(h->source, (uint8_t)id, ur.name) != ESP_OK) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_BAD_REQUEST);
        return;
    }
    save_config(h);
    f_source_get_info(h->source, (uint8_t)id, &si);
    static SourceInfo pb;
    espfm_source_to_pb(&si, &pb);
    encode_response(resp, COAP_RESPONSE_CODE_CHANGED, &pb, &SourceInfo_msg);
}

static void handle_source_delete(coap_resource_t *resource, coap_session_t *session,
                                 const coap_pdu_t *req, const coap_string_t *query,
                                 coap_pdu_t *resp)
{
    struct f_coap *h = (struct f_coap *)coap_resource_get_userdata(resource);
    char seg[COAP_MAX_SEG][COAP_MAX_SEG_LEN];
    int nseg = parse_segments(req, seg, COAP_MAX_SEG);
    if (nseg < 2) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
        return;
    }
    uint32_t id = (uint32_t)atoi(seg[1]);
    if (f_source_remove(h->source, (uint8_t)id) != ESP_OK) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
        return;
    }
    save_config(h);
    static StatusResponse sr;
    sr    = (StatusResponse)StatusResponse_init_default;
    sr.ok = true;
    encode_response(resp, COAP_RESPONSE_CODE_DELETED, &sr, &StatusResponse_msg);
}

/* ------------------------------------------------------------------ */
/*  Curve handlers: /curves, /curves/{id}                              */
/* ------------------------------------------------------------------ */

static void handle_curve_get(coap_resource_t *resource, coap_session_t *session,
                             const coap_pdu_t *req, const coap_string_t *query, coap_pdu_t *resp)
{
    struct f_coap *h = (struct f_coap *)coap_resource_get_userdata(resource);
    char seg[COAP_MAX_SEG][COAP_MAX_SEG_LEN];
    int nseg = parse_segments(req, seg, COAP_MAX_SEG);

    if (nseg >= 2) {
        /* GET /curves/{id} */
        uint32_t id = (uint32_t)atoi(seg[1]);
        f_curve_info_t ci;
        if (f_curve_get_info(h->curve, (uint8_t)id, &ci) != ESP_OK) {
            coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
            return;
        }
        static CurveInfo pb;
        espfm_curve_to_pb(&ci, &pb);
        encode_response(resp, COAP_RESPONSE_CODE_CONTENT, &pb, &CurveInfo_msg);
    } else {
        /* GET /curves — list all */
        static CurveList list;
        list = (CurveList)CurveList_init_default;
        for (uint8_t i = 0; i < F_CURVE_MAX_COUNT; i++) {
            f_curve_info_t ci;
            if (f_curve_get_info(h->curve, i, &ci) == ESP_OK)
                espfm_curve_to_pb(&ci, &list.curves[list.curves_count++]);
        }
        encode_response(resp, COAP_RESPONSE_CODE_CONTENT, &list, &CurveList_msg);
    }
}

static void handle_curve_post(coap_resource_t *resource, coap_session_t *session,
                              const coap_pdu_t *req, const coap_string_t *query, coap_pdu_t *resp)
{
    struct f_coap *h      = (struct f_coap *)coap_resource_get_userdata(resource);
    CurveCreateRequest cr = CurveCreateRequest_init_default;
    if (!decode_request(req, &cr, &CurveCreateRequest_msg)) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_BAD_REQUEST);
        return;
    }
    f_curve_info_t ci = {0};
    strncpy(ci.name, cr.name, sizeof(ci.name) - 1);
    ci.name[sizeof(ci.name) - 1] = '\0';
    ci.num_points                = (uint8_t)cr.points_count;
    for (int i = 0; i < cr.points_count && i < F_CURVE_MAX_POINTS; i++) {
        ci.points[i].temp_c = cr.points[i].temp_c;
        ci.points[i].duty   = (uint8_t)cr.points[i].duty;
    }
    uint8_t nid;
    if (f_curve_upsert(h->curve, &ci, &nid) != ESP_OK) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_BAD_REQUEST);
        return;
    }
    save_config(h);
    f_curve_get_info(h->curve, nid, &ci);
    static CurveInfo pb;
    espfm_curve_to_pb(&ci, &pb);
    encode_response(resp, COAP_RESPONSE_CODE_CREATED, &pb, &CurveInfo_msg);
}

static void handle_curve_put(coap_resource_t *resource, coap_session_t *session,
                             const coap_pdu_t *req, const coap_string_t *query, coap_pdu_t *resp)
{
    struct f_coap *h = (struct f_coap *)coap_resource_get_userdata(resource);
    char seg[COAP_MAX_SEG][COAP_MAX_SEG_LEN];
    int nseg = parse_segments(req, seg, COAP_MAX_SEG);
    if (nseg < 2) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
        return;
    }
    uint32_t id           = (uint32_t)atoi(seg[1]);

    CurveUpdateRequest ur = CurveUpdateRequest_init_default;
    if (!decode_request(req, &ur, &CurveUpdateRequest_msg)) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_BAD_REQUEST);
        return;
    }
    f_curve_info_t ci = {0};
    ci.id             = (uint8_t)id;
    strncpy(ci.name, ur.name, sizeof(ci.name) - 1);
    ci.name[sizeof(ci.name) - 1] = '\0';
    ci.num_points                = (uint8_t)ur.points_count;
    for (int i = 0; i < ur.points_count && i < F_CURVE_MAX_POINTS; i++) {
        ci.points[i].temp_c = ur.points[i].temp_c;
        ci.points[i].duty   = (uint8_t)ur.points[i].duty;
    }
    uint8_t oid;
    if (f_curve_upsert(h->curve, &ci, &oid) != ESP_OK) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_BAD_REQUEST);
        return;
    }
    save_config(h);
    f_curve_get_info(h->curve, oid, &ci);
    static CurveInfo pb;
    espfm_curve_to_pb(&ci, &pb);
    encode_response(resp, COAP_RESPONSE_CODE_CHANGED, &pb, &CurveInfo_msg);
}

static void handle_curve_delete(coap_resource_t *resource, coap_session_t *session,
                                const coap_pdu_t *req, const coap_string_t *query, coap_pdu_t *resp)
{
    struct f_coap *h = (struct f_coap *)coap_resource_get_userdata(resource);
    char seg[COAP_MAX_SEG][COAP_MAX_SEG_LEN];
    int nseg = parse_segments(req, seg, COAP_MAX_SEG);
    if (nseg < 2) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
        return;
    }
    uint32_t id = (uint32_t)atoi(seg[1]);
    if (f_curve_remove(h->curve, (uint8_t)id) != ESP_OK) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
        return;
    }
    save_config(h);
    static StatusResponse sr;
    sr    = (StatusResponse)StatusResponse_init_default;
    sr.ok = true;
    encode_response(resp, COAP_RESPONSE_CODE_DELETED, &sr, &StatusResponse_msg);
}

/* ------------------------------------------------------------------ */
/*  Schedule handlers: /schedules, /schedules/{id}                     */
/* ------------------------------------------------------------------ */

static void handle_schedule_get(coap_resource_t *resource, coap_session_t *session,
                                const coap_pdu_t *req, const coap_string_t *query, coap_pdu_t *resp)
{
    struct f_coap *h = (struct f_coap *)coap_resource_get_userdata(resource);
    char seg[COAP_MAX_SEG][COAP_MAX_SEG_LEN];
    int nseg = parse_segments(req, seg, COAP_MAX_SEG);

    if (nseg >= 2) {
        /* GET /schedules/{id} */
        uint32_t id = (uint32_t)atoi(seg[1]);
        f_schedule_info_t si;
        if (f_schedule_get_info(h->schedule, (uint8_t)id, &si) != ESP_OK) {
            coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
            return;
        }
        static ScheduleInfo pb;
        espfm_schedule_to_pb(&si, &pb);
        encode_response(resp, COAP_RESPONSE_CODE_CONTENT, &pb, &ScheduleInfo_msg);
    } else {
        /* GET /schedules — list all */
        static ScheduleList list;
        list = (ScheduleList)ScheduleList_init_default;
        for (uint8_t i = 0; i < F_SCHEDULE_MAX_COUNT; i++) {
            f_schedule_info_t si;
            if (f_schedule_get_info(h->schedule, i, &si) == ESP_OK)
                espfm_schedule_to_pb(&si, &list.schedules[list.schedules_count++]);
        }
        encode_response(resp, COAP_RESPONSE_CODE_CONTENT, &list, &ScheduleList_msg);
    }
}

static void handle_schedule_post(coap_resource_t *resource, coap_session_t *session,
                                 const coap_pdu_t *req, const coap_string_t *query,
                                 coap_pdu_t *resp)
{
    struct f_coap *h         = (struct f_coap *)coap_resource_get_userdata(resource);
    ScheduleCreateRequest cr = ScheduleCreateRequest_init_default;
    if (!decode_request(req, &cr, &ScheduleCreateRequest_msg)) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_BAD_REQUEST);
        return;
    }
    f_schedule_info_t si = {.fan_id    = (uint8_t)cr.fan_id,
                            .duty      = (uint8_t)cr.duty,
                            .start_min = (uint16_t)cr.start_min,
                            .end_min   = (uint16_t)cr.end_min,
                            .enabled   = cr.enabled};
    uint8_t nid;
    if (f_schedule_add(h->schedule, &si, &nid) != ESP_OK) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_BAD_REQUEST);
        return;
    }
    save_config(h);
    f_schedule_get_info(h->schedule, nid, &si);
    static ScheduleInfo pb;
    espfm_schedule_to_pb(&si, &pb);
    encode_response(resp, COAP_RESPONSE_CODE_CREATED, &pb, &ScheduleInfo_msg);
}

static void handle_schedule_put(coap_resource_t *resource, coap_session_t *session,
                                const coap_pdu_t *req, const coap_string_t *query, coap_pdu_t *resp)
{
    struct f_coap *h = (struct f_coap *)coap_resource_get_userdata(resource);
    char seg[COAP_MAX_SEG][COAP_MAX_SEG_LEN];
    int nseg = parse_segments(req, seg, COAP_MAX_SEG);
    if (nseg < 2) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
        return;
    }
    uint32_t id              = (uint32_t)atoi(seg[1]);

    ScheduleUpdateRequest ur = ScheduleUpdateRequest_init_default;
    if (!decode_request(req, &ur, &ScheduleUpdateRequest_msg)) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_BAD_REQUEST);
        return;
    }
    f_schedule_info_t si;
    if (f_schedule_get_info(h->schedule, (uint8_t)id, &si) != ESP_OK) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
        return;
    }
    if (ur.has_fan_id) si.fan_id = (uint8_t)ur.fan_id;
    if (ur.has_duty) si.duty = (uint8_t)ur.duty;
    if (ur.has_start_min) si.start_min = (uint16_t)ur.start_min;
    if (ur.has_end_min) si.end_min = (uint16_t)ur.end_min;
    if (ur.has_enabled) si.enabled = ur.enabled;
    if (f_schedule_update(h->schedule, (uint8_t)id, &si) != ESP_OK) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
        return;
    }
    save_config(h);
    f_schedule_get_info(h->schedule, (uint8_t)id, &si);
    static ScheduleInfo pb;
    espfm_schedule_to_pb(&si, &pb);
    encode_response(resp, COAP_RESPONSE_CODE_CHANGED, &pb, &ScheduleInfo_msg);
}

static void handle_schedule_delete(coap_resource_t *resource, coap_session_t *session,
                                   const coap_pdu_t *req, const coap_string_t *query,
                                   coap_pdu_t *resp)
{
    struct f_coap *h = (struct f_coap *)coap_resource_get_userdata(resource);
    char seg[COAP_MAX_SEG][COAP_MAX_SEG_LEN];
    int nseg = parse_segments(req, seg, COAP_MAX_SEG);
    if (nseg < 2) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
        return;
    }
    uint32_t id = (uint32_t)atoi(seg[1]);
    if (f_schedule_remove(h->schedule, (uint8_t)id) != ESP_OK) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
        return;
    }
    save_config(h);
    static StatusResponse sr;
    sr    = (StatusResponse)StatusResponse_init_default;
    sr.ok = true;
    encode_response(resp, COAP_RESPONSE_CODE_DELETED, &sr, &StatusResponse_msg);
}

/* ------------------------------------------------------------------ */
/*  WiFi handler: /wifi/{scan,connect,status}                          */
/* ------------------------------------------------------------------ */

static void handle_wifi_get(coap_resource_t *resource, coap_session_t *session,
                            const coap_pdu_t *req, const coap_string_t *query, coap_pdu_t *resp)
{
    char seg[COAP_MAX_SEG][COAP_MAX_SEG_LEN];
    int nseg = parse_segments(req, seg, COAP_MAX_SEG);
    if (nseg < 2) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
        return;
    }

    if (strcmp(seg[1], "scan") == 0) {
        /* GET /wifi/scan */
        wifi_scan_config_t sc = {.scan_type            = WIFI_SCAN_TYPE_ACTIVE,
                                 .scan_time.active.min = 100,
                                 .scan_time.active.max = 300};
        if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
            coap_pdu_set_code(resp, COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE);
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
                        ap->rssi                       = aps[i].rssi;
                        ap->channel                    = aps[i].primary;
                        ap->authmode                   = aps[i].authmode;
                    }
                }
                free(aps);
            }
        }
        encode_response(resp, COAP_RESPONSE_CODE_CONTENT, &sr, &WifiScanResult_msg);
    } else if (strcmp(seg[1], "status") == 0) {
        /* GET /wifi/status */
        static WifiStatus ws;
        ws               = (WifiStatus)WifiStatus_init_default;
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
        encode_response(resp, COAP_RESPONSE_CODE_CONTENT, &ws, &WifiStatus_msg);
    } else {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
    }
}

static void handle_wifi_post(coap_resource_t *resource, coap_session_t *session,
                             const coap_pdu_t *req, const coap_string_t *query, coap_pdu_t *resp)
{
    char seg[COAP_MAX_SEG][COAP_MAX_SEG_LEN];
    int nseg = parse_segments(req, seg, COAP_MAX_SEG);
    if (nseg < 2 || strcmp(seg[1], "connect") != 0) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
        return;
    }

    /* POST /wifi/connect */
    WifiConnectRequest cr = WifiConnectRequest_init_default;
    if (!decode_request(req, &cr, &WifiConnectRequest_msg)) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_BAD_REQUEST);
        return;
    }
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, cr.ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, cr.password, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE);
        return;
    }
    esp_wifi_disconnect();
    esp_wifi_connect();
    static StatusResponse sr;
    sr    = (StatusResponse)StatusResponse_init_default;
    sr.ok = true;
    encode_response(resp, COAP_RESPONSE_CODE_CHANGED, &sr, &StatusResponse_msg);
}

/* ------------------------------------------------------------------ */
/*  Reboot timer (one-shot, 2s delay)                                 */
/* ------------------------------------------------------------------ */

static bool s_reboot_pending             = false;
static esp_timer_handle_t s_reboot_timer = NULL;

static void reboot_timer_cb(void *arg)
{
    esp_restart();
}

static void handle_system_post(coap_resource_t *resource, coap_session_t *session,
                               const coap_pdu_t *req, const coap_string_t *query, coap_pdu_t *resp)
{
    char seg[COAP_MAX_SEG][COAP_MAX_SEG_LEN];
    int nseg = parse_segments(req, seg, COAP_MAX_SEG);
    if (nseg < 2 || strcmp(seg[1], "reboot") != 0) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
        return;
    }

    if (s_reboot_pending) {
        static StatusResponse sr;
        sr    = (StatusResponse)StatusResponse_init_default;
        sr.ok = false;
        snprintf(sr.error_msg, sizeof(sr.error_msg), "reboot pending");
        encode_response(resp, COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE, &sr, &StatusResponse_msg);
        return;
    }

    if (!s_reboot_timer) {
        const esp_timer_create_args_t args = {
            .callback = reboot_timer_cb,
            .name     = "coap_reboot",
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &s_reboot_timer));
    }

    esp_err_t err = esp_timer_start_once(s_reboot_timer, 2000000); /* 2 seconds */
    if (err != ESP_OK) {
        static StatusResponse sr;
        sr    = (StatusResponse)StatusResponse_init_default;
        sr.ok = false;
        snprintf(sr.error_msg, sizeof(sr.error_msg), "timer start failed: %s",
                 esp_err_to_name(err));
        encode_response(resp, COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE, &sr, &StatusResponse_msg);
        ESP_LOGE(TAG, "esp_timer_start_once failed: %s", esp_err_to_name(err));
        return;
    }

    s_reboot_pending = true;

    static StatusResponse sr;
    sr    = (StatusResponse)StatusResponse_init_default;
    sr.ok = true;
    encode_response(resp, COAP_RESPONSE_CODE_CHANGED, &sr, &StatusResponse_msg);

    ESP_LOGI(TAG, "Reboot scheduled in 2 seconds");
}

/* ------------------------------------------------------------------ */
/*  System handler: /system/{info,hostname}                            */
/* ------------------------------------------------------------------ */

static void handle_system_get(coap_resource_t *resource, coap_session_t *session,
                              const coap_pdu_t *req, const coap_string_t *query, coap_pdu_t *resp)
{
    struct f_coap *h = (struct f_coap *)coap_resource_get_userdata(resource);
    char seg[COAP_MAX_SEG][COAP_MAX_SEG_LEN];
    int nseg = parse_segments(req, seg, COAP_MAX_SEG);
    if (nseg < 2 || strcmp(seg[1], "info") != 0) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
        return;
    }

    static SystemInfo si;
    si = (SystemInfo)SystemInfo_init_default;
    snprintf(si.version, sizeof(si.version), "%d.%d.%d", ESPFM_VERSION_MAJOR, ESPFM_VERSION_MINOR,
             ESPFM_VERSION_PATCH);
    si.uptime_s          = (uint32_t)(esp_timer_get_time() / 1000000);
    si.heap_free         = esp_get_free_heap_size();
    si.fan_count         = h->fan ? f_fan_get_count(h->fan) : 0;
    si.source_count      = h->source ? f_source_get_count(h->source) : 0;
    si.curve_count       = h->curve ? f_curve_get_count(h->curve) : 0;
    si.schedule_count    = h->schedule ? f_schedule_get_count(h->schedule) : 0;
    const char *hostname = f_mdns_get_hostname(h->mdns);
    if (hostname) {
        strncpy(si.hostname, hostname, sizeof(si.hostname) - 1);
        si.hostname[sizeof(si.hostname) - 1] = '\0';
    }
    encode_response(resp, COAP_RESPONSE_CODE_CONTENT, &si, &SystemInfo_msg);
}

static void handle_system_put(coap_resource_t *resource, coap_session_t *session,
                              const coap_pdu_t *req, const coap_string_t *query, coap_pdu_t *resp)
{
    char seg[COAP_MAX_SEG][COAP_MAX_SEG_LEN];
    int nseg = parse_segments(req, seg, COAP_MAX_SEG);
    if (nseg < 2 || strcmp(seg[1], "hostname") != 0) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_NOT_FOUND);
        return;
    }

    HostnameRequest hr = HostnameRequest_init_default;
    if (!decode_request(req, &hr, &HostnameRequest_msg)) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_BAD_REQUEST);
        return;
    }
    if (f_mdns_set_hostname(hr.hostname) != ESP_OK) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_BAD_REQUEST);
        return;
    }
    static StatusResponse sr;
    sr    = (StatusResponse)StatusResponse_init_default;
    sr.ok = true;
    encode_response(resp, COAP_RESPONSE_CODE_CHANGED, &sr, &StatusResponse_msg);
}

/* ------------------------------------------------------------------ */
/*  Resource registration                                              */
/*  libcoap matches by exact URI path.  Register every path the       */
/*  shell sends, including sub-paths like /system/info, /wifi/scan.   */
/* ------------------------------------------------------------------ */

static void add_resource(coap_context_t *ctx, struct f_coap *h, const char *path,
                         coap_method_handler_t get, coap_method_handler_t post,
                         coap_method_handler_t put, coap_method_handler_t del)
{
    coap_resource_t *r = coap_resource_init(coap_make_str_const(path), 0);
    if (get) coap_register_handler(r, COAP_REQUEST_CODE_GET, get);
    if (post) coap_register_handler(r, COAP_REQUEST_CODE_POST, post);
    if (put) coap_register_handler(r, COAP_REQUEST_CODE_PUT, put);
    if (del) coap_register_handler(r, COAP_REQUEST_CODE_DELETE, del);
    coap_resource_set_userdata(r, h);
    coap_add_resource(ctx, r);
}

void f_coap_register_resources(coap_context_t *ctx, struct f_coap *h)
{
    /* /fans — list (GET), create (POST) */
    add_resource(ctx, h, "fans", handle_fan_get, handle_fan_post, NULL, NULL);

    /* /sources — list (GET), create (POST) */
    add_resource(ctx, h, "sources", handle_source_get, handle_source_post, NULL, NULL);
    /* /sources/temp — manual temperature update */
    add_resource(ctx, h, "sources/temp", NULL, handle_source_post, NULL, NULL);

    /* /curves — list (GET), create (POST) */
    add_resource(ctx, h, "curves", handle_curve_get, handle_curve_post, NULL, NULL);

    /* /schedules — list (GET), create (POST) */
    add_resource(ctx, h, "schedules", handle_schedule_get, handle_schedule_post, NULL, NULL);

    /* Sub-paths for {resource}/{id} — register for IDs 0..7 */
    for (int i = 0; i < 8; i++) {
        char path[16];
        snprintf(path, sizeof(path), "fans/%d", i);
        add_resource(ctx, h, path, handle_fan_get, NULL, handle_fan_put, handle_fan_delete);
        snprintf(path, sizeof(path), "sources/%d", i);
        add_resource(ctx, h, path, handle_source_get, NULL, handle_source_put,
                     handle_source_delete);
        snprintf(path, sizeof(path), "curves/%d", i);
        add_resource(ctx, h, path, handle_curve_get, NULL, handle_curve_put, handle_curve_delete);
        snprintf(path, sizeof(path), "schedules/%d", i);
        add_resource(ctx, h, path, handle_schedule_get, NULL, handle_schedule_put,
                     handle_schedule_delete);
    }

    /* /system/info */
    add_resource(ctx, h, "system/info", handle_system_get, NULL, NULL, NULL);
    /* /system/hostname */
    add_resource(ctx, h, "system/hostname", NULL, NULL, handle_system_put, NULL);
    /* /system/reboot */
    add_resource(ctx, h, "system/reboot", NULL, handle_system_post, NULL, NULL);

    /* /ds18b20/scan */
    add_resource(ctx, h, "ds18b20/scan", handle_ds18b20_scan, NULL, NULL, NULL);
    /* /ds18b20/config — runtime GPIO configuration */
    add_resource(ctx, h, "ds18b20/config", NULL, handle_ds18b20_config, NULL, NULL);

    /* /wifi/scan */
    add_resource(ctx, h, "wifi/scan", handle_wifi_get, NULL, NULL, NULL);
    /* /wifi/status */
    add_resource(ctx, h, "wifi/status", handle_wifi_get, NULL, NULL, NULL);
    /* /wifi/connect */
    add_resource(ctx, h, "wifi/connect", NULL, handle_wifi_post, NULL, NULL);
}
