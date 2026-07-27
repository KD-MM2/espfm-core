/* f_coap_internal.h — shared types for f_coap decomposition */
#pragma once
#include "f_coap.h"
#include "f_fan.h"
#include "f_source.h"
#include "f_curve.h"
#include "f_schedule.h"
#include "f_config.h"
#include "f_mdns.h"
#include <coap3/coap.h>
#include "pb.h"
#include "espfm.pb.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum URI path segments and segment length */
#define COAP_MAX_SEG 4
#define COAP_MAX_SEG_LEN 32

/* CoAP server constants (shared between lifecycle and routes) */
#define COAP_PORT 5683
#define COAP_MTU 1280

/* ---- f_coap instance struct (shared between lifecycle, routes, conv) ---- */
struct f_coap {
    int sock;
    bool running;
    TaskHandle_t task;
    f_fan_handle_t fan;
    f_source_handle_t source;
    f_curve_handle_t curve;
    f_schedule_handle_t schedule;
    f_config_handle_t config;
    f_mdns_handle_t mdns;
};

/* Per-request context passed to route handlers */
typedef struct {
    char seg[COAP_MAX_SEG][COAP_MAX_SEG_LEN];
    int nseg;
    uint32_t id;
    const uint8_t *payload;
    size_t payload_len;
    /* Handler fills output: */
    void *rsp_msg;
    const pb_msgdesc_t *rsp_desc;
    int rsp_code;
} coap_req_ctx_t;

/* Route handler function type */
typedef void (*coap_handler_t)(struct f_coap *h, coap_req_ctx_t *ctx);

/* Dispatch entry point — called from coap_task() */
void f_coap_dispatch(struct f_coap *h, const coap_packet_t *inpkt,
                     uint8_t *tx_buf, size_t *out_len, coap_packet_t *outpkt);

/* Conversion helpers (native types -> protobuf) */
void f_coap_fan_to_pb(const f_fan_info_t *fi, FanInfo *pb);
void f_coap_source_to_pb(const f_source_info_t *si, SourceInfo *pb);
void f_coap_curve_to_pb(const f_curve_info_t *ci, CurveInfo *pb);
void f_coap_schedule_to_pb(const f_schedule_info_t *sci, ScheduleInfo *pb);

#ifdef __cplusplus
}
#endif
