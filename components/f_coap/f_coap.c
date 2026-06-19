/* f_coap.c — CoAP + Protobuf server */
#include "f_coap.h"
#include "f_constraints.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "espfm.pb.h"
#include "pb.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "f_coap";
#define COAP_PORT 5683
#define COAP_MTU 1500
#define COAP_TASK_STACK 4096
#define COAP_TASK_PRIO 4

/* Inline field descriptors */
#define F(tag,type,htype,atype,first,struc,fld,prev,...) {tag,PB_LTYPE_##type,PB_HTYPE_##htype,PB_ATYPE_##atype,0,offsetof(struc,fld),SIZEOF_MEMBER(struc,fld),offsetof(struc,first), NULL}
#define FH(tag,type,htype,atype,first,struc,fld,prev,hasptr) {tag,PB_LTYPE_##type,PB_HTYPE_##htype,PB_ATYPE_##atype,0,offsetof(struc,fld),SIZEOF_MEMBER(struc,fld),offsetof(struc,first), (const void*)hasptr}
#define L {0,0,0,0,0,0,0,0,0}

static const pb_field_t ff_FanInfo[]={F(1,UINT32,REQUIRED,STATIC,id,FanInfo,id,0),F(2,STRING,OPTIONAL,STATIC,id,FanInfo,name,id,0),F(3,UENUM,OPTIONAL,STATIC,id,FanInfo,mode,name,0),F(4,UINT32,OPTIONAL,STATIC,id,FanInfo,duty,mode,0),F(5,UINT32,OPTIONAL,STATIC,id,FanInfo,rpm,duty,0),F(6,BOOL,OPTIONAL,STATIC,id,FanInfo,enabled,rpm,0),F(7,BOOL,OPTIONAL,STATIC,id,FanInfo,inverted,enabled,0),F(8,UINT32,OPTIONAL,STATIC,id,FanInfo,pwm_gpio,inverted,0),F(9,UINT32,OPTIONAL,STATIC,id,FanInfo,tach_gpio,pwm_gpio,0),F(10,UINT32,OPTIONAL,STATIC,id,FanInfo,source_id,tach_gpio,0),F(11,UINT32,OPTIONAL,STATIC,id,FanInfo,curve_id,source_id,0),F(12,UINT32,OPTIONAL,STATIC,id,FanInfo,schedule_id,curve_id,0),F(13,UINT32,OPTIONAL,STATIC,id,FanInfo,group_id,schedule_id,0),F(14,UENUM,OPTIONAL,STATIC,id,FanInfo,alarm,group_id,0),L};
static const pb_field_t ff_FanList[]={F(1,MESSAGE,REPEATED,STATIC,fans,FanList,fans,0,&ff_FanInfo[0]),L};
static const pb_field_t ff_FanCreate[]={F(1,UINT32,REQUIRED,STATIC,pwm_gpio,FanCreateRequest,pwm_gpio,0),F(2,UINT32,OPTIONAL,STATIC,pwm_gpio,FanCreateRequest,tach_gpio,pwm_gpio,0),F(3,STRING,REQUIRED,STATIC,pwm_gpio,FanCreateRequest,name,tach_gpio,0),L};
static const pb_field_t ff_FanUpdate[]={F(1,UINT32,REQUIRED,STATIC,id,FanUpdateRequest,id,0),FH(2,UENUM,OPTIONAL,STATIC,id,FanUpdateRequest,mode,id,&((FanUpdateRequest*)0)->has_mode),FH(3,UINT32,OPTIONAL,STATIC,id,FanUpdateRequest,duty,mode,&((FanUpdateRequest*)0)->has_duty),FH(4,UINT32,OPTIONAL,STATIC,id,FanUpdateRequest,source_id,duty,&((FanUpdateRequest*)0)->has_source_id),FH(5,UINT32,OPTIONAL,STATIC,id,FanUpdateRequest,curve_id,source_id,&((FanUpdateRequest*)0)->has_curve_id),FH(6,UINT32,OPTIONAL,STATIC,id,FanUpdateRequest,schedule_id,curve_id,&((FanUpdateRequest*)0)->has_schedule_id),FH(7,UINT32,OPTIONAL,STATIC,id,FanUpdateRequest,group_id,schedule_id,&((FanUpdateRequest*)0)->has_group_id),FH(8,BOOL,OPTIONAL,STATIC,id,FanUpdateRequest,inverted,group_id,&((FanUpdateRequest*)0)->has_inverted),L};

