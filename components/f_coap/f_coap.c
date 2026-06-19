/* f_coap.c — CoAP+Protobuf server using microcoap + nanopb runtime */
#include "f_coap.h"
#include "f_constraints.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "coap.h"
#include "espfm.pb.h"
#include "pb.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "f_coap";
#define COAP_PORT 5683
#define COAP_MTU 1280
#define COAP_TASK_STACK 8192
#define COAP_TASK_PRIO 4

/* ---- PB field descriptors (inline, same proven format) ---- */
#define F(tag,type,htype,atype,first,struc,fld,prev,...) {tag,PB_LTYPE_##type,PB_HTYPE_##htype,PB_ATYPE_##atype,0,offsetof(struc,fld),SIZEOF_MEMBER(struc,fld),offsetof(struc,first),NULL}
#define FR(tag,type,htype,atype,first,struc,fld,prev,n,...) {tag,PB_LTYPE_##type,PB_HTYPE_##htype,PB_ATYPE_##atype,0,offsetof(struc,fld),ELEM_SIZE(struc,fld,n),offsetof(struc,first),NULL}
#define FH(tag,type,htype,atype,first,struc,fld,prev,hasptr) {tag,PB_LTYPE_##type,PB_HTYPE_##htype,PB_ATYPE_##atype,0,offsetof(struc,fld),SIZEOF_MEMBER(struc,fld),offsetof(struc,first),(const void*)hasptr}
#define FHR(tag,type,htype,atype,first,struc,fld,prev,n,hasptr) {tag,PB_LTYPE_##type,PB_HTYPE_##htype,PB_ATYPE_##atype,0,offsetof(struc,fld),ELEM_SIZE(struc,fld,n),offsetof(struc,first),(const void*)hasptr}
#define L {0,0,0,0,0,0,0,0,0}

#define SIZEOF_MEMBER(s,f) sizeof(((s*)0)->f)
#define ELEM_SIZE(s,f,n)  (sizeof(((s*)0)->f)/(n))
#define PB_LTYPE_UVARINT 0
#define PB_LTYPE_UINT32  0
#define PB_LTYPE_UENUM   0
#define PB_LTYPE_BOOL    0
#define PB_LTYPE_SVARINT 1
#define PB_LTYPE_SINT32  1
#define PB_LTYPE_FIXED32 2
#define PB_LTYPE_FLOAT   2
#define PB_LTYPE_STRING  4
#define PB_LTYPE_BYTES   4
#define PB_LTYPE_SUBMESSAGE 5
#define PB_LTYPE_MESSAGE 5
#define PB_HTYPE_REQUIRED 0
#define PB_HTYPE_OPTIONAL 1
#define PB_HTYPE_REPEATED 2
#define PB_ATYPE_STATIC 0

static const pb_field_t ff_FanInfo[]={F(1,UINT32,REQUIRED,STATIC,id,FanInfo,id,0),F(2,STRING,OPTIONAL,STATIC,id,FanInfo,name,id,0),F(3,UENUM,OPTIONAL,STATIC,id,FanInfo,mode,name,0),F(4,UINT32,OPTIONAL,STATIC,id,FanInfo,duty,mode,0),F(5,UINT32,OPTIONAL,STATIC,id,FanInfo,rpm,duty,0),F(6,BOOL,OPTIONAL,STATIC,id,FanInfo,enabled,rpm,0),F(7,BOOL,OPTIONAL,STATIC,id,FanInfo,inverted,enabled,0),F(8,UINT32,OPTIONAL,STATIC,id,FanInfo,pwm_gpio,inverted,0),F(9,UINT32,OPTIONAL,STATIC,id,FanInfo,tach_gpio,pwm_gpio,0),F(10,UINT32,OPTIONAL,STATIC,id,FanInfo,source_id,tach_gpio,0),F(11,UINT32,OPTIONAL,STATIC,id,FanInfo,curve_id,source_id,0),F(12,UINT32,OPTIONAL,STATIC,id,FanInfo,schedule_id,curve_id,0),F(13,UINT32,OPTIONAL,STATIC,id,FanInfo,group_id,schedule_id,0),F(14,UENUM,OPTIONAL,STATIC,id,FanInfo,alarm,group_id,0),L};
static const pb_field_t ff_FanList[]={FHR(1,MESSAGE,REPEATED,STATIC,fans,FanList,fans,0,8,&ff_FanInfo[0]),L};
static const pb_field_t ff_FanCreate[]={F(1,UINT32,REQUIRED,STATIC,pwm_gpio,FanCreateRequest,pwm_gpio,0),F(2,UINT32,OPTIONAL,STATIC,pwm_gpio,FanCreateRequest,tach_gpio,pwm_gpio,0),F(3,STRING,REQUIRED,STATIC,pwm_gpio,FanCreateRequest,name,tach_gpio,0),L};
static const pb_field_t ff_FanUpdate[]={F(1,UINT32,REQUIRED,STATIC,id,FanUpdateRequest,id,0),FH(2,UENUM,OPTIONAL,STATIC,id,FanUpdateRequest,mode,id,&((FanUpdateRequest*)0)->has_mode),FH(3,UINT32,OPTIONAL,STATIC,id,FanUpdateRequest,duty,mode,&((FanUpdateRequest*)0)->has_duty),FH(4,UINT32,OPTIONAL,STATIC,id,FanUpdateRequest,source_id,duty,&((FanUpdateRequest*)0)->has_source_id),FH(5,UINT32,OPTIONAL,STATIC,id,FanUpdateRequest,curve_id,source_id,&((FanUpdateRequest*)0)->has_curve_id),FH(6,UINT32,OPTIONAL,STATIC,id,FanUpdateRequest,schedule_id,curve_id,&((FanUpdateRequest*)0)->has_schedule_id),FH(7,UINT32,OPTIONAL,STATIC,id,FanUpdateRequest,group_id,schedule_id,&((FanUpdateRequest*)0)->has_group_id),FH(8,BOOL,OPTIONAL,STATIC,id,FanUpdateRequest,inverted,group_id,&((FanUpdateRequest*)0)->has_inverted),L};

