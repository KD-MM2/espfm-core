/* f_coap_internal.h — shared types for f_coap (libcoap-4) */
#pragma once
#include "f_coap.h"
#include "f_fan.h"
#include "f_source.h"
#include "f_curve.h"
#include "f_schedule.h"
#include "f_config.h"
#include "f_control.h"
#include "f_mdns.h"
#include "f_ds18b20.h"
#include <coap3/coap.h>
#include "pb.h"
#include "espfm.pb.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COAP_MAX_SEG     4
#define COAP_MAX_SEG_LEN 32
#define COAP_PORT        5683
#define COAP_MTU         1280

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
    f_ds18b20_handle_t *ds18b20_ref;
    f_gpio_handle_t gpio;
    f_control_handle_t control;
};

void f_coap_register_resources(coap_context_t *ctx, struct f_coap *h);

#ifdef __cplusplus
}
#endif