static const pb_field_t ff_SrcInfo[]={F(1,UINT32,REQUIRED,STATIC,id,SourceInfo,id,0),F(2,STRING,OPTIONAL,STATIC,id,SourceInfo,name,id,0),F(3,UENUM,OPTIONAL,STATIC,id,SourceInfo,type,name,0),F(4,UENUM,OPTIONAL,STATIC,id,SourceInfo,status,type,0),F(5,FLOAT,OPTIONAL,STATIC,id,SourceInfo,temp_c,status,0),F(6,UINT32,OPTIONAL,STATIC,id,SourceInfo,gpio,temp_c,0),L};
static const pb_field_t ff_SrcList[]={F(1,MESSAGE,REPEATED,STATIC,sources,SourceList,sources,0,&ff_SrcInfo[0]),L};
static const pb_field_t ff_SrcCreate[]={F(1,UENUM,REQUIRED,STATIC,type,SourceCreateRequest,type,0),F(2,STRING,REQUIRED,STATIC,type,SourceCreateRequest,name,type,0),F(3,UINT32,OPTIONAL,STATIC,type,SourceCreateRequest,gpio,name,0),L};
static const pb_field_t ff_ManTemp[]={F(1,UINT32,REQUIRED,STATIC,id,ManualTempRequest,id,0),F(2,FLOAT,REQUIRED,STATIC,id,ManualTempRequest,temp_c,id,0),L};

__attribute__((unused)) static const pb_field_t ff_CurvePt[]={F(1,FLOAT,OPTIONAL,STATIC,temp_c,CurvePoint,temp_c,0),F(2,UINT32,OPTIONAL,STATIC,temp_c,CurvePoint,duty,temp_c,0),L};
static const pb_field_t ff_CurveInfo[]={F(1,UINT32,REQUIRED,STATIC,id,CurveInfo,id,0),F(2,STRING,OPTIONAL,STATIC,id,CurveInfo,name,id,0),F(3,MESSAGE,REPEATED,STATIC,id,CurveInfo,points,name,&ff_CurvePt[0]),L};
static const pb_field_t ff_CurveList[]={F(1,MESSAGE,REPEATED,STATIC,curves,CurveList,curves,0,&ff_CurveInfo[0]),L};
static const pb_field_t ff_CurveCreate[]={F(1,STRING,REQUIRED,STATIC,name,CurveCreateRequest,name,0),F(3,MESSAGE,REPEATED,STATIC,name,CurveCreateRequest,points,name,&ff_CurvePt[0]),L};
static const pb_field_t ff_CurveUpdate[]={F(1,UINT32,REQUIRED,STATIC,id,CurveUpdateRequest,id,0),F(2,STRING,OPTIONAL,STATIC,id,CurveUpdateRequest,name,id,0),F(3,MESSAGE,REPEATED,STATIC,id,CurveUpdateRequest,points,name,&ff_CurvePt[0]),L};

static const pb_field_t ff_SchedInfo[]={F(1,UINT32,REQUIRED,STATIC,id,ScheduleInfo,id,0),F(2,UINT32,OPTIONAL,STATIC,id,ScheduleInfo,fan_id,id,0),F(3,UINT32,OPTIONAL,STATIC,id,ScheduleInfo,duty,fan_id,0),F(4,UINT32,OPTIONAL,STATIC,id,ScheduleInfo,start_min,duty,0),F(5,UINT32,OPTIONAL,STATIC,id,ScheduleInfo,end_min,start_min,0),F(6,BOOL,OPTIONAL,STATIC,id,ScheduleInfo,enabled,end_min,0),L};
static const pb_field_t ff_SchedList[]={F(1,MESSAGE,REPEATED,STATIC,schedules,ScheduleList,schedules,0,&ff_SchedInfo[0]),L};
static const pb_field_t ff_SchedCreate[]={F(1,UINT32,REQUIRED,STATIC,fan_id,ScheduleCreateRequest,fan_id,0),F(2,UINT32,REQUIRED,STATIC,fan_id,ScheduleCreateRequest,duty,fan_id,0),F(3,UINT32,REQUIRED,STATIC,fan_id,ScheduleCreateRequest,start_min,duty,0),F(4,UINT32,REQUIRED,STATIC,fan_id,ScheduleCreateRequest,end_min,start_min,0),F(5,BOOL,OPTIONAL,STATIC,fan_id,ScheduleCreateRequest,enabled,end_min,0),L};
static const pb_field_t ff_SchedUpdate[]={F(1,UINT32,REQUIRED,STATIC,id,ScheduleUpdateRequest,id,0),F(2,UINT32,OPTIONAL,STATIC,id,ScheduleUpdateRequest,fan_id,id,0),F(3,UINT32,OPTIONAL,STATIC,id,ScheduleUpdateRequest,duty,fan_id,0),F(4,UINT32,OPTIONAL,STATIC,id,ScheduleUpdateRequest,start_min,duty,0),F(5,UINT32,OPTIONAL,STATIC,id,ScheduleUpdateRequest,end_min,start_min,0),F(6,BOOL,OPTIONAL,STATIC,id,ScheduleUpdateRequest,enabled,end_min,0),L};

