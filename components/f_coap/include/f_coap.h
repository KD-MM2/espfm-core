#pragma once
#include "esp_err.h"
#include "f_core.h"
#include "f_fan.h"
#include "f_source.h"
#include "f_curve.h"
#include "f_schedule.h"
#include "f_config.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/*  CoAP Resource Map  (v3 — replaces f_http REST + JSON)                    */
/*                                                                           */
/*  METHOD  PATH                     REQUEST         RESPONSE                */
/*  ------  ----                     -------         --------                */
/*  GET     /fans                    —               FanList                 */
/*  GET     /fans/{id}               FanId           FanInfo                 */
/*  POST    /fans                    FanCreateReq    FanInfo                 */
/*  PUT     /fans/{id}               FanUpdateReq    FanInfo                 */
/*  DELETE  /fans/{id}               FanId           StatusResponse          */
/*  GET     /sources                 —               SourceList              */
/*  POST    /sources                 SourceCreateReq SourceInfo              */
/*  DELETE  /sources/{id}            FanId           StatusResponse          */
/*  POST    /sources/temp            ManualTempReq   StatusResponse          */
/*  GET     /curves                  —               CurveList               */
/*  GET     /curves/{id}             FanId           CurveInfo               */
/*  POST    /curves                  CurveCreateReq  CurveInfo               */
/*  PUT     /curves/{id}             CurveUpdateReq  CurveInfo               */
/*  DELETE  /curves/{id}             FanId           StatusResponse          */
/*  GET     /schedules               —               ScheduleList            */
/*  POST    /schedules               ScheduleCreate.. ScheduleInfo           */
/*  PUT     /schedules/{id}          ScheduleUpdate.. ScheduleInfo           */
/*  DELETE  /schedules/{id}          FanId           StatusResponse          */
/*  GET     /wifi/scan               —               WifiScanResult          */
/*  POST    /wifi/connect            WifiConnectReq   StatusResponse         */
/*  GET     /wifi/status             —               WifiStatus              */
/*  GET     /system/info             —               SystemInfo              */
/*                                                                           */
/*  OBSERVE /fans                    —               FanList (periodic)      */
/*  OBSERVE /sources                 —               SourceList (periodic)   */
/*  OBSERVE /system/info             —               SystemInfo (periodic)   */
/* ======================================================================== */

typedef struct f_coap *f_coap_handle_t;

esp_err_t f_coap_init(f_coap_handle_t *handle, f_fan_handle_t fan,
                      f_source_handle_t source, f_curve_handle_t curve,
                      f_schedule_handle_t schedule, f_config_handle_t config);
esp_err_t f_coap_start(f_coap_handle_t handle);
esp_err_t f_coap_stop(f_coap_handle_t handle);

#ifdef __cplusplus
}
#endif