static const pb_field_t ff_SrcInfo[]={F(1,UINT32,REQUIRED,STATIC,id,SourceInfo,id,0),F(2,STRING,OPTIONAL,STATIC,id,SourceInfo,name,id,0),F(3,UENUM,OPTIONAL,STATIC,id,SourceInfo,type,name,0),F(4,UENUM,OPTIONAL,STATIC,id,SourceInfo,status,type,0),F(5,FLOAT,OPTIONAL,STATIC,id,SourceInfo,temp_c,status,0),F(6,UINT32,OPTIONAL,STATIC,id,SourceInfo,gpio,temp_c,0),L};
static const pb_field_t ff_SrcList[]={FHR(1,MESSAGE,REPEATED,STATIC,sources,SourceList,sources,0,8,&ff_SrcInfo[0]),L};
static const pb_field_t ff_SrcCreate[]={F(1,UENUM,REQUIRED,STATIC,type,SourceCreateRequest,type,0),F(2,STRING,REQUIRED,STATIC,type,SourceCreateRequest,name,type,0),F(3,UINT32,OPTIONAL,STATIC,type,SourceCreateRequest,gpio,name,0),L};
static const pb_field_t ff_ManTemp[]={F(1,UINT32,REQUIRED,STATIC,id,ManualTempRequest,id,0),F(2,FLOAT,REQUIRED,STATIC,id,ManualTempRequest,temp_c,id,0),L};

__attribute__((unused)) static const pb_field_t ff_CurvePt[]={F(1,FLOAT,OPTIONAL,STATIC,temp_c,CurvePoint,temp_c,0),F(2,UINT32,OPTIONAL,STATIC,temp_c,CurvePoint,duty,temp_c,0),L};
static const pb_field_t ff_CurveInfo[]={F(1,UINT32,REQUIRED,STATIC,id,CurveInfo,id,0),F(2,STRING,OPTIONAL,STATIC,id,CurveInfo,name,id,0),FHR(3,MESSAGE,REPEATED,STATIC,id,CurveInfo,points,name,10,&ff_CurvePt[0]),L};
static const pb_field_t ff_CurveList[]={FHR(1,MESSAGE,REPEATED,STATIC,curves,CurveList,curves,0,16,&ff_CurveInfo[0]),L};
static const pb_field_t ff_CurveCreate[]={F(1,STRING,REQUIRED,STATIC,name,CurveCreateRequest,name,0),FHR(3,MESSAGE,REPEATED,STATIC,name,CurveCreateRequest,points,name,10,&ff_CurvePt[0]),L};
static const pb_field_t ff_CurveUpdate[]={F(1,UINT32,REQUIRED,STATIC,id,CurveUpdateRequest,id,0),F(2,STRING,OPTIONAL,STATIC,id,CurveUpdateRequest,name,id,0),FHR(3,MESSAGE,REPEATED,STATIC,id,CurveUpdateRequest,points,name,10,&ff_CurvePt[0]),L};

