/* Field descriptors — generated from proto/espfm.proto */
#include "pb.h"
#include "espfm.pb.h"

const pb_field_t FanInfo_fields[15] = {
    PB_FIELD(1,UINT32,REQUIRED,STATIC,id,FanInfo,id,0),
    PB_FIELD(2,STRING,OPTIONAL,STATIC,id,FanInfo,name,id,0),
    PB_FIELD(3,UENUM,OPTIONAL,STATIC,id,FanInfo,mode,name,0),
    PB_FIELD(4,UINT32,OPTIONAL,STATIC,id,FanInfo,duty,mode,0),
    PB_FIELD(5,UINT32,OPTIONAL,STATIC,id,FanInfo,rpm,duty,0),
    PB_FIELD(6,BOOL,OPTIONAL,STATIC,id,FanInfo,enabled,rpm,0),
    PB_FIELD(7,BOOL,OPTIONAL,STATIC,id,FanInfo,inverted,enabled,0),
    PB_FIELD(8,UINT32,OPTIONAL,STATIC,id,FanInfo,pwm_gpio,inverted,0),
    PB_FIELD(9,UINT32,OPTIONAL,STATIC,id,FanInfo,tach_gpio,pwm_gpio,0),
    PB_FIELD(10,UINT32,OPTIONAL,STATIC,id,FanInfo,source_id,tach_gpio,0),
    PB_FIELD(11,UINT32,OPTIONAL,STATIC,id,FanInfo,curve_id,source_id,0),
    PB_FIELD(12,UINT32,OPTIONAL,STATIC,id,FanInfo,schedule_id,curve_id,0),
    PB_FIELD(13,UINT32,OPTIONAL,STATIC,id,FanInfo,group_id,schedule_id,0),
    PB_FIELD(14,UENUM,OPTIONAL,STATIC,id,FanInfo,alarm,group_id,0),
    PB_LAST_FIELD
};

const pb_field_t SourceInfo_fields[7] = {
    PB_FIELD(1,UINT32,REQUIRED,STATIC,id,SourceInfo,id,0),
    PB_FIELD(2,STRING,OPTIONAL,STATIC,id,SourceInfo,name,id,0),
    PB_FIELD(3,UENUM,OPTIONAL,STATIC,id,SourceInfo,type,name,0),
    PB_FIELD(4,UENUM,OPTIONAL,STATIC,id,SourceInfo,status,type,0),
    PB_FIELD(5,FLOAT,OPTIONAL,STATIC,id,SourceInfo,temp_c,status,0),
    PB_FIELD(6,UINT32,OPTIONAL,STATIC,id,SourceInfo,gpio,temp_c,0),
    PB_LAST_FIELD
};

const pb_field_t CurvePoint_fields[3] = {
    PB_FIELD(1,FLOAT,OPTIONAL,STATIC,temp_c,CurvePoint,temp_c,0),
    PB_FIELD(2,UINT32,OPTIONAL,STATIC,temp_c,CurvePoint,duty,temp_c,0),
    PB_LAST_FIELD
};

const pb_field_t CurveInfo_fields[4] = {
    PB_FIELD(1,UINT32,REQUIRED,STATIC,id,CurveInfo,id,0),
    PB_FIELD(2,STRING,OPTIONAL,STATIC,id,CurveInfo,name,id,0),
    PB_FIELD(3,MESSAGE,REPEATED,STATIC,id,CurveInfo,points,name,&CurvePoint_fields[0]),
    PB_LAST_FIELD
};

const pb_field_t ScheduleInfo_fields[7] = {
    PB_FIELD(1,UINT32,REQUIRED,STATIC,id,ScheduleInfo,id,0),
    PB_FIELD(2,UINT32,OPTIONAL,STATIC,id,ScheduleInfo,fan_id,id,0),
    PB_FIELD(3,UINT32,OPTIONAL,STATIC,id,ScheduleInfo,duty,fan_id,0),
    PB_FIELD(4,UINT32,OPTIONAL,STATIC,id,ScheduleInfo,start_min,duty,0),
    PB_FIELD(5,UINT32,OPTIONAL,STATIC,id,ScheduleInfo,end_min,start_min,0),
    PB_FIELD(6,BOOL,OPTIONAL,STATIC,id,ScheduleInfo,enabled,end_min,0),
    PB_LAST_FIELD
};

const pb_field_t WifiApRecord_fields[5] = {
    PB_FIELD(1,STRING,OPTIONAL,STATIC,ssid,WifiApRecord,ssid,0),
    PB_FIELD(2,SINT32,OPTIONAL,STATIC,ssid,WifiApRecord,rssi,ssid,0),
    PB_FIELD(3,UINT32,OPTIONAL,STATIC,ssid,WifiApRecord,channel,rssi,0),
    PB_FIELD(4,UINT32,OPTIONAL,STATIC,ssid,WifiApRecord,authmode,channel,0),
    PB_LAST_FIELD
};