__attribute__((unused)) static const pb_field_t ff_WifiAp[]={F(1,STRING,OPTIONAL,STATIC,ssid,WifiApRecord,ssid,0),F(2,SINT32,OPTIONAL,STATIC,ssid,WifiApRecord,rssi,ssid,0),F(3,UINT32,OPTIONAL,STATIC,ssid,WifiApRecord,channel,rssi,0),F(4,UINT32,OPTIONAL,STATIC,ssid,WifiApRecord,authmode,channel,0),L};
static const pb_field_t ff_WifiScan[]={F(1,MESSAGE,REPEATED,STATIC,aps,WifiScanResult,aps,0,&ff_WifiAp[0]),L};
static const pb_field_t ff_WifiConn[]={F(1,STRING,REQUIRED,STATIC,ssid,WifiConnectRequest,ssid,0),F(2,STRING,REQUIRED,STATIC,ssid,WifiConnectRequest,password,ssid,0),L};
static const pb_field_t ff_WifiStatus[]={F(1,BOOL,OPTIONAL,STATIC,sta_connected,WifiStatus,sta_connected,0),F(2,STRING,OPTIONAL,STATIC,sta_connected,WifiStatus,sta_ip,sta_connected,0),F(3,STRING,OPTIONAL,STATIC,sta_connected,WifiStatus,ap_ip,sta_ip,0),L};
static const pb_field_t ff_SysInfo[]={F(1,STRING,OPTIONAL,STATIC,version,SystemInfo,version,0),F(2,UINT32,OPTIONAL,STATIC,version,SystemInfo,uptime_s,version,0),F(3,UINT32,OPTIONAL,STATIC,version,SystemInfo,heap_free,uptime_s,0),F(4,UINT32,OPTIONAL,STATIC,version,SystemInfo,fan_count,heap_free,0),F(5,UINT32,OPTIONAL,STATIC,version,SystemInfo,source_count,fan_count,0),F(6,UINT32,OPTIONAL,STATIC,version,SystemInfo,curve_count,source_count,0),F(7,UINT32,OPTIONAL,STATIC,version,SystemInfo,schedule_count,curve_count,0),L};
static const pb_field_t ff_Status[]={F(1,BOOL,REQUIRED,STATIC,ok,StatusResponse,ok,0),F(2,UINT32,OPTIONAL,STATIC,ok,StatusResponse,error_code,ok,0),F(3,STRING,OPTIONAL,STATIC,ok,StatusResponse,error_msg,error_code,0),L};

/* ---- CoAP types ---- */
typedef struct { uint8_t code,tkl; uint16_t mid; uint8_t token[8]; uint8_t *payload; size_t payload_len; struct { char seg[10][32]; int count; } path; } coap_req_t;
typedef struct { uint8_t *buf; size_t len,cap; } coap_resp_t;
#define COAP_GET 1
#define COAP_POST 2
#define COAP_PUT 3
#define COAP_DELETE 4

/* ---- PB helpers ---- */
static bool pb_enc(const void *m, const pb_field_t f[], uint8_t *b, size_t c, size_t *o) { pb_ostream_t s=pb_ostream_from_buffer(b,c); bool ok=pb_encode(&s,f,m); if(ok)*o=s.bytes_written; return ok; }
static bool pb_dec(const uint8_t *b, size_t l, void *m, const pb_field_t f[]) { pb_istream_t s=pb_istream_from_buffer(b,l); return pb_decode(&s,f,m); }

/* ---- Struct-to-PB ---- */
static void f2pb(const f_fan_info_t *fi, FanInfo *pb) { *pb=(FanInfo)FanInfo_init_default; pb->id=fi->id; pb->mode=(FanMode)fi->mode; pb->duty=fi->duty; pb->rpm=fi->rpm; pb->enabled=fi->enabled; pb->inverted=fi->inverted; pb->pwm_gpio=fi->pwm_gpio; pb->tach_gpio=fi->tach_gpio; pb->source_id=fi->source_id; pb->curve_id=fi->curve_id; pb->schedule_id=fi->schedule_id; pb->group_id=fi->group_id; pb->alarm=(FanAlarm)fi->alarm; strncpy(pb->name,fi->name,sizeof(pb->name)-1); }
static void s2pb(const f_source_info_t *si, SourceInfo *pb) { *pb=(SourceInfo)SourceInfo_init_default; pb->id=si->id; pb->type=(SourceType)si->type; pb->status=(SourceStatus)si->status; pb->temp_c=si->temp_c; pb->gpio=si->gpio; strncpy(pb->name,si->name,sizeof(pb->name)-1); }
static void c2pb(const f_curve_info_t *ci, CurveInfo *pb) { *pb=(CurveInfo)CurveInfo_init_default; pb->id=ci->id; pb->points_count=ci->num_points; strncpy(pb->name,ci->name,sizeof(pb->name)-1); for(int i=0;i<ci->num_points;i++){pb->points[i].temp_c=ci->points[i].temp_c;pb->points[i].duty=ci->points[i].duty;} }
static void sc2pb(const f_schedule_info_t *si, ScheduleInfo *pb) { *pb=(ScheduleInfo)ScheduleInfo_init_default; pb->id=si->id; pb->fan_id=si->fan_id; pb->duty=si->duty; pb->start_min=si->start_min; pb->end_min=si->end_min; pb->enabled=si->enabled; }

