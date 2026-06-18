/* Auto-generated from proto/espfm.proto — nanopb generator */
#ifndef ESPFM_PB_H
#define ESPFM_PB_H
#include "pb.h"
#include "f_core.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Map f_core.h enums to protobuf enum names */
typedef fan_mode_t       FanMode;
typedef source_type_t    SourceType;
typedef source_status_t  SourceStatus;
typedef fan_alarm_t      FanAlarm;
#define FAN_MODE_MANUAL   0
#define FAN_MODE_AUTO     1

typedef struct { float temp_c; uint32_t duty; } CurvePoint;
#define CurvePoint_init_default {0.0f,0}
#define CurvePoint_init_zero     {0.0f,0}

typedef struct {
    uint32_t id; char name[16]; FanMode mode; uint32_t duty; uint32_t rpm;
    bool enabled, inverted; uint32_t pwm_gpio, tach_gpio;
    uint32_t source_id, curve_id, schedule_id, group_id; FanAlarm alarm;
} FanInfo;
#define FanInfo_init_default {0,"",0,0,0,0,0,0,0,0,0,0,0,0}

typedef struct {
    uint32_t id; char name[16]; SourceType type; SourceStatus status;
    float temp_c; uint32_t gpio;
} SourceInfo;
#define SourceInfo_init_default {0,"",0,0,0.0f,0}

typedef struct {
    uint32_t id; char name[16]; pb_size_t points_count; CurvePoint points[10];
} CurveInfo;
#define CurveInfo_init_default {0,"",0,{CurvePoint_init_default}}

typedef struct {
    uint32_t id, fan_id, duty, start_min, end_min; bool enabled;
} ScheduleInfo;
#define ScheduleInfo_init_default {0,0,0,0,0,0}

typedef struct { char ssid[33]; int32_t rssi; uint32_t channel, authmode; } WifiApRecord;
#define WifiApRecord_init_default {"",0,0,0}

typedef struct {
    char version[12]; uint32_t uptime_s, heap_free, fan_count, source_count, curve_count, schedule_count;
} SystemInfo;
#define SystemInfo_init_default {"",0,0,0,0,0,0}

typedef struct { bool sta_connected; char sta_ip[16], ap_ip[16]; } WifiStatus;
#define WifiStatus_init_default {0,"",""}

typedef struct { pb_size_t fans_count; FanInfo fans[8]; } FanList;
#define FanList_init_default {0,{FanInfo_init_default}}

typedef struct { uint32_t pwm_gpio, tach_gpio; char name[16]; } FanCreateRequest;
#define FanCreateRequest_init_default {0,0,""}

typedef struct {
    uint32_t id; bool has_mode, has_duty, has_source_id, has_curve_id, has_schedule_id, has_group_id, has_inverted;
    FanMode mode; uint32_t duty, source_id, curve_id, schedule_id, group_id; bool inverted;
} FanUpdateRequest;
#define FanUpdateRequest_init_default {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}

typedef struct { uint32_t id; } FanId;
#define FanId_init_default {0}

typedef struct { pb_size_t sources_count; SourceInfo sources[8]; } SourceList;
#define SourceList_init_default {0,{SourceInfo_init_default}}

typedef struct { SourceType type; char name[16]; uint32_t gpio; } SourceCreateRequest;
#define SourceCreateRequest_init_default {0,"",0}

typedef struct { uint32_t id; float temp_c; } ManualTempRequest;
#define ManualTempRequest_init_default {0,0.0f}

typedef struct { pb_size_t curves_count; CurveInfo curves[16]; } CurveList;
#define CurveList_init_default {0,{CurveInfo_init_default}}

typedef struct { char name[16]; pb_size_t points_count; CurvePoint points[10]; } CurveCreateRequest;
#define CurveCreateRequest_init_default {"",0,{CurvePoint_init_default}}

typedef struct { uint32_t id; char name[16]; pb_size_t points_count; CurvePoint points[10]; } CurveUpdateRequest;
#define CurveUpdateRequest_init_default {0,"",0,{CurvePoint_init_default}}

typedef struct { pb_size_t schedules_count; ScheduleInfo schedules[8]; } ScheduleList;
#define ScheduleList_init_default {0,{ScheduleInfo_init_default}}

typedef struct { uint32_t fan_id, duty, start_min, end_min; bool enabled; } ScheduleCreateRequest;
#define ScheduleCreateRequest_init_default {0,0,0,0,0}

typedef struct { uint32_t id, fan_id, duty, start_min, end_min; bool enabled; } ScheduleUpdateRequest;
#define ScheduleUpdateRequest_init_default {0,0,0,0,0,0}

typedef struct { pb_size_t aps_count; WifiApRecord aps[16]; } WifiScanResult;
#define WifiScanResult_init_default {0,{WifiApRecord_init_default}}

typedef struct { char ssid[33]; char password[64]; } WifiConnectRequest;
#define WifiConnectRequest_init_default {"",""}

typedef struct { bool ok; uint32_t error_code; char error_msg[64]; } StatusResponse;
#define StatusResponse_init_default {0,0,""}

typedef struct { char _dummy; } Empty;
#define Empty_init_default {0}

/* Field descriptors */
extern const pb_field_t FanInfo_fields[15], SourceInfo_fields[7], CurvePoint_fields[3], CurveInfo_fields[4];
extern const pb_field_t ScheduleInfo_fields[7], WifiApRecord_fields[5], SystemInfo_fields[8], WifiStatus_fields[4];
extern const pb_field_t FanList_fields[2], FanCreateRequest_fields[4], FanUpdateRequest_fields[9], FanId_fields[2];
extern const pb_field_t SourceList_fields[2], SourceCreateRequest_fields[4], ManualTempRequest_fields[3];
extern const pb_field_t CurveList_fields[2], CurveCreateRequest_fields[4], CurveUpdateRequest_fields[5];
extern const pb_field_t ScheduleList_fields[2], ScheduleCreateRequest_fields[6], ScheduleUpdateRequest_fields[7];
extern const pb_field_t WifiScanResult_fields[2], WifiConnectRequest_fields[3];
extern const pb_field_t Empty_fields[1], StatusResponse_fields[4];

#ifdef __cplusplus
}
#endif
#endif