static const pb_field_t ff_SchedInfo[]={F(1,UINT32,REQUIRED,STATIC,id,ScheduleInfo,id,0),F(2,UINT32,OPTIONAL,STATIC,id,ScheduleInfo,fan_id,id,0),F(3,UINT32,OPTIONAL,STATIC,id,ScheduleInfo,duty,fan_id,0),F(4,UINT32,OPTIONAL,STATIC,id,ScheduleInfo,start_min,duty,0),F(5,UINT32,OPTIONAL,STATIC,id,ScheduleInfo,end_min,start_min,0),F(6,BOOL,OPTIONAL,STATIC,id,ScheduleInfo,enabled,end_min,0),L};
static const pb_field_t ff_SchedList[]={FHR(1,MESSAGE,REPEATED,STATIC,schedules,ScheduleList,schedules,0,8,&ff_SchedInfo[0]),L};
static const pb_field_t ff_SchedCreate[]={F(1,UINT32,REQUIRED,STATIC,fan_id,ScheduleCreateRequest,fan_id,0),F(2,UINT32,REQUIRED,STATIC,fan_id,ScheduleCreateRequest,duty,fan_id,0),F(3,UINT32,REQUIRED,STATIC,fan_id,ScheduleCreateRequest,start_min,duty,0),F(4,UINT32,REQUIRED,STATIC,fan_id,ScheduleCreateRequest,end_min,start_min,0),F(5,BOOL,OPTIONAL,STATIC,fan_id,ScheduleCreateRequest,enabled,end_min,0),L};
static const pb_field_t ff_SchedUpdate[]={F(1,UINT32,REQUIRED,STATIC,id,ScheduleUpdateRequest,id,0),F(2,UINT32,OPTIONAL,STATIC,id,ScheduleUpdateRequest,fan_id,id,0),F(3,UINT32,OPTIONAL,STATIC,id,ScheduleUpdateRequest,duty,fan_id,0),F(4,UINT32,OPTIONAL,STATIC,id,ScheduleUpdateRequest,start_min,duty,0),F(5,UINT32,OPTIONAL,STATIC,id,ScheduleUpdateRequest,end_min,start_min,0),F(6,BOOL,OPTIONAL,STATIC,id,ScheduleUpdateRequest,enabled,end_min,0),L};

__attribute__((unused)) static const pb_field_t ff_WifiAp[]={F(1,STRING,OPTIONAL,STATIC,ssid,WifiApRecord,ssid,0),F(2,SINT32,OPTIONAL,STATIC,ssid,WifiApRecord,rssi,ssid,0),F(3,UINT32,OPTIONAL,STATIC,ssid,WifiApRecord,channel,rssi,0),F(4,UINT32,OPTIONAL,STATIC,ssid,WifiApRecord,authmode,channel,0),L};
static const pb_field_t ff_WifiScan[]={FHR(1,MESSAGE,REPEATED,STATIC,aps,WifiScanResult,aps,0,16,&ff_WifiAp[0]),L};
static const pb_field_t ff_WifiConn[]={F(1,STRING,REQUIRED,STATIC,ssid,WifiConnectRequest,ssid,0),F(2,STRING,REQUIRED,STATIC,ssid,WifiConnectRequest,password,ssid,0),L};
static const pb_field_t ff_WifiStatus[]={F(1,BOOL,OPTIONAL,STATIC,sta_connected,WifiStatus,sta_connected,0),F(2,STRING,OPTIONAL,STATIC,sta_connected,WifiStatus,sta_ip,sta_connected,0),F(3,STRING,OPTIONAL,STATIC,sta_connected,WifiStatus,ap_ip,sta_ip,0),L};
static const pb_field_t ff_SysInfo[]={F(1,STRING,OPTIONAL,STATIC,version,SystemInfo,version,0),F(2,UINT32,OPTIONAL,STATIC,version,SystemInfo,uptime_s,version,0),F(3,UINT32,OPTIONAL,STATIC,version,SystemInfo,heap_free,uptime_s,0),F(4,UINT32,OPTIONAL,STATIC,version,SystemInfo,fan_count,heap_free,0),F(5,UINT32,OPTIONAL,STATIC,version,SystemInfo,source_count,fan_count,0),F(6,UINT32,OPTIONAL,STATIC,version,SystemInfo,curve_count,source_count,0),F(7,UINT32,OPTIONAL,STATIC,version,SystemInfo,schedule_count,curve_count,0),L};
static const pb_field_t ff_Status[]={F(1,BOOL,REQUIRED,STATIC,ok,StatusResponse,ok,0),F(2,UINT32,OPTIONAL,STATIC,ok,StatusResponse,error_code,ok,0),F(3,STRING,OPTIONAL,STATIC,ok,StatusResponse,error_msg,error_code,0),L};


/* ---- PB runtime (minimal) ---- */
static bool pb_enc(const void *m, const pb_field_t f[], uint8_t *b, size_t c, size_t *o) { pb_ostream_t s=pb_ostream_from_buffer(b,c); bool ok=pb_encode(&s,f,m); if(ok)*o=s.bytes_written; return ok; }
static bool pb_dec(const uint8_t *b, size_t l, void *m, const pb_field_t f[]) { pb_istream_t s=pb_istream_from_buffer(b,l); return pb_decode(&s,f,m); }