/* ---- CoAP parser ---- */
static int coap_parse(const uint8_t *buf, size_t len, coap_req_t *req) {
    if(len<4)return -1;
    if(((buf[0]>>6)&3)!=1)return -1;
    req->code=buf[1]; req->mid=((uint16_t)buf[2]<<8)|buf[3]; req->tkl=buf[0]&0x0F;
    req->payload=NULL;req->payload_len=0;memset(&req->path,0,sizeof(req->path));
    size_t pos=4;
    if(req->tkl>0&&req->tkl<=sizeof(req->token)&&pos+req->tkl<=len){memcpy(req->token,buf+pos,req->tkl);pos+=req->tkl;}
    if(pos>=len)return 0;
    uint8_t *opt=(uint8_t*)buf+pos;size_t oplen=len-pos,oi=0;uint16_t onum=0;
    while(oi<oplen){if(opt[oi]==0xFF){req->payload=(uint8_t*)buf+pos+oi+1;req->payload_len=len-pos-oi-1;break;}
        uint16_t delta=(opt[oi]>>4)&0xF,len8=opt[oi]&0xF;oi++;
        if(delta==13){delta=opt[oi]+13;oi++;}else if(delta==14){delta=((uint16_t)opt[oi]<<8)+opt[oi+1]+269;oi+=2;}
        onum+=delta;
        if(len8==13){len8=opt[oi]+13;oi++;}else if(len8==14){len8=((uint16_t)opt[oi]<<8)+opt[oi+1]+269;oi+=2;}
        if(oi+len8>oplen)break;
        if(onum==11&&req->path.count<10&&len8<32){memcpy(req->path.seg[req->path.count],opt+oi,len8);req->path.seg[req->path.count][len8]='\0';req->path.count++;}
        oi+=len8;
    }
    return 0;
}

/* ---- CoAP response ---- */
static void coap_send(coap_resp_t *r, uint8_t code, const coap_req_t *req, const uint8_t *p, size_t pl) {
    uint8_t *b=r->buf;b[0]=0x60|(req->tkl&0xF);b[1]=code;b[2]=(req->mid>>8)&0xFF;b[3]=req->mid&0xFF;
    size_t pos=4;if(req->tkl){memcpy(b+pos,req->token,req->tkl);pos+=req->tkl;}
    b[pos++]=0xFF;if(p&&pl){memcpy(b+pos,p,pl);pos+=pl;}r->len=pos;
}
static void coap_err(coap_resp_t *r, uint8_t code, const coap_req_t *req) {
    StatusResponse sr={.ok=false,.error_code=code};snprintf(sr.error_msg,64,"%d.%02d",code>>5,code&0x1F);
    uint8_t b[128];size_t o;pb_enc(&sr,ff_Status,b,128,&o);coap_send(r,code,req,b,o);
}
static bool path_len(const coap_req_t *req,int n){return req->path.count==n;}
static uint8_t path_id(const coap_req_t *req,int idx){return (uint8_t)atoi(req->path.seg[idx]);}

/* ---- f_coap struct ---- */
struct f_coap { int sock; bool running; TaskHandle_t task; f_fan_handle_t fan; f_source_handle_t source; f_curve_handle_t curve; f_schedule_handle_t schedule; f_config_handle_t config; };

/* ===== Resource Handlers ===== */

