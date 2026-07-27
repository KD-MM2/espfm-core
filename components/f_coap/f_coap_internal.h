/* f_coap_internal.h — shared types for f_coap (libcoap-4) */
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

#define COAP_MAX_SEG 4
#define COAP_MAX_SEG_LEN 32
#define COAP_PORT 5683
#define COAP_MTU 1280

struct f_coap {
    coap_context_t *ctx;
    coap_endpoint_t *ep;
    volatile bool running;
    volatile bool start_requested;
    volatile bool stop_requested;
    TaskHandle_t task;
    f_fan_handle_t fan;
    f_source_handle_t source;
    f_curve_handle_t curve;
    f_schedule_handle_t schedule;
    f_config_handle_t config;
    f_mdns_handle_t mdns;
};

void f_coap_register_resources(coap_context_t *ctx, struct f_coap *h);

void f_coap_fan_to_pb(const f_fan_info_t *fi, FanInfo *pb);
void f_coap_source_to_pb(const f_source_info_t *si, SourceInfo *pb);
void f_coap_curve_to_pb(const f_curve_info_t *ci, CurveInfo *pb);
void f_coap_schedule_to_pb(const f_schedule_info_t *sci, ScheduleInfo *pb);

#ifdef __cplusplus
}
#endif