const pb_field_t SystemInfo_fields[8] = {
    PB_FIELD(1,STRING,OPTIONAL,STATIC,version,SystemInfo,version,0),
    PB_FIELD(2,UINT32,OPTIONAL,STATIC,version,SystemInfo,uptime_s,version,0),
    PB_FIELD(3,UINT32,OPTIONAL,STATIC,version,SystemInfo,heap_free,uptime_s,0),
    PB_FIELD(4,UINT32,OPTIONAL,STATIC,version,SystemInfo,fan_count,heap_free,0),
    PB_FIELD(5,UINT32,OPTIONAL,STATIC,version,SystemInfo,source_count,fan_count,0),
    PB_FIELD(6,UINT32,OPTIONAL,STATIC,version,SystemInfo,curve_count,source_count,0),
    PB_FIELD(7,UINT32,OPTIONAL,STATIC,version,SystemInfo,schedule_count,curve_count,0),
    PB_LAST_FIELD
};

const pb_field_t WifiStatus_fields[4] = {
    PB_FIELD(1,BOOL,OPTIONAL,STATIC,sta_connected,WifiStatus,sta_connected,0),
    PB_FIELD(2,STRING,OPTIONAL,STATIC,sta_connected,WifiStatus,sta_ip,sta_connected,0),
    PB_FIELD(3,STRING,OPTIONAL,STATIC,sta_connected,WifiStatus,ap_ip,sta_ip,0),
    PB_LAST_FIELD
};

const pb_field_t FanList_fields[2] = {
    PB_FIELD(1,MESSAGE,REPEATED,STATIC,fans,FanList,fans,0,&FanInfo_fields[0]),
    PB_LAST_FIELD
};

const pb_field_t FanCreateRequest_fields[4] = {
    PB_FIELD(1,UINT32,REQUIRED,STATIC,pwm_gpio,FanCreateRequest,pwm_gpio,0),
    PB_FIELD(2,UINT32,OPTIONAL,STATIC,pwm_gpio,FanCreateRequest,tach_gpio,pwm_gpio,0),
    PB_FIELD(3,STRING,REQUIRED,STATIC,pwm_gpio,FanCreateRequest,name,tach_gpio,0),
    PB_LAST_FIELD
};

const pb_field_t FanUpdateRequest_fields[9] = {
    PB_FIELD(1,UINT32,REQUIRED,STATIC,id,FanUpdateRequest,id,0),
    PB_FIELD(2,UENUM,OPTIONAL,STATIC,id,FanUpdateRequest,mode,id,&FanUpdateRequest.has_mode),
    PB_FIELD(3,UINT32,OPTIONAL,STATIC,id,FanUpdateRequest,duty,mode,&FanUpdateRequest.has_duty),
    PB_FIELD(4,UINT32,OPTIONAL,STATIC,id,FanUpdateRequest,source_id,duty,&FanUpdateRequest.has_source_id),
    PB_FIELD(5,UINT32,OPTIONAL,STATIC,id,FanUpdateRequest,curve_id,source_id,&FanUpdateRequest.has_curve_id),
    PB_FIELD(6,UINT32,OPTIONAL,STATIC,id,FanUpdateRequest,schedule_id,curve_id,&FanUpdateRequest.has_schedule_id),
    PB_FIELD(7,UINT32,OPTIONAL,STATIC,id,FanUpdateRequest,group_id,schedule_id,&FanUpdateRequest.has_group_id),
    PB_FIELD(8,BOOL,OPTIONAL,STATIC,id,FanUpdateRequest,inverted,group_id,&FanUpdateRequest.has_inverted),
    PB_LAST_FIELD
};

const pb_field_t FanId_fields[2] = {
    PB_FIELD(1,UINT32,REQUIRED,STATIC,id,FanId,id,0),
    PB_LAST_FIELD
};

const pb_field_t SourceList_fields[2] = {
    PB_FIELD(1,MESSAGE,REPEATED,STATIC,sources,SourceList,sources,0,&SourceInfo_fields[0]),
    PB_LAST_FIELD
};

const pb_field_t SourceCreateRequest_fields[4] = {
    PB_FIELD(1,UENUM,REQUIRED,STATIC,type,SourceCreateRequest,type,0),
    PB_FIELD(2,STRING,REQUIRED,STATIC,type,SourceCreateRequest,name,type,0),
    PB_FIELD(3,UINT32,OPTIONAL,STATIC,type,SourceCreateRequest,gpio,name,0),
    PB_LAST_FIELD
};