static void h_fans_get(struct f_coap *h, const coap_req_t *req, coap_resp_t *r) {
    if(path_len(req,1)){FanList list=FanList_init_default;
        for(uint8_t i=0;i<8;i++){f_fan_info_t fi;if(f_fan_get_info(h->fan,i,&fi)==ESP_OK)f2pb(&fi,&list.fans[list.fans_count++]);}
        uint8_t b[1024];size_t o;pb_enc(&list,ff_FanList,b,1024,&o);coap_send(r,0x45,req,b,o);
    }else if(path_len(req,2)){uint8_t id=path_id(req,1);f_fan_info_t fi;if(f_fan_get_info(h->fan,id,&fi)!=ESP_OK){coap_err(r,0x84,req);return;}
        FanInfo pb;f2pb(&fi,&pb);uint8_t b[256];size_t o;pb_enc(&pb,ff_FanInfo,b,256,&o);coap_send(r,0x45,req,b,o);
    }else coap_err(r,0x80,req);
}
static void h_fans_post(struct f_coap *h, const coap_req_t *req, coap_resp_t *r) {
    FanCreateRequest cr=FanCreateRequest_init_default;if(!pb_dec(req->payload,req->payload_len,&cr,ff_FanCreate)){coap_err(r,0x80,req);return;}
    uint8_t id;if(f_fan_add(h->fan,cr.pwm_gpio,cr.tach_gpio,cr.name,&id)!=ESP_OK){coap_err(r,0x80,req);return;}
    if(h->config)f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
    f_fan_info_t fi;f_fan_get_info(h->fan,id,&fi);FanInfo pb;f2pb(&fi,&pb);uint8_t b[256];size_t o;pb_enc(&pb,ff_FanInfo,b,256,&o);coap_send(r,0x41,req,b,o);
}
static void h_fans_put(struct f_coap *h, const coap_req_t *req, coap_resp_t *r) {
    uint8_t id=path_id(req,1);FanUpdateRequest ur=FanUpdateRequest_init_default;
    if(!pb_dec(req->payload,req->payload_len,&ur,ff_FanUpdate)){coap_err(r,0x80,req);return;}
    f_fan_info_t fi;if(f_fan_get_info(h->fan,id,&fi)!=ESP_OK){coap_err(r,0x84,req);return;}
    if(ur.has_mode)f_fan_set_mode(h->fan,id,(fan_mode_t)ur.mode);
    if(ur.has_duty)f_fan_set_duty(h->fan,id,(uint8_t)ur.duty);
    if(ur.has_source_id)f_fan_set_source(h->fan,id,(uint8_t)ur.source_id);
    if(ur.has_curve_id)f_fan_set_curve(h->fan,id,(uint8_t)ur.curve_id);
    if(ur.has_schedule_id)f_fan_set_schedule(h->fan,id,(uint8_t)ur.schedule_id);
    if(ur.has_group_id)f_fan_set_group(h->fan,id,(uint8_t)ur.group_id);
    if(ur.has_inverted)f_fan_set_inverted(h->fan,id,ur.inverted);
    if(h->config)f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
    f_fan_get_info(h->fan,id,&fi);FanInfo pb;f2pb(&fi,&pb);uint8_t b[256];size_t o;pb_enc(&pb,ff_FanInfo,b,256,&o);coap_send(r,0x44,req,b,o);
}
static void h_fans_delete(struct f_coap *h, const coap_req_t *req, coap_resp_t *r) {
    uint8_t id=path_id(req,1);if(f_fan_remove(h->fan,id)!=ESP_OK){coap_err(r,0x84,req);return;}
    if(h->config)f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
    StatusResponse sr={.ok=true};uint8_t b[64];size_t o;pb_enc(&sr,ff_Status,b,64,&o);coap_send(r,0x42,req,b,o);
}

static void h_sources_get(struct f_coap *h, const coap_req_t *req, coap_resp_t *r) {
    SourceList list=SourceList_init_default;
    for(uint8_t i=0;i<8;i++){f_source_info_t si;if(f_source_get_info(h->source,i,&si)==ESP_OK)s2pb(&si,&list.sources[list.sources_count++]);}
    uint8_t b[1024];size_t o;pb_enc(&list,ff_SrcList,b,1024,&o);coap_send(r,0x45,req,b,o);
}
static void h_sources_post(struct f_coap *h, const coap_req_t *req, coap_resp_t *r) {
    SourceCreateRequest cr=SourceCreateRequest_init_default;if(!pb_dec(req->payload,req->payload_len,&cr,ff_SrcCreate)){coap_err(r,0x80,req);return;}
    uint8_t id;if(f_source_add(h->source,(source_type_t)cr.type,cr.gpio,cr.name,&id)!=ESP_OK){coap_err(r,0x80,req);return;}
    if(h->config)f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
    f_source_info_t si;f_source_get_info(h->source,id,&si);SourceInfo pb;s2pb(&si,&pb);uint8_t b[256];size_t o;pb_enc(&pb,ff_SrcInfo,b,256,&o);coap_send(r,0x41,req,b,o);
}
static void h_sources_delete(struct f_coap *h, const coap_req_t *req, coap_resp_t *r) {
    uint8_t id=path_id(req,1);if(f_source_remove(h->source,id)!=ESP_OK){coap_err(r,0x84,req);return;}
    if(h->config)f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
    StatusResponse sr={.ok=true};uint8_t b[64];size_t o;pb_enc(&sr,ff_Status,b,64,&o);coap_send(r,0x42,req,b,o);
}
static void h_sources_temp(struct f_coap *h, const coap_req_t *req, coap_resp_t *r) {
    ManualTempRequest mt=ManualTempRequest_init_default;if(!pb_dec(req->payload,req->payload_len,&mt,ff_ManTemp)){coap_err(r,0x80,req);return;}
    if(f_source_update_manual(h->source,mt.id,mt.temp_c)!=ESP_OK){coap_err(r,0x80,req);return;}
    if(h->config)f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
    StatusResponse sr={.ok=true};uint8_t b[64];size_t o;pb_enc(&sr,ff_Status,b,64,&o);coap_send(r,0x44,req,b,o);
}