static void f2pb(const f_fan_info_t *fi, FanInfo *pb) { *pb=(FanInfo)FanInfo_init_default; pb->id=fi->id; pb->mode=(FanMode)fi->mode; pb->duty=fi->duty; pb->rpm=fi->rpm; pb->enabled=fi->enabled; pb->inverted=fi->inverted; pb->pwm_gpio=fi->pwm_gpio; pb->tach_gpio=fi->tach_gpio; pb->source_id=fi->source_id; pb->curve_id=fi->curve_id; pb->schedule_id=fi->schedule_id; pb->group_id=fi->group_id; pb->alarm=(FanAlarm)fi->alarm; strncpy(pb->name,fi->name,15); }
static void s2pb(const f_source_info_t *si, SourceInfo *pb) { *pb=(SourceInfo)SourceInfo_init_default; pb->id=si->id; pb->type=(SourceType)si->type; pb->status=(SourceStatus)si->status; pb->temp_c=si->temp_c; pb->gpio=si->gpio; strncpy(pb->name,si->name,15); }
static void c2pb(const f_curve_info_t *ci, CurveInfo *pb) { *pb=(CurveInfo)CurveInfo_init_default; pb->id=ci->id; pb->points_count=ci->num_points; strncpy(pb->name,ci->name,15); for(int i=0;i<ci->num_points;i++){pb->points[i].temp_c=ci->points[i].temp_c;pb->points[i].duty=ci->points[i].duty;} }
static void sc2pb(const f_schedule_info_t *si, ScheduleInfo *pb) { *pb=(ScheduleInfo)ScheduleInfo_init_default; pb->id=si->id; pb->fan_id=si->fan_id; pb->duty=si->duty; pb->start_min=si->start_min; pb->end_min=si->end_min; pb->enabled=si->enabled; }

/* ---- f_coap struct ---- */
struct f_coap { int sock; bool running; TaskHandle_t task; f_fan_handle_t fan; f_source_handle_t source; f_curve_handle_t curve; f_schedule_handle_t schedule; f_config_handle_t config; };

/* Global handle for microcoap handler (single-instance server) */
static struct f_coap *g_coap = NULL;