const pb_field_t ManualTempRequest_fields[3] = {
    PB_FIELD(1,UINT32,REQUIRED,STATIC,id,ManualTempRequest,id,0),
    PB_FIELD(2,FLOAT,REQUIRED,STATIC,id,ManualTempRequest,temp_c,id,0),
    PB_LAST_FIELD
};

const pb_field_t CurveList_fields[2] = {
    PB_FIELD(1,MESSAGE,REPEATED,STATIC,curves,CurveList,curves,0,&CurveInfo_fields[0]),
    PB_LAST_FIELD
};

const pb_field_t CurveCreateRequest_fields[4] = {
    PB_FIELD(1,STRING,REQUIRED,STATIC,name,CurveCreateRequest,name,0),
    PB_FIELD(3,MESSAGE,REPEATED,STATIC,name,CurveCreateRequest,points,name,&CurvePoint_fields[0]),
    PB_LAST_FIELD
};

const pb_field_t CurveUpdateRequest_fields[5] = {
    PB_FIELD(1,UINT32,REQUIRED,STATIC,id,CurveUpdateRequest,id,0),
    PB_FIELD(2,STRING,OPTIONAL,STATIC,id,CurveUpdateRequest,name,id,0),
    PB_FIELD(3,MESSAGE,REPEATED,STATIC,id,CurveUpdateRequest,points,name,&CurvePoint_fields[0]),
    PB_LAST_FIELD
};

const pb_field_t ScheduleList_fields[2] = {
    PB_FIELD(1,MESSAGE,REPEATED,STATIC,schedules,ScheduleList,schedules,0,&ScheduleInfo_fields[0]),
    PB_LAST_FIELD
};

const pb_field_t ScheduleCreateRequest_fields[6] = {
    PB_FIELD(1,UINT32,REQUIRED,STATIC,fan_id,ScheduleCreateRequest,fan_id,0),
    PB_FIELD(2,UINT32,REQUIRED,STATIC,fan_id,ScheduleCreateRequest,duty,fan_id,0),
    PB_FIELD(3,UINT32,REQUIRED,STATIC,fan_id,ScheduleCreateRequest,start_min,duty,0),
    PB_FIELD(4,UINT32,REQUIRED,STATIC,fan_id,ScheduleCreateRequest,end_min,start_min,0),
    PB_FIELD(5,BOOL,OPTIONAL,STATIC,fan_id,ScheduleCreateRequest,enabled,end_min,0),
    PB_LAST_FIELD
};

const pb_field_t ScheduleUpdateRequest_fields[7] = {
    PB_FIELD(1,UINT32,REQUIRED,STATIC,id,ScheduleUpdateRequest,id,0),
    PB_FIELD(2,UINT32,OPTIONAL,STATIC,id,ScheduleUpdateRequest,fan_id,id,0),
    PB_FIELD(3,UINT32,OPTIONAL,STATIC,id,ScheduleUpdateRequest,duty,fan_id,0),
    PB_FIELD(4,UINT32,OPTIONAL,STATIC,id,ScheduleUpdateRequest,start_min,duty,0),
    PB_FIELD(5,UINT32,OPTIONAL,STATIC,id,ScheduleUpdateRequest,end_min,start_min,0),
    PB_FIELD(6,BOOL,OPTIONAL,STATIC,id,ScheduleUpdateRequest,enabled,end_min,0),
    PB_LAST_FIELD
};

const pb_field_t WifiScanResult_fields[2] = {
    PB_FIELD(1,MESSAGE,REPEATED,STATIC,aps,WifiScanResult,aps,0,&WifiApRecord_fields[0]),
    PB_LAST_FIELD
};

const pb_field_t WifiConnectRequest_fields[3] = {
    PB_FIELD(1,STRING,REQUIRED,STATIC,ssid,WifiConnectRequest,ssid,0),
    PB_FIELD(2,STRING,REQUIRED,STATIC,ssid,WifiConnectRequest,password,ssid,0),
    PB_LAST_FIELD
};

const pb_field_t Empty_fields[1] = { PB_LAST_FIELD };

const pb_field_t StatusResponse_fields[4] = {
    PB_FIELD(1,BOOL,REQUIRED,STATIC,ok,StatusResponse,ok,0),
    PB_FIELD(2,UINT32,OPTIONAL,STATIC,ok,StatusResponse,error_code,ok,0),
    PB_FIELD(3,STRING,OPTIONAL,STATIC,ok,StatusResponse,error_msg,error_code,0),
    PB_LAST_FIELD
};