static void h_curves_get(struct f_coap *h, const coap_req_t *req, coap_resp_t *r) {
    if(path_len(req,1)){CurveList list=CurveList_init_default;
        for(uint8_t i=0;i<16;i++){f_curve_info_t ci;if(f_curve_get_info(h->curve,i,&ci)==ESP_OK)c2pb(&ci,&list.curves[list.curves_count++]);}
        uint8_t b[2048];size_t o;pb_enc(&list,ff_CurveList,b,2048,&o);coap_send(r,0x45,req,b,o);
    }else if(path_len(req,2)){uint8_t id=path_id(req,1);f_curve_info_t ci;if(f_curve_get_info(h->curve,id,&ci)!=ESP_OK){coap_err(r,0x84,req);return;}
        CurveInfo pb;c2pb(&ci,&pb);uint8_t b[512];size_t o;pb_enc(&pb,ff_CurveInfo,b,512,&o);coap_send(r,0x45,req,b,o);
    }else coap_err(r,0x80,req);
}
static void h_curves_post(struct f_coap *h, const coap_req_t *req, coap_resp_t *r) {
    CurveCreateRequest cr=CurveCreateRequest_init_default;if(!pb_dec(req->payload,req->payload_len,&cr,ff_CurveCreate)){coap_err(r,0x80,req);return;}
    f_curve_info_t ci={0};strncpy(ci.name,cr.name,15);ci.num_points=cr.points_count;
    for(int i=0;i<cr.points_count;i++){ci.points[i].temp_c=cr.points[i].temp_c;ci.points[i].duty=(uint8_t)cr.points[i].duty;}
    uint8_t id;if(f_curve_upsert(h->curve,&ci,&id)!=ESP_OK){coap_err(r,0x80,req);return;}
    if(h->config)f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
    f_curve_get_info(h->curve,id,&ci);CurveInfo pb;c2pb(&ci,&pb);uint8_t b[512];size_t o;pb_enc(&pb,ff_CurveInfo,b,512,&o);coap_send(r,0x41,req,b,o);
}
static void h_curves_put(struct f_coap *h, const coap_req_t *req, coap_resp_t *r) {
    uint8_t id=path_id(req,1);CurveUpdateRequest ur=CurveUpdateRequest_init_default;
    if(!pb_dec(req->payload,req->payload_len,&ur,ff_CurveUpdate)){coap_err(r,0x80,req);return;}
    f_curve_info_t ci={0};ci.id=id;strncpy(ci.name,ur.name,15);ci.num_points=ur.points_count;
    for(int i=0;i<ur.points_count;i++){ci.points[i].temp_c=ur.points[i].temp_c;ci.points[i].duty=(uint8_t)ur.points[i].duty;}
    uint8_t oid;if(f_curve_upsert(h->curve,&ci,&oid)!=ESP_OK){coap_err(r,0x80,req);return;}
    if(h->config)f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
    f_curve_get_info(h->curve,oid,&ci);CurveInfo pb;c2pb(&ci,&pb);uint8_t b[512];size_t o;pb_enc(&pb,ff_CurveInfo,b,512,&o);coap_send(r,0x44,req,b,o);
}
static void h_curves_delete(struct f_coap *h, const coap_req_t *req, coap_resp_t *r) {
    uint8_t id=path_id(req,1);if(f_curve_remove(h->curve,id)!=ESP_OK){coap_err(r,0x84,req);return;}
    if(h->config)f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
    StatusResponse sr={.ok=true};uint8_t b[64];size_t o;pb_enc(&sr,ff_Status,b,64,&o);coap_send(r,0x42,req,b,o);
}

static void h_schedules_get(struct f_coap *h, const coap_req_t *req, coap_resp_t *r) {
    ScheduleList list=ScheduleList_init_default;
    for(uint8_t i=0;i<8;i++){f_schedule_info_t si;if(f_schedule_get_info(h->schedule,i,&si)==ESP_OK)sc2pb(&si,&list.schedules[list.schedules_count++]);}
    uint8_t b[512];size_t o;pb_enc(&list,ff_SchedList,b,512,&o);coap_send(r,0x45,req,b,o);
}
static void h_schedules_post(struct f_coap *h, const coap_req_t *req, coap_resp_t *r) {
    ScheduleCreateRequest cr=ScheduleCreateRequest_init_default;if(!pb_dec(req->payload,req->payload_len,&cr,ff_SchedCreate)){coap_err(r,0x80,req);return;}
    f_schedule_info_t si={.fan_id=(uint8_t)cr.fan_id,.duty=(uint8_t)cr.duty,.start_min=(uint16_t)cr.start_min,.end_min=(uint16_t)cr.end_min,.enabled=cr.enabled};
    uint8_t id;if(f_schedule_add(h->schedule,&si,&id)!=ESP_OK){coap_err(r,0x80,req);return;}
    if(h->config)f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
    f_schedule_get_info(h->schedule,id,&si);ScheduleInfo pb;sc2pb(&si,&pb);uint8_t b[256];size_t o;pb_enc(&pb,ff_SchedInfo,b,256,&o);coap_send(r,0x41,req,b,o);
}
static void h_schedules_put(struct f_coap *h, const coap_req_t *req, coap_resp_t *r) {
    uint8_t id=path_id(req,1);ScheduleUpdateRequest ur=ScheduleUpdateRequest_init_default;
    if(!pb_dec(req->payload,req->payload_len,&ur,ff_SchedUpdate)){coap_err(r,0x80,req);return;}
    f_schedule_info_t si={.fan_id=(uint8_t)ur.fan_id,.duty=(uint8_t)ur.duty,.start_min=(uint16_t)ur.start_min,.end_min=(uint16_t)ur.end_min,.enabled=ur.enabled};
    if(f_schedule_update(h->schedule,id,&si)!=ESP_OK){coap_err(r,0x84,req);return;}
    if(h->config)f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
    f_schedule_get_info(h->schedule,id,&si);ScheduleInfo pb;sc2pb(&si,&pb);uint8_t b[256];size_t o;pb_enc(&pb,ff_SchedInfo,b,256,&o);coap_send(r,0x44,req,b,o);
}
static void h_schedules_delete(struct f_coap *h, const coap_req_t *req, coap_resp_t *r) {
    uint8_t id=path_id(req,1);if(f_schedule_remove(h->schedule,id)!=ESP_OK){coap_err(r,0x84,req);return;}
    if(h->config)f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
    StatusResponse sr={.ok=true};uint8_t b[64];size_t o;pb_enc(&sr,ff_Status,b,64,&o);coap_send(r,0x42,req,b,o);
}