/* ---- Microcoap endpoint handler ---- */
static int micro_handler(coap_rw_buffer_t *scratch, const coap_packet_t *inpkt, coap_packet_t *outpkt, uint8_t id_hi, uint8_t id_lo)
{
    struct f_coap *h = g_coap;
    static uint8_t rx[1024], tx[1024];
    int rsp_code = COAP_RSPCODE_CONTENT;

    if(inpkt->payload.len > 0 && inpkt->payload.len < 1024)
        memcpy(rx, inpkt->payload.p, inpkt->payload.len);

    uint8_t count = 0;
    const coap_option_t *opt = coap_findOptions(inpkt, COAP_OPTION_URI_PATH, &count);
    if(!opt || count == 0) { rsp_code = COAP_RSPCODE_NOT_FOUND; goto send_empty; }

    char seg[4][32]; int nseg = count;
    for(int i=0; i<count && i<4; i++) {
        int l = opt[i].buf.len; if(l>31) l=31;
        memcpy(seg[i], opt[i].buf.p, l); seg[i][l]=0;
    }

    int method = inpkt->hdr.code;
    uint8_t id = (nseg >= 2) ? (uint8_t)atoi(seg[1]) : 0;
    void *rsp_msg = NULL; const pb_field_t *rsp_fd = NULL;

    /* Fan routes */
    if(!strcmp(seg[0],"fans")) {
        if(method == COAP_METHOD_GET && nseg == 1) {
            static FanList list; list = (FanList)FanList_init_default;
            for(uint8_t i=0;i<8;i++){f_fan_info_t fi;if(f_fan_get_info(h->fan,i,&fi)==ESP_OK)f2pb(&fi,&list.fans[list.fans_count++]);}
            rsp_msg = &list; rsp_fd = ff_FanList;
        } else if(method == COAP_METHOD_GET && nseg == 2) {
            f_fan_info_t fi; if(f_fan_get_info(h->fan,id,&fi)!=ESP_OK){rsp_code=COAP_RSPCODE_NOT_FOUND;goto send_empty;}
            static FanInfo pb; f2pb(&fi,&pb); rsp_msg=&pb; rsp_fd=ff_FanInfo;
        } else if(method == COAP_METHOD_POST && nseg == 1) {
            FanCreateRequest cr=FanCreateRequest_init_default;
            if(!pb_dec(rx,inpkt->payload.len,&cr,ff_FanCreate)){rsp_code=COAP_RSPCODE_BAD_REQUEST;goto send_empty;}
            uint8_t nid; if(f_fan_add(h->fan,cr.pwm_gpio,cr.tach_gpio,cr.name,&nid)!=ESP_OK){rsp_code=COAP_RSPCODE_BAD_REQUEST;goto send_empty;}
            if(h->config)f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
            f_fan_info_t fi; f_fan_get_info(h->fan,nid,&fi); static FanInfo pb; f2pb(&fi,&pb); rsp_msg=&pb; rsp_fd=ff_FanInfo;
            rsp_code = MAKE_RSPCODE(2,1);
        } else if(method == COAP_METHOD_PUT && nseg == 2) {
            FanUpdateRequest ur=FanUpdateRequest_init_default;
            if(!pb_dec(rx,inpkt->payload.len,&ur,ff_FanUpdate)){rsp_code=COAP_RSPCODE_BAD_REQUEST;goto send_empty;}
            f_fan_info_t fi; if(f_fan_get_info(h->fan,id,&fi)!=ESP_OK){rsp_code=COAP_RSPCODE_NOT_FOUND;goto send_empty;}
            if(ur.has_mode)f_fan_set_mode(h->fan,id,(fan_mode_t)ur.mode);
            if(ur.has_duty)f_fan_set_duty(h->fan,id,(uint8_t)ur.duty);
            if(ur.has_source_id)f_fan_set_source(h->fan,id,(uint8_t)ur.source_id);
            if(ur.has_curve_id)f_fan_set_curve(h->fan,id,(uint8_t)ur.curve_id);
            if(ur.has_schedule_id)f_fan_set_schedule(h->fan,id,(uint8_t)ur.schedule_id);
            if(ur.has_group_id)f_fan_set_group(h->fan,id,(uint8_t)ur.group_id);
            if(ur.has_inverted)f_fan_set_inverted(h->fan,id,ur.inverted);
            if(h->config)f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
            f_fan_get_info(h->fan,id,&fi); static FanInfo pb; f2pb(&fi,&pb); rsp_msg=&pb; rsp_fd=ff_FanInfo;
            rsp_code = MAKE_RSPCODE(2,4);
        } else if(method == COAP_METHOD_DELETE && nseg == 2) {
            if(f_fan_remove(h->fan,id)!=ESP_OK){rsp_code=COAP_RSPCODE_NOT_FOUND;goto send_empty;}
            if(h->config)f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
            static StatusResponse sr; sr.ok=true; rsp_msg=&sr; rsp_fd=ff_Status; rsp_code=MAKE_RSPCODE(2,2);
        } else { rsp_code = COAP_RSPCODE_NOT_FOUND; goto send_empty; }
    }
    /* Source routes */
    else if(!strcmp(seg[0],"sources")) {
        if(nseg==2 && !strcmp(seg[1],"temp") && method==COAP_METHOD_POST) {
            ManualTempRequest mt=ManualTempRequest_init_default;
            if(!pb_dec(rx,inpkt->payload.len,&mt,ff_ManTemp)){rsp_code=COAP_RSPCODE_BAD_REQUEST;goto send_empty;}
            if(f_source_update_manual(h->source,mt.id,mt.temp_c)!=ESP_OK){rsp_code=COAP_RSPCODE_BAD_REQUEST;goto send_empty;}
            if(h->config)f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
            static StatusResponse sr; sr.ok=true; rsp_msg=&sr; rsp_fd=ff_Status; rsp_code=MAKE_RSPCODE(2,4);
        } else if(method==COAP_METHOD_GET && nseg==1) {
            static SourceList list; list = (SourceList)SourceList_init_default;
            for(uint8_t i=0;i<8;i++){f_source_info_t si;if(f_source_get_info(h->source,i,&si)==ESP_OK)s2pb(&si,&list.sources[list.sources_count++]);}
            rsp_msg=&list; rsp_fd=ff_SrcList;
        } else if(method==COAP_METHOD_POST && nseg==1) {
            SourceCreateRequest cr=SourceCreateRequest_init_default;
            if(!pb_dec(rx,inpkt->payload.len,&cr,ff_SrcCreate)){rsp_code=COAP_RSPCODE_BAD_REQUEST;goto send_empty;}
            uint8_t nid; if(f_source_add(h->source,(source_type_t)cr.type,cr.gpio,cr.name,&nid)!=ESP_OK){rsp_code=COAP_RSPCODE_BAD_REQUEST;goto send_empty;}
            if(h->config)f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
            static SourceInfo pb; f_source_info_t si; f_source_get_info(h->source,nid,&si); s2pb(&si,&pb); rsp_msg=&pb; rsp_fd=ff_SrcInfo;
            rsp_code = MAKE_RSPCODE(2,1);
        } else if(method==COAP_METHOD_DELETE && nseg==2) {
            if(f_source_remove(h->source,id)!=ESP_OK){rsp_code=COAP_RSPCODE_NOT_FOUND;goto send_empty;}
            if(h->config)f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
            static StatusResponse sr; sr.ok=true; rsp_msg=&sr; rsp_fd=ff_Status; rsp_code=MAKE_RSPCODE(2,2);
        } else { rsp_code = COAP_RSPCODE_NOT_FOUND; goto send_empty; }
    }
    /* Curve routes */
    else if(!strcmp(seg[0],"curves")) {
        if(method==COAP_METHOD_GET && nseg==1) {
            static CurveList list; list = (CurveList)CurveList_init_default;
            for(uint8_t i=0;i<16;i++){f_curve_info_t ci;if(f_curve_get_info(h->curve,i,&ci)==ESP_OK)c2pb(&ci,&list.curves[list.curves_count++]);}
            rsp_msg=&list; rsp_fd=ff_CurveList;
        } else if(method==COAP_METHOD_GET && nseg==2) {
            f_curve_info_t ci; if(f_curve_get_info(h->curve,id,&ci)!=ESP_OK){rsp_code=COAP_RSPCODE_NOT_FOUND;goto send_empty;}
            static CurveInfo pb; c2pb(&ci,&pb); rsp_msg=&pb; rsp_fd=ff_CurveInfo;
        } else if(method==COAP_METHOD_POST && nseg==1) {
            CurveCreateRequest cr=CurveCreateRequest_init_default;
            if(!pb_dec(rx,inpkt->payload.len,&cr,ff_CurveCreate)){rsp_code=COAP_RSPCODE_BAD_REQUEST;goto send_empty;}
            f_curve_info_t ci={0}; strncpy(ci.name,cr.name,15); ci.num_points=cr.points_count;
            for(int i=0;i<cr.points_count;i++){ci.points[i].temp_c=cr.points[i].temp_c;ci.points[i].duty=(uint8_t)cr.points[i].duty;}
            uint8_t nid; if(f_curve_upsert(h->curve,&ci,&nid)!=ESP_OK){rsp_code=COAP_RSPCODE_BAD_REQUEST;goto send_empty;}
            if(h->config)f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
            f_curve_get_info(h->curve,nid,&ci); static CurveInfo pb; c2pb(&ci,&pb); rsp_msg=&pb; rsp_fd=ff_CurveInfo;
            rsp_code=MAKE_RSPCODE(2,1);
        } else if(method==COAP_METHOD_PUT && nseg==2) {
            CurveUpdateRequest ur=CurveUpdateRequest_init_default;
            if(!pb_dec(rx,inpkt->payload.len,&ur,ff_CurveUpdate)){rsp_code=COAP_RSPCODE_BAD_REQUEST;goto send_empty;}
            f_curve_info_t ci={0}; ci.id=id; strncpy(ci.name,ur.name,15); ci.num_points=ur.points_count;
            for(int i=0;i<ur.points_count;i++){ci.points[i].temp_c=ur.points[i].temp_c;ci.points[i].duty=(uint8_t)ur.points[i].duty;}
            uint8_t oid; if(f_curve_upsert(h->curve,&ci,&oid)!=ESP_OK){rsp_code=COAP_RSPCODE_BAD_REQUEST;goto send_empty;}
            if(h->config)f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
            f_curve_get_info(h->curve,oid,&ci); static CurveInfo pb; c2pb(&ci,&pb); rsp_msg=&pb; rsp_fd=ff_CurveInfo;
            rsp_code=MAKE_RSPCODE(2,4);
        } else if(method==COAP_METHOD_DELETE && nseg==2) {
            if(f_curve_remove(h->curve,id)!=ESP_OK){rsp_code=COAP_RSPCODE_NOT_FOUND;goto send_empty;}
            if(h->config)f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
            static StatusResponse sr; sr.ok=true; rsp_msg=&sr; rsp_fd=ff_Status; rsp_code=MAKE_RSPCODE(2,2);
        } else { rsp_code = COAP_RSPCODE_NOT_FOUND; goto send_empty; }
    }
    /* Schedule routes */
    else if(!strcmp(seg[0],"schedules")) {
        if(method==COAP_METHOD_GET && nseg==1) {
            static ScheduleList list; list = (ScheduleList)ScheduleList_init_default;
            for(uint8_t i=0;i<8;i++){f_schedule_info_t si;if(f_schedule_get_info(h->schedule,i,&si)==ESP_OK)sc2pb(&si,&list.schedules[list.schedules_count++]);}
            rsp_msg=&list; rsp_fd=ff_SchedList;
        } else if(method==COAP_METHOD_POST && nseg==1) {
            ScheduleCreateRequest cr=ScheduleCreateRequest_init_default;
            if(!pb_dec(rx,inpkt->payload.len,&cr,ff_SchedCreate)){rsp_code=COAP_RSPCODE_BAD_REQUEST;goto send_empty;}
            f_schedule_info_t si={.fan_id=(uint8_t)cr.fan_id,.duty=(uint8_t)cr.duty,.start_min=(uint16_t)cr.start_min,.end_min=(uint16_t)cr.end_min,.enabled=cr.enabled};
            uint8_t nid; if(f_schedule_add(h->schedule,&si,&nid)!=ESP_OK){rsp_code=COAP_RSPCODE_BAD_REQUEST;goto send_empty;}
            if(h->config)f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
            f_schedule_get_info(h->schedule,nid,&si); static ScheduleInfo pb; sc2pb(&si,&pb); rsp_msg=&pb; rsp_fd=ff_SchedInfo;
            rsp_code=MAKE_RSPCODE(2,1);
        } else if(method==COAP_METHOD_PUT && nseg==2) {
            ScheduleUpdateRequest ur=ScheduleUpdateRequest_init_default;
            if(!pb_dec(rx,inpkt->payload.len,&ur,ff_SchedUpdate)){rsp_code=COAP_RSPCODE_BAD_REQUEST;goto send_empty;}
            f_schedule_info_t si={.fan_id=(uint8_t)ur.fan_id,.duty=(uint8_t)ur.duty,.start_min=(uint16_t)ur.start_min,.end_min=(uint16_t)ur.end_min,.enabled=ur.enabled};
            if(f_schedule_update(h->schedule,id,&si)!=ESP_OK){rsp_code=COAP_RSPCODE_NOT_FOUND;goto send_empty;}
            if(h->config)f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
            f_schedule_get_info(h->schedule,id,&si); static ScheduleInfo pb; sc2pb(&si,&pb); rsp_msg=&pb; rsp_fd=ff_SchedInfo;
            rsp_code=MAKE_RSPCODE(2,4);
        } else if(method==COAP_METHOD_DELETE && nseg==2) {
            if(f_schedule_remove(h->schedule,id)!=ESP_OK){rsp_code=COAP_RSPCODE_NOT_FOUND;goto send_empty;}
            if(h->config)f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
            static StatusResponse sr; sr.ok=true; rsp_msg=&sr; rsp_fd=ff_Status; rsp_code=MAKE_RSPCODE(2,2);
        } else { rsp_code = COAP_RSPCODE_NOT_FOUND; goto send_empty; }
    }
    /* WiFi routes */
    else if(!strcmp(seg[0],"wifi")) {
        if(nseg==2 && !strcmp(seg[1],"scan") && method==COAP_METHOD_GET) {
            wifi_scan_config_t sc={.scan_type=WIFI_SCAN_TYPE_ACTIVE,.scan_time.active={.min=100,.max=300}};
            if(esp_wifi_scan_start(&sc,true)!=ESP_OK){rsp_code=MAKE_RSPCODE(5,3);goto send_empty;}
            uint16_t n=0; esp_wifi_scan_get_ap_num(&n);
            static WifiScanResult sr; sr = (WifiScanResult)WifiScanResult_init_default;
            if(n>0){wifi_ap_record_t *aps=calloc(n,sizeof(wifi_ap_record_t));
                if(aps){esp_wifi_scan_get_ap_records(&n,aps);
                    for(int i=0;i<n&&i<16;i++){WifiApRecord *ap=&sr.aps[sr.aps_count++];
                        strncpy(ap->ssid,(char*)aps[i].ssid,32);ap->rssi=aps[i].rssi;ap->channel=aps[i].primary;ap->authmode=aps[i].authmode;}
                    free(aps);}}
            rsp_msg=&sr; rsp_fd=ff_WifiScan;
        } else if(nseg==2 && !strcmp(seg[1],"connect") && method==COAP_METHOD_POST) {
            WifiConnectRequest cr=WifiConnectRequest_init_default;
            if(!pb_dec(rx,inpkt->payload.len,&cr,ff_WifiConn)){rsp_code=COAP_RSPCODE_BAD_REQUEST;goto send_empty;}
            wifi_config_t wc={0}; strncpy((char*)wc.sta.ssid,cr.ssid,32); strncpy((char*)wc.sta.password,cr.password,63);
            wc.sta.threshold.authmode=WIFI_AUTH_WPA2_PSK;
            if(esp_wifi_set_config(WIFI_IF_STA,&wc)!=ESP_OK){rsp_code=MAKE_RSPCODE(5,3);goto send_empty;}
            esp_wifi_disconnect(); esp_wifi_connect();
            static StatusResponse sr; sr.ok=true; rsp_msg=&sr; rsp_fd=ff_Status; rsp_code=MAKE_RSPCODE(2,4);
        } else if(nseg==2 && !strcmp(seg[1],"status") && method==COAP_METHOD_GET) {
            static WifiStatus ws; ws = (WifiStatus)WifiStatus_init_default;
            esp_netif_t *sta=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if(sta){esp_netif_ip_info_t ip;if(esp_netif_get_ip_info(sta,&ip)==ESP_OK&&ip.ip.addr!=0){ws.sta_connected=true;snprintf(ws.sta_ip,16,IPSTR,IP2STR(&ip.ip));}}
            strncpy(ws.ap_ip,"192.168.4.1",15); rsp_msg=&ws; rsp_fd=ff_WifiStatus;
        } else { rsp_code = COAP_RSPCODE_NOT_FOUND; goto send_empty; }
    }
    /* System info */
    else if(!strcmp(seg[0],"system") && nseg==2 && !strcmp(seg[1],"info") && method==COAP_METHOD_GET) {
        static SystemInfo si; si = (SystemInfo)SystemInfo_init_default;
        snprintf(si.version,12,"%d.%d.%d",ESPFM_VERSION_MAJOR,ESPFM_VERSION_MINOR,ESPFM_VERSION_PATCH);
        si.uptime_s=esp_timer_get_time()/1000000; si.heap_free=esp_get_free_heap_size();
        si.fan_count=h->fan?f_fan_get_count(h->fan):0; si.source_count=h->source?f_source_get_count(h->source):0;
        si.curve_count=h->curve?f_curve_get_count(h->curve):0; si.schedule_count=h->schedule?f_schedule_get_count(h->schedule):0;
        rsp_msg=&si; rsp_fd=ff_SysInfo;
    }
    else { rsp_code = COAP_RSPCODE_NOT_FOUND; goto send_empty; }

send_empty:
    if(rsp_msg && rsp_fd) {
        size_t o; pb_enc(rsp_msg, rsp_fd, tx, sizeof(tx), &o);
        return coap_make_response(scratch, outpkt, tx, o, id_hi, id_lo, &inpkt->tok, rsp_code, COAP_CONTENTTYPE_APPLICATION_OCTECT_STREAM);
    } else {
        return coap_make_response(scratch, outpkt, NULL, 0, id_hi, id_lo, &inpkt->tok, rsp_code, COAP_CONTENTTYPE_NONE);
    }
}
/* ---- CoAP server task ---- */
static void coap_task(void *arg) {
    struct f_coap *h=arg; g_coap = h;
    static uint8_t rx[COAP_MTU], tx[COAP_MTU];
    coap_rw_buffer_t scratch = {tx, COAP_MTU};
    ESP_LOGI(TAG,"CoAP on :%d",COAP_PORT);
    while(h->running) {
        struct sockaddr_in from; socklen_t fl=sizeof(from);
        int n=recvfrom(h->sock,rx,COAP_MTU,0,(struct sockaddr*)&from,&fl);
        if(n<4){vTaskDelay(pdMS_TO_TICKS(10));continue;}
        coap_packet_t inpkt, outpkt;
        if(coap_parse(&inpkt, rx, n)!=0) continue;
        micro_handler(&scratch, &inpkt, &outpkt, inpkt.hdr.id[0], inpkt.hdr.id[1]);
        size_t outlen = COAP_MTU;
        if(coap_build(tx, &outlen, &outpkt)==0)
            sendto(h->sock, tx, outlen, 0, (struct sockaddr*)&from, fl);
    }
    vTaskDelete(NULL);
}
static void _wc(void *a,esp_event_base_t b,int32_t i,void *d){struct f_coap *h=a;if(h)f_coap_start(h);}
static void _wd(void *a,esp_event_base_t b,int32_t i,void *d){struct f_coap *h=a;if(h)f_coap_stop(h);}
esp_err_t f_coap_init(f_coap_handle_t *handle,f_fan_handle_t fan,f_source_handle_t source,f_curve_handle_t curve,f_schedule_handle_t schedule,f_config_handle_t config) {
    if(!handle)return ESP_ERR_INVALID_ARG;
    struct f_coap *h=calloc(1,sizeof(*h));
    if(!h)return ESP_ERR_NO_MEM;
    h->fan=fan;h->source=source;h->curve=curve;h->schedule=schedule;h->config=config;h->sock=-1;
    esp_event_handler_register(ESPFM_EVENT,ESPFM_EVENT_WIFI_CONNECTED,_wc,h);
    esp_event_handler_register(ESPFM_EVENT,ESPFM_EVENT_WIFI_DISCONNECTED,_wd,h);
    *handle=h;ESP_LOGI(TAG,"CoAP init");return ESP_OK;
}
esp_err_t f_coap_start(f_coap_handle_t h) {
    if(!h||h->running)return ESP_OK;
    h->sock=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(h->sock<0)return ESP_FAIL;
    struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(COAP_PORT)};a.sin_addr.s_addr=INADDR_ANY;
    if(bind(h->sock,(struct sockaddr*)&a,sizeof(a))<0){close(h->sock);h->sock=-1;return ESP_FAIL;}
    h->running=true;xTaskCreate(coap_task,"coap",COAP_TASK_STACK,h,COAP_TASK_PRIO,&h->task);
    ESP_LOGI(TAG,"CoAP :%d",COAP_PORT);return ESP_OK;
}
esp_err_t f_coap_stop(f_coap_handle_t h) {
    if(!h||!h->running)return ESP_OK;
    h->running=false;
    if(h->sock>=0){close(h->sock);h->sock=-1;}
    ESP_LOGI(TAG,"CoAP stop");return ESP_OK;
}