static void h_wifi_scan(struct f_coap *h, const coap_req_t *req, coap_resp_t *r) {
    wifi_scan_config_t sc={.scan_type=WIFI_SCAN_TYPE_ACTIVE,.scan_time.active={.min=100,.max=300}};
    if(esp_wifi_scan_start(&sc,true)!=ESP_OK){coap_err(r,0xA3,req);return;}
    uint16_t n=0;esp_wifi_scan_get_ap_num(&n);WifiScanResult sr=WifiScanResult_init_default;
    if(n>0){wifi_ap_record_t *aps=calloc(n,sizeof(wifi_ap_record_t));
        if(aps){esp_wifi_scan_get_ap_records(&n,aps);
            for(int i=0;i<n&&i<16;i++){WifiApRecord *ap=&sr.aps[sr.aps_count++];
                strncpy(ap->ssid,(char*)aps[i].ssid,32);ap->rssi=aps[i].rssi;ap->channel=aps[i].primary;ap->authmode=aps[i].authmode;}
            free(aps);}}
    uint8_t b[1024];size_t o;pb_enc(&sr,ff_WifiScan,b,1024,&o);coap_send(r,0x45,req,b,o);
}
static void h_wifi_connect(struct f_coap *h, const coap_req_t *req, coap_resp_t *r) {
    WifiConnectRequest cr=WifiConnectRequest_init_default;if(!pb_dec(req->payload,req->payload_len,&cr,ff_WifiConn)){coap_err(r,0x80,req);return;}
    wifi_config_t wc={0};strncpy((char*)wc.sta.ssid,cr.ssid,32);strncpy((char*)wc.sta.password,cr.password,63);wc.sta.threshold.authmode=WIFI_AUTH_WPA2_PSK;
    if(esp_wifi_set_config(WIFI_IF_STA,&wc)!=ESP_OK){coap_err(r,0xA3,req);return;}
    esp_wifi_disconnect();esp_wifi_connect();
    StatusResponse sr={.ok=true};uint8_t b[64];size_t o;pb_enc(&sr,ff_Status,b,64,&o);coap_send(r,0x44,req,b,o);
}
static void h_wifi_status(struct f_coap *h, const coap_req_t *req, coap_resp_t *r) {
    WifiStatus ws=WifiStatus_init_default;esp_netif_t *sta=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if(sta){esp_netif_ip_info_t ip;if(esp_netif_get_ip_info(sta,&ip)==ESP_OK&&ip.ip.addr!=0){ws.sta_connected=true;snprintf(ws.sta_ip,16,IPSTR,IP2STR(&ip.ip));}}
    strncpy(ws.ap_ip,"192.168.4.1",15);uint8_t b[128];size_t o;pb_enc(&ws,ff_WifiStatus,b,128,&o);coap_send(r,0x45,req,b,o);
}
static void h_system_info(struct f_coap *h, const coap_req_t *req, coap_resp_t *r) {
    SystemInfo si=SystemInfo_init_default;snprintf(si.version,12,"%d.%d.%d",ESPFM_VERSION_MAJOR,ESPFM_VERSION_MINOR,ESPFM_VERSION_PATCH);
    si.uptime_s=esp_timer_get_time()/1000000;si.heap_free=esp_get_free_heap_size();
    si.fan_count=h->fan?f_fan_get_count(h->fan):0;si.source_count=h->source?f_source_get_count(h->source):0;
    si.curve_count=h->curve?f_curve_get_count(h->curve):0;si.schedule_count=h->schedule?f_schedule_get_count(h->schedule):0;
    uint8_t b[256];size_t o;pb_enc(&si,ff_SysInfo,b,256,&o);coap_send(r,0x45,req,b,o);
}

/* ---- Dispatcher ---- */
static void dispatch(struct f_coap *h, coap_req_t *req, coap_resp_t *resp) {
    if(req->path.count==0)return;
    const char *b=req->path.seg[0];
    if(!strcmp(b,"fans")){switch(req->code){case COAP_GET:h_fans_get(h,req,resp);break;case COAP_POST:h_fans_post(h,req,resp);break;case COAP_PUT:h_fans_put(h,req,resp);break;case COAP_DELETE:h_fans_delete(h,req,resp);break;default:coap_err(resp,0x80,req);}}
    else if(!strcmp(b,"sources")){if(req->path.count==2&&!strcmp(req->path.seg[1],"temp")&&req->code==COAP_POST)h_sources_temp(h,req,resp);else switch(req->code){case COAP_GET:h_sources_get(h,req,resp);break;case COAP_POST:h_sources_post(h,req,resp);break;case COAP_DELETE:h_sources_delete(h,req,resp);break;default:coap_err(resp,0x80,req);}}
    else if(!strcmp(b,"curves")){switch(req->code){case COAP_GET:h_curves_get(h,req,resp);break;case COAP_POST:h_curves_post(h,req,resp);break;case COAP_PUT:h_curves_put(h,req,resp);break;case COAP_DELETE:h_curves_delete(h,req,resp);break;default:coap_err(resp,0x80,req);}}
    else if(!strcmp(b,"schedules")){switch(req->code){case COAP_GET:h_schedules_get(h,req,resp);break;case COAP_POST:h_schedules_post(h,req,resp);break;case COAP_PUT:h_schedules_put(h,req,resp);break;case COAP_DELETE:h_schedules_delete(h,req,resp);break;default:coap_err(resp,0x80,req);}}
    else if(!strcmp(b,"wifi")){if(req->path.count==2&&!strcmp(req->path.seg[1],"scan"))h_wifi_scan(h,req,resp);else if(req->path.count==2&&!strcmp(req->path.seg[1],"connect"))h_wifi_connect(h,req,resp);else if(req->path.count==2&&!strcmp(req->path.seg[1],"status"))h_wifi_status(h,req,resp);else coap_err(resp,0x84,req);}
    else if(!strcmp(b,"system")&&req->path.count==2&&!strcmp(req->path.seg[1],"info"))h_system_info(h,req,resp);
    else coap_err(resp,0x84,req);
}

/* ---- CoAP server task ---- */
static void coap_task(void *arg) {
    struct f_coap *h=arg;uint8_t rx[COAP_MTU],tx[COAP_MTU];ESP_LOGI(TAG,"CoAP listening on :%d",COAP_PORT);
    while(h->running){struct sockaddr_in from;socklen_t fl=sizeof(from);
        int n=recvfrom(h->sock,rx,COAP_MTU,0,(struct sockaddr*)&from,&fl);
        if(n<4){vTaskDelay(pdMS_TO_TICKS(10));continue;}
        coap_req_t req;if(coap_parse(rx,n,&req)<0)continue;
        coap_resp_t resp={.buf=tx,.len=0,.cap=COAP_MTU};dispatch(h,&req,&resp);
        if(resp.len>0)sendto(h->sock,tx,resp.len,0,(struct sockaddr*)&from,fl);
    }
    vTaskDelete(NULL);
}

/* ---- WiFi events ---- */
static void _wc(void *a,esp_event_base_t b,int32_t i,void *d){struct f_coap *h=a;if(h)f_coap_start(h);}
static void _wd(void *a,esp_event_base_t b,int32_t i,void *d){struct f_coap *h=a;if(h)f_coap_stop(h);}

/* ---- Public API ---- */
esp_err_t f_coap_init(f_coap_handle_t *handle,f_fan_handle_t fan,f_source_handle_t source,f_curve_handle_t curve,f_schedule_handle_t schedule,f_config_handle_t config) {
    if(!handle)return ESP_ERR_INVALID_ARG;
    struct f_coap *h=calloc(1,sizeof(*h));
    if(!h)return ESP_ERR_NO_MEM;
    h->fan=fan;h->source=source;h->curve=curve;h->schedule=schedule;h->config=config;h->sock=-1;
    esp_event_handler_register(ESPFM_EVENT,ESPFM_EVENT_WIFI_CONNECTED,_wc,h);
    esp_event_handler_register(ESPFM_EVENT,ESPFM_EVENT_WIFI_DISCONNECTED,_wd,h);
    *handle=h;ESP_LOGI(TAG,"CoAP server initialized");return ESP_OK;
}
esp_err_t f_coap_start(f_coap_handle_t h) {
    if(!h||h->running)return ESP_OK;
    h->sock=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(h->sock<0)return ESP_FAIL;
    struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(COAP_PORT)};a.sin_addr.s_addr=INADDR_ANY;
    if(bind(h->sock,(struct sockaddr*)&a,sizeof(a))<0){close(h->sock);h->sock=-1;return ESP_FAIL;}
    h->running=true;xTaskCreate(coap_task,"coap",COAP_TASK_STACK,h,COAP_TASK_PRIO,&h->task);
    ESP_LOGI(TAG,"CoAP started :%d",COAP_PORT);return ESP_OK;
}
esp_err_t f_coap_stop(f_coap_handle_t h) {
    if(!h||!h->running)return ESP_OK;
    h->running=false;
    if(h->sock>=0){close(h->sock);h->sock=-1;}
    ESP_LOGI(TAG,"CoAP stopped");return ESP_OK;
}
