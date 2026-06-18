/* f_coap.c — CoAP + Protobuf server (v3 — replaces f_http REST + JSON) */
#include "f_coap.h"
#include "f_constraints.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "espfm.pb.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "f_coap";
#define COAP_PORT      5683
#define COAP_MAX_PKT   1500
#define COAP_TASK_STACK 4096
#define COAP_TASK_PRIO  4

/* CoAP message codes */
#define COAP_GET    1
#define COAP_POST   2
#define COAP_PUT    3
#define COAP_DELETE 4
#define COAP_RESP_CONTENT     0x45  /* 2.05 Content */
#define COAP_RESP_CREATED     0x41  /* 2.01 Created */
#define COAP_RESP_CHANGED     0x44  /* 2.04 Changed */
#define COAP_RESP_DELETED     0x42  /* 2.02 Deleted */
#define COAP_RESP_BAD_REQ     0x80  /* 4.00 Bad Request */
#define COAP_RESP_NOT_FOUND   0x84  /* 4.04 Not Found */
#define COAP_RESP_SVC_UNAVAIL 0xA3  /* 5.03 Service Unavailable */

struct f_coap {
    int                 sock;
    bool                running;
    TaskHandle_t        task;
    f_fan_handle_t      fan;
    f_source_handle_t   source;
    f_curve_handle_t    curve;
    f_schedule_handle_t schedule;
    f_config_handle_t   config;
};

/* Forward decls */
typedef struct { uint8_t code, tkl; uint16_t mid; uint8_t token[8];
    uint8_t *payload; size_t payload_len; uint32_t content_fmt;
    struct { char seg[10][32]; int count; } path;
} coap_req_t;

typedef struct { uint8_t *buf; size_t len, cap; } coap_resp_t;

/* ---- PB helpers ---- */
static bool pb_encode_msg(const void *msg, const pb_field_t fields[],
                          uint8_t *buf, size_t cap, size_t *out) {
    pb_ostream_t s = pb_ostream_from_buffer(buf, cap);
    bool ok = pb_encode(&s, fields, msg);
    if (ok) *out = s.bytes_written;
    return ok;
}

static bool pb_decode_msg(const uint8_t *buf, size_t len,
                          void *msg, const pb_field_t fields[]) {
    pb_istream_t s = pb_istream_from_buffer(buf, len);
    return pb_decode(&s, fields, msg);
}

/* ---- Registry -> PB converters ---- */
static void fan_to_pb(const f_fan_info_t *fi, FanInfo *pb) {
    *pb = (FanInfo)FanInfo_init_default;
    pb->id=fi->id; pb->mode=(FanMode)fi->mode; pb->duty=fi->duty;
    pb->rpm=fi->rpm; pb->enabled=fi->enabled; pb->inverted=fi->inverted;
    pb->pwm_gpio=fi->pwm_gpio; pb->tach_gpio=fi->tach_gpio;
    pb->source_id=fi->source_id; pb->curve_id=fi->curve_id;
    pb->schedule_id=fi->schedule_id; pb->group_id=fi->group_id;
    pb->alarm=(FanAlarm)fi->alarm;
    strncpy(pb->name, fi->name, sizeof(pb->name)-1);
}

static void src_to_pb(const f_source_info_t *si, SourceInfo *pb) {
    *pb = (SourceInfo)SourceInfo_init_default;
    pb->id=si->id; pb->type=(SourceType)si->type; pb->status=(SourceStatus)si->status;
    pb->temp_c=si->temp_c; pb->gpio=si->gpio;
    strncpy(pb->name, si->name, sizeof(pb->name)-1);
}

static void curve_to_pb(const f_curve_info_t *ci, CurveInfo *pb) {
    *pb = (CurveInfo)CurveInfo_init_default;
    pb->id=ci->id; pb->points_count=ci->num_points;
    strncpy(pb->name, ci->name, sizeof(pb->name)-1);
    for(int i=0;i<ci->num_points;i++){pb->points[i].temp_c=ci->points[i].temp_c;pb->points[i].duty=ci->points[i].duty;}
}

static void sched_to_pb(const f_schedule_info_t *si, ScheduleInfo *pb) {
    *pb = (ScheduleInfo)ScheduleInfo_init_default;
    pb->id=si->id; pb->fan_id=si->fan_id; pb->duty=si->duty;
    pb->start_min=si->start_min; pb->end_min=si->end_min; pb->enabled=si->enabled;
}

/* ---- Minimal CoAP parser ---- */
static int coap_parse(const uint8_t *buf, size_t len, coap_req_t *req) {
    if(len<4) return -1;
    uint8_t ver = (buf[0]>>6)&3;
    if(ver!=1) return -1;
    req->code = buf[1];
    req->mid = ((uint16_t)buf[2]<<8)|buf[3];
    req->tkl = buf[0]&0x0F;
    req->payload=NULL; req->payload_len=0;
    req->content_fmt=0;
    memset(&req->path,0,sizeof(req->path));

    size_t pos=4;
    if(req->tkl>0 && pos+req->tkl<=len) {
        memcpy(req->token,buf+pos,req->tkl);
        pos+=req->tkl;
    }
    /* Parse options */
    if(pos>=len) return 0;
    uint8_t *opt=(uint8_t*)buf+pos; size_t oplen=len-pos;
    size_t oi=0; uint16_t onum=0;
    while(oi<oplen) {
        if(opt[oi]==0xFF){req->payload=(uint8_t*)buf+pos+oi+1;req->payload_len=len-pos-oi-1;break;}
        uint16_t delta=(opt[oi]>>4)&0x0F; uint16_t len8=opt[oi]&0x0F; oi++;
        if(delta==13){delta=opt[oi]+13;oi++;}
        else if(delta==14){delta=((uint16_t)opt[oi]<<8)+opt[oi+1]+269;oi+=2;}
        onum+=delta;
        if(len8==13){len8=opt[oi]+13;oi++;}
        else if(len8==14){len8=((uint16_t)opt[oi]<<8)+opt[oi+1]+269;oi+=2;}
        if(oi+len8>oplen) break;
        /* URI-Path (option 11) */
        if(onum==11 && req->path.count<10 && len8<32) {
            memcpy(req->path.seg[req->path.count],opt+oi,len8);
            req->path.seg[req->path.count][len8]='\0';
            req->path.count++;
        }
        /* Content-Format (option 12) */
        if(onum==12 && len8>=1) {
            req->content_fmt=opt[oi];
            if(len8==2) req->content_fmt=(req->content_fmt<<8)|opt[oi+1];
        }
        oi+=len8;
    }
    return 0;
}

/* ---- CoAP response builder ---- */
static void coap_resp_init(coap_resp_t *r, uint8_t *buf, size_t cap) {
    r->buf=buf; r->len=0; r->cap=cap;
}

static void coap_resp_set(coap_resp_t *r, uint8_t code, const coap_req_t *req,
                          uint32_t cf, const uint8_t *payload, size_t plen) {
    uint8_t *b=r->buf;
    b[0]=(1<<6)|(req->tkl&0x0F); /* version 1, CON type */
    b[1]=code; b[2]=(req->mid>>8)&0xFF; b[3]=req->mid&0xFF;
    size_t pos=4;
    if(req->tkl){memcpy(b+pos,req->token,req->tkl);pos+=req->tkl;}
    /* Content-Format option if needed */
    if(cf && plen>0) {
        uint16_t onum=12;
        if(onum<13){b[pos++]=(onum<<4)|0;}else{b[pos++]=(13<<4)|(onum-13);}
        if(cf<=15){b[pos++]=(12<<4)|(uint8_t)cf;}
        else{b[pos++]=(12<<4)|0;b[pos++]=(uint8_t)(cf>>8);b[pos++]=(uint8_t)cf;}
    }
    b[pos++]=0xFF; /* Payload marker */
    if(payload && plen>0){memcpy(b+pos,payload,plen);pos+=plen;}
    r->len=pos;
}

/* ---- CoAP error response ---- */
static void coap_err(coap_resp_t *r, uint8_t code, const coap_req_t *req) {
    StatusResponse sr = {.ok=false,.error_code=code,.error_msg=""};
    snprintf(sr.error_msg,sizeof(sr.error_msg),"CoAP %d.%02d",(code>>5),(code&0x1F));
    uint8_t buf[128]; size_t out;
    if(pb_encode_msg(&sr,StatusResponse_fields,buf,sizeof(buf),&out))
        coap_resp_set(r,code,req,0,buf,out);
}

/* ---- Path matching ---- */
static bool path_len(const coap_req_t *req, int n) {return req->path.count==n;}
static uint8_t path_id(const coap_req_t *req, int idx) {return (uint8_t)atoi(req->path.seg[idx]);}

/* ===== Fan handlers ===== */
static void h_fans_get(f_coap_handle_t h, const coap_req_t *req, coap_resp_t *r) {
    if(path_len(req,1)) { /* list */
        FanList list = FanList_init_default;
        for(uint8_t i=0;i<8;i++) {
            f_fan_info_t fi; if(f_fan_get_info(h->fan,i,&fi)!=ESP_OK) continue;
            fan_to_pb(&fi,&list.fans[list.fans_count++]);
        }
        uint8_t buf[1024]; size_t out;
        if(pb_encode_msg(&list,FanList_fields,buf,sizeof(buf),&out))
            coap_resp_set(r,COAP_RESP_CONTENT,req,0,buf,out);
    } else if(path_len(req,2)) { /* single */
        uint8_t id=path_id(req,1);
        f_fan_info_t fi; if(f_fan_get_info(h->fan,id,&fi)!=ESP_OK){coap_err(r,COAP_RESP_NOT_FOUND,req);return;}
        FanInfo pb; fan_to_pb(&fi,&pb);
        uint8_t buf[256]; size_t out;
        if(pb_encode_msg(&pb,FanInfo_fields,buf,sizeof(buf),&out))
            coap_resp_set(r,COAP_RESP_CONTENT,req,0,buf,out);
    } else coap_err(r,COAP_RESP_BAD_REQ,req);
}

static void h_fans_post(f_coap_handle_t h, const coap_req_t *req, coap_resp_t *r) {
    FanCreateRequest cr = FanCreateRequest_init_default;
    if(!pb_decode_msg(req->payload,req->payload_len,&cr,FanCreateRequest_fields))
    {coap_err(r,COAP_RESP_BAD_REQ,req);return;}
    uint8_t id; esp_err_t e=f_fan_add(h->fan,cr.pwm_gpio,cr.tach_gpio,cr.name,&id);
    if(e!=ESP_OK){coap_err(r,COAP_RESP_BAD_REQ,req);return;}
    if(h->config) f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
    f_fan_info_t fi; f_fan_get_info(h->fan,id,&fi);
    FanInfo pb; fan_to_pb(&fi,&pb);
    uint8_t buf[256]; size_t out;
    if(pb_encode_msg(&pb,FanInfo_fields,buf,sizeof(buf),&out))
        coap_resp_set(r,COAP_RESP_CREATED,req,0,buf,out);
}

static void h_fans_put(f_coap_handle_t h, const coap_req_t *req, coap_resp_t *r) {
    uint8_t id=path_id(req,1);
    FanUpdateRequest ur = FanUpdateRequest_init_default;
    if(!pb_decode_msg(req->payload,req->payload_len,&ur,FanUpdateRequest_fields))
    {coap_err(r,COAP_RESP_BAD_REQ,req);return;}
    f_fan_info_t fi; if(f_fan_get_info(h->fan,id,&fi)!=ESP_OK){coap_err(r,COAP_RESP_NOT_FOUND,req);return;}
    if(ur.has_mode)       f_fan_set_mode(h->fan,id,(fan_mode_t)ur.mode);
    if(ur.has_duty)       f_fan_set_duty(h->fan,id,(uint8_t)ur.duty);
    if(ur.has_source_id)  f_fan_set_source(h->fan,id,(uint8_t)ur.source_id);
    if(ur.has_curve_id)   f_fan_set_curve(h->fan,id,(uint8_t)ur.curve_id);
    if(ur.has_schedule_id)f_fan_set_schedule(h->fan,id,(uint8_t)ur.schedule_id);
    if(ur.has_group_id)   f_fan_set_group(h->fan,id,(uint8_t)ur.group_id);
    if(ur.has_inverted)   f_fan_set_inverted(h->fan,id,ur.inverted);
    if(h->config) f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
    f_fan_get_info(h->fan,id,&fi);
    FanInfo pb; fan_to_pb(&fi,&pb);
    uint8_t buf[256]; size_t out;
    if(pb_encode_msg(&pb,FanInfo_fields,buf,sizeof(buf),&out))
        coap_resp_set(r,COAP_RESP_CHANGED,req,0,buf,out);
}

static void h_fans_delete(f_coap_handle_t h, const coap_req_t *req, coap_resp_t *r) {
    uint8_t id=path_id(req,1);
    if(f_fan_remove(h->fan,id)!=ESP_OK){coap_err(r,COAP_RESP_NOT_FOUND,req);return;}
    if(h->config) f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
    StatusResponse sr={.ok=true,.error_code=0,.error_msg=""};
    uint8_t buf[64]; size_t out;
    pb_encode_msg(&sr,StatusResponse_fields,buf,sizeof(buf),&out);
    coap_resp_set(r,COAP_RESP_DELETED,req,0,buf,out);
}

/* ===== Source handlers ===== */
static void h_sources_get(f_coap_handle_t h, const coap_req_t *req, coap_resp_t *r) {
    SourceList list = SourceList_init_default;
    for(uint8_t i=0;i<8;i++) {
        f_source_info_t si; if(f_source_get_info(h->source,i,&si)!=ESP_OK) continue;
        src_to_pb(&si,&list.sources[list.sources_count++]);
    }
    uint8_t buf[1024]; size_t out;
    if(pb_encode_msg(&list,SourceList_fields,buf,sizeof(buf),&out))
        coap_resp_set(r,COAP_RESP_CONTENT,req,0,buf,out);
}

static void h_sources_post(f_coap_handle_t h, const coap_req_t *req, coap_resp_t *r) {
    SourceCreateRequest cr = SourceCreateRequest_init_default;
    if(!pb_decode_msg(req->payload,req->payload_len,&cr,SourceCreateRequest_fields))
    {coap_err(r,COAP_RESP_BAD_REQ,req);return;}
    uint8_t id; esp_err_t e=f_source_add(h->source,(source_type_t)cr.type,cr.gpio,cr.name,&id);
    if(e!=ESP_OK){coap_err(r,COAP_RESP_BAD_REQ,req);return;}
    if(h->config) f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
    f_source_info_t si; f_source_get_info(h->source,id,&si);
    SourceInfo pb; src_to_pb(&si,&pb);
    uint8_t buf[256]; size_t out;
    if(pb_encode_msg(&pb,SourceInfo_fields,buf,sizeof(buf),&out))
        coap_resp_set(r,COAP_RESP_CREATED,req,0,buf,out);
}

static void h_sources_delete(f_coap_handle_t h, const coap_req_t *req, coap_resp_t *r) {
    uint8_t id=path_id(req,1);
    if(f_source_remove(h->source,id)!=ESP_OK){coap_err(r,COAP_RESP_NOT_FOUND,req);return;}
    if(h->config) f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
    StatusResponse sr={.ok=true};
    uint8_t buf[64]; size_t out;
    pb_encode_msg(&sr,StatusResponse_fields,buf,sizeof(buf),&out);
    coap_resp_set(r,COAP_RESP_DELETED,req,0,buf,out);
}

static void h_sources_temp(f_coap_handle_t h, const coap_req_t *req, coap_resp_t *r) {
    ManualTempRequest mt = ManualTempRequest_init_default;
    if(!pb_decode_msg(req->payload,req->payload_len,&mt,ManualTempRequest_fields))
    {coap_err(r,COAP_RESP_BAD_REQ,req);return;}
    if(f_source_update_manual(h->source,mt.id,mt.temp_c)!=ESP_OK)
    {coap_err(r,COAP_RESP_BAD_REQ,req);return;}
    if(h->config) f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
    StatusResponse sr={.ok=true};
    uint8_t buf[64]; size_t out;
    pb_encode_msg(&sr,StatusResponse_fields,buf,sizeof(buf),&out);
    coap_resp_set(r,COAP_RESP_CHANGED,req,0,buf,out);
}

/* ===== Curve handlers ===== */
static void h_curves_get(f_coap_handle_t h, const coap_req_t *req, coap_resp_t *r) {
    if(path_len(req,1)) {
        CurveList list = CurveList_init_default;
        for(uint8_t i=0;i<16;i++) {
            f_curve_info_t ci; if(f_curve_get_info(h->curve,i,&ci)!=ESP_OK) continue;
            curve_to_pb(&ci,&list.curves[list.curves_count++]);
        }
        uint8_t buf[2048]; size_t out;
        if(pb_encode_msg(&list,CurveList_fields,buf,sizeof(buf),&out))
            coap_resp_set(r,COAP_RESP_CONTENT,req,0,buf,out);
    } else if(path_len(req,2)) {
        uint8_t id=path_id(req,1);
        f_curve_info_t ci; if(f_curve_get_info(h->curve,id,&ci)!=ESP_OK){coap_err(r,COAP_RESP_NOT_FOUND,req);return;}
        CurveInfo pb; curve_to_pb(&ci,&pb);
        uint8_t buf[512]; size_t out;
        if(pb_encode_msg(&pb,CurveInfo_fields,buf,sizeof(buf),&out))
            coap_resp_set(r,COAP_RESP_CONTENT,req,0,buf,out);
    } else coap_err(r,COAP_RESP_BAD_REQ,req);
}

static void h_curves_post(f_coap_handle_t h, const coap_req_t *req, coap_resp_t *r) {
    CurveCreateRequest cr = CurveCreateRequest_init_default;
    if(!pb_decode_msg(req->payload,req->payload_len,&cr,CurveCreateRequest_fields))
    {coap_err(r,COAP_RESP_BAD_REQ,req);return;}
    f_curve_info_t ci; memset(&ci,0,sizeof(ci));
    strncpy(ci.name,cr.name,ESPFM_NAME_MAX-1); ci.num_points=cr.points_count;
    for(int i=0;i<cr.points_count;i++){ci.points[i].temp_c=cr.points[i].temp_c;ci.points[i].duty=(uint8_t)cr.points[i].duty;}
    uint8_t id; if(f_curve_upsert(h->curve,&ci,&id)!=ESP_OK){coap_err(r,COAP_RESP_BAD_REQ,req);return;}
    if(h->config) f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
    f_curve_get_info(h->curve,id,&ci);
    CurveInfo pb; curve_to_pb(&ci,&pb);
    uint8_t buf[512]; size_t out;
    if(pb_encode_msg(&pb,CurveInfo_fields,buf,sizeof(buf),&out))
        coap_resp_set(r,COAP_RESP_CREATED,req,0,buf,out);
}

static void h_curves_put(f_coap_handle_t h, const coap_req_t *req, coap_resp_t *r) {
    uint8_t id=path_id(req,1);
    CurveUpdateRequest ur = CurveUpdateRequest_init_default;
    if(!pb_decode_msg(req->payload,req->payload_len,&ur,CurveUpdateRequest_fields))
    {coap_err(r,COAP_RESP_BAD_REQ,req);return;}
    ur.id=id;
    f_curve_info_t ci; memset(&ci,0,sizeof(ci)); ci.id=id;
    strncpy(ci.name,ur.name,ESPFM_NAME_MAX-1); ci.num_points=ur.points_count;
    for(int i=0;i<ur.points_count;i++){ci.points[i].temp_c=ur.points[i].temp_c;ci.points[i].duty=(uint8_t)ur.points[i].duty;}
    uint8_t out_id; if(f_curve_upsert(h->curve,&ci,&out_id)!=ESP_OK){coap_err(r,COAP_RESP_BAD_REQ,req);return;}
    if(h->config) f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
    f_curve_get_info(h->curve,out_id,&ci);
    CurveInfo pb; curve_to_pb(&ci,&pb);
    uint8_t buf[512]; size_t out;
    if(pb_encode_msg(&pb,CurveInfo_fields,buf,sizeof(buf),&out))
        coap_resp_set(r,COAP_RESP_CHANGED,req,0,buf,out);
}

static void h_curves_delete(f_coap_handle_t h, const coap_req_t *req, coap_resp_t *r) {
    uint8_t id=path_id(req,1);
    if(f_curve_remove(h->curve,id)!=ESP_OK){coap_err(r,COAP_RESP_NOT_FOUND,req);return;}
    if(h->config) f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
    StatusResponse sr={.ok=true};
    uint8_t buf[64]; size_t out;
    pb_encode_msg(&sr,StatusResponse_fields,buf,sizeof(buf),&out);
    coap_resp_set(r,COAP_RESP_DELETED,req,0,buf,out);
}

/* ===== Schedule handlers ===== */
static void h_schedules_get(f_coap_handle_t h, const coap_req_t *req, coap_resp_t *r) {
    ScheduleList list = ScheduleList_init_default;
    for(uint8_t i=0;i<8;i++) {
        f_schedule_info_t si; if(f_schedule_get_info(h->schedule,i,&si)!=ESP_OK) continue;
        sched_to_pb(&si,&list.schedules[list.schedules_count++]);
    }
    uint8_t buf[512]; size_t out;
    if(pb_encode_msg(&list,ScheduleList_fields,buf,sizeof(buf),&out))
        coap_resp_set(r,COAP_RESP_CONTENT,req,0,buf,out);
}

static void h_schedules_post(f_coap_handle_t h, const coap_req_t *req, coap_resp_t *r) {
    ScheduleCreateRequest cr = ScheduleCreateRequest_init_default;
    if(!pb_decode_msg(req->payload,req->payload_len,&cr,ScheduleCreateRequest_fields))
    {coap_err(r,COAP_RESP_BAD_REQ,req);return;}
    f_schedule_info_t si={.fan_id=(uint8_t)cr.fan_id,.duty=(uint8_t)cr.duty,
        .start_min=(uint16_t)cr.start_min,.end_min=(uint16_t)cr.end_min,.enabled=cr.enabled};
    uint8_t id; if(f_schedule_add(h->schedule,&si,&id)!=ESP_OK){coap_err(r,COAP_RESP_BAD_REQ,req);return;}
    if(h->config) f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
    f_schedule_get_info(h->schedule,id,&si);
    ScheduleInfo pb; sched_to_pb(&si,&pb);
    uint8_t buf[256]; size_t out;
    if(pb_encode_msg(&pb,ScheduleInfo_fields,buf,sizeof(buf),&out))
        coap_resp_set(r,COAP_RESP_CREATED,req,0,buf,out);
}

static void h_schedules_put(f_coap_handle_t h, const coap_req_t *req, coap_resp_t *r) {
    uint8_t id=path_id(req,1);
    ScheduleUpdateRequest ur = ScheduleUpdateRequest_init_default;
    if(!pb_decode_msg(req->payload,req->payload_len,&ur,ScheduleUpdateRequest_fields))
    {coap_err(r,COAP_RESP_BAD_REQ,req);return;}
    f_schedule_info_t si={.fan_id=(uint8_t)ur.fan_id,.duty=(uint8_t)ur.duty,
        .start_min=(uint16_t)ur.start_min,.end_min=(uint16_t)ur.end_min,.enabled=ur.enabled};
    if(f_schedule_update(h->schedule,id,&si)!=ESP_OK){coap_err(r,COAP_RESP_NOT_FOUND,req);return;}
    if(h->config) f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
    f_schedule_get_info(h->schedule,id,&si);
    ScheduleInfo pb; sched_to_pb(&si,&pb);
    uint8_t buf[256]; size_t out;
    if(pb_encode_msg(&pb,ScheduleInfo_fields,buf,sizeof(buf),&out))
        coap_resp_set(r,COAP_RESP_CHANGED,req,0,buf,out);
}

static void h_schedules_delete(f_coap_handle_t h, const coap_req_t *req, coap_resp_t *r) {
    uint8_t id=path_id(req,1);
    if(f_schedule_remove(h->schedule,id)!=ESP_OK){coap_err(r,COAP_RESP_NOT_FOUND,req);return;}
    if(h->config) f_config_save_all(h->config,h->fan,h->source,h->curve,h->schedule);
    StatusResponse sr={.ok=true};
    uint8_t buf[64]; size_t out;
    pb_encode_msg(&sr,StatusResponse_fields,buf,sizeof(buf),&out);
    coap_resp_set(r,COAP_RESP_DELETED,req,0,buf,out);
}

/* ===== WiFi handlers ===== */
static void h_wifi_scan(f_coap_handle_t h, const coap_req_t *req, coap_resp_t *r) {
    wifi_scan_config_t sc={.scan_type=WIFI_SCAN_TYPE_ACTIVE,.scan_time.active={.min=100,.max=300}};
    if(esp_wifi_scan_start(&sc,true)!=ESP_OK){coap_err(r,COAP_RESP_SVC_UNAVAIL,req);return;}
    uint16_t n=0; esp_wifi_scan_get_ap_num(&n);
    WifiScanResult sr = WifiScanResult_init_default;
    if(n>0) {
        wifi_ap_record_t *aps=calloc(n,sizeof(wifi_ap_record_t));
        if(aps){esp_wifi_scan_get_ap_records(&n,aps);
            for(int i=0;i<n&&i<16;i++) {
                WifiApRecord *ap=&sr.aps[sr.aps_count++];
                strncpy(ap->ssid,(char*)aps[i].ssid,sizeof(ap->ssid)-1);
                ap->rssi=aps[i].rssi; ap->channel=aps[i].primary; ap->authmode=aps[i].authmode;
            }
            free(aps);
        }
    }
    uint8_t buf[1024]; size_t out;
    if(pb_encode_msg(&sr,WifiScanResult_fields,buf,sizeof(buf),&out))
        coap_resp_set(r,COAP_RESP_CONTENT,req,0,buf,out);
}

static void h_wifi_connect(f_coap_handle_t h, const coap_req_t *req, coap_resp_t *r) {
    WifiConnectRequest cr = WifiConnectRequest_init_default;
    if(!pb_decode_msg(req->payload,req->payload_len,&cr,WifiConnectRequest_fields))
    {coap_err(r,COAP_RESP_BAD_REQ,req);return;}
    wifi_config_t wc={0};
    strncpy((char*)wc.sta.ssid,cr.ssid,sizeof(wc.sta.ssid)-1);
    strncpy((char*)wc.sta.password,cr.password,sizeof(wc.sta.password)-1);
    wc.sta.threshold.authmode=WIFI_AUTH_WPA2_PSK;
    if(esp_wifi_set_config(WIFI_IF_STA,&wc)!=ESP_OK){coap_err(r,COAP_RESP_SVC_UNAVAIL,req);return;}
    esp_wifi_disconnect(); esp_wifi_connect();
    StatusResponse sr={.ok=true};
    uint8_t buf[64]; size_t out;
    pb_encode_msg(&sr,StatusResponse_fields,buf,sizeof(buf),&out);
    coap_resp_set(r,COAP_RESP_CHANGED,req,0,buf,out);
}

static void h_wifi_status(f_coap_handle_t h, const coap_req_t *req, coap_resp_t *r) {
    WifiStatus ws = WifiStatus_init_default;
    esp_netif_t *sta=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if(sta){esp_netif_ip_info_t ip;if(esp_netif_get_ip_info(sta,&ip)==ESP_OK&&ip.ip.addr!=0){
        ws.sta_connected=true;snprintf(ws.sta_ip,sizeof(ws.sta_ip),IPSTR,IP2STR(&ip.ip));}}
    strncpy(ws.ap_ip,"192.168.4.1",sizeof(ws.ap_ip)-1);
    uint8_t buf[128]; size_t out;
    if(pb_encode_msg(&ws,WifiStatus_fields,buf,sizeof(buf),&out))
        coap_resp_set(r,COAP_RESP_CONTENT,req,0,buf,out);
}

/* ===== System info ===== */
static void h_system_info(f_coap_handle_t h, const coap_req_t *req, coap_resp_t *r) {
    SystemInfo si = SystemInfo_init_default;
    snprintf(si.version,sizeof(si.version),"%d.%d.%d",ESPFM_VERSION_MAJOR,ESPFM_VERSION_MINOR,ESPFM_VERSION_PATCH);
    si.uptime_s = (uint32_t)(esp_timer_get_time()/1000000);
    si.heap_free = (uint32_t)esp_get_free_heap_size();
    si.fan_count = h->fan?f_fan_get_count(h->fan):0;
    si.source_count = h->source?f_source_get_count(h->source):0;
    si.curve_count = h->curve?f_curve_get_count(h->curve):0;
    si.schedule_count = h->schedule?f_schedule_get_count(h->schedule):0;
    uint8_t buf[256]; size_t out;
    if(pb_encode_msg(&si,SystemInfo_fields,buf,sizeof(buf),&out))
        coap_resp_set(r,COAP_RESP_CONTENT,req,0,buf,out);
}

/* ===== Request dispatcher ===== */
static void coap_dispatch(f_coap_handle_t h, coap_req_t *req, coap_resp_t *resp) {
    if(req->path.count==0) return;

    const char *base = req->path.seg[0];

    if(!strcmp(base,"fans")) {
        switch(req->code) {
            case COAP_GET: h_fans_get(h,req,resp); break;
            case COAP_POST: h_fans_post(h,req,resp); break;
            case COAP_PUT: h_fans_put(h,req,resp); break;
            case COAP_DELETE: h_fans_delete(h,req,resp); break;
            default: coap_err(resp,COAP_RESP_BAD_REQ,req); break;
        }
    } else if(!strcmp(base,"sources") && req->path.count>=1) {
        if(req->path.count==2 && !strcmp(req->path.seg[1],"temp") && req->code==COAP_POST)
            h_sources_temp(h,req,resp);
        else switch(req->code) {
            case COAP_GET: h_sources_get(h,req,resp); break;
            case COAP_POST: h_sources_post(h,req,resp); break;
            case COAP_DELETE: h_sources_delete(h,req,resp); break;
            default: coap_err(resp,COAP_RESP_BAD_REQ,req); break;
        }
    } else if(!strcmp(base,"curves")) {
        switch(req->code) {
            case COAP_GET: h_curves_get(h,req,resp); break;
            case COAP_POST: h_curves_post(h,req,resp); break;
            case COAP_PUT: h_curves_put(h,req,resp); break;
            case COAP_DELETE: h_curves_delete(h,req,resp); break;
            default: coap_err(resp,COAP_RESP_BAD_REQ,req); break;
        }
    } else if(!strcmp(base,"schedules")) {
        switch(req->code) {
            case COAP_GET: h_schedules_get(h,req,resp); break;
            case COAP_POST: h_schedules_post(h,req,resp); break;
            case COAP_PUT: h_schedules_put(h,req,resp); break;
            case COAP_DELETE: h_schedules_delete(h,req,resp); break;
            default: coap_err(resp,COAP_RESP_BAD_REQ,req); break;
        }
    } else if(!strcmp(base,"wifi")) {
        if(req->path.count==2 && !strcmp(req->path.seg[1],"scan"))      h_wifi_scan(h,req,resp);
        else if(req->path.count==2 && !strcmp(req->path.seg[1],"connect")) h_wifi_connect(h,req,resp);
        else if(req->path.count==2 && !strcmp(req->path.seg[1],"status"))  h_wifi_status(h,req,resp);
        else coap_err(resp,COAP_RESP_NOT_FOUND,req);
    } else if(!strcmp(base,"system") && req->path.count==2 && !strcmp(req->path.seg[1],"info")) {
        h_system_info(h,req,resp);
    } else {
        coap_err(resp,COAP_RESP_NOT_FOUND,req);
    }
}

/* ===== CoAP server task ===== */
static void _coap_task(void *arg) {
    f_coap_handle_t h = (f_coap_handle_t)arg;
    ESP_LOGI(TAG,"CoAP server listening on port %d",COAP_PORT);
    uint8_t rx[COAP_MAX_PKT], tx[COAP_MAX_PKT];

    while(h->running) {
        struct sockaddr_in from; socklen_t flen=sizeof(from);
        int n=recvfrom(h->sock,rx,sizeof(rx),0,(struct sockaddr*)&from,&flen);
        if(n<4) {vTaskDelay(pdMS_TO_TICKS(10));continue;}

        coap_req_t req; if(coap_parse(rx,n,&req)<0) continue;

        coap_resp_t resp; coap_resp_init(&resp,tx,sizeof(tx));
        coap_dispatch(h,&req,&resp);

        if(resp.len>0) sendto(h->sock,tx,resp.len,0,(struct sockaddr*)&from,flen);
    }
    vTaskDelete(NULL);
}

/* ===== WiFi event handlers ===== */
static void _on_wifi_connected(void *arg, esp_event_base_t b, int32_t id, void *d) {
    f_coap_handle_t h=(f_coap_handle_t)arg; if(h) f_coap_start(h);
}
static void _on_wifi_disconnected(void *arg, esp_event_base_t b, int32_t id, void *d) {
    f_coap_handle_t h=(f_coap_handle_t)arg; if(h) f_coap_stop(h);
}

/* ===== Public API ===== */
esp_err_t f_coap_init(f_coap_handle_t *handle, f_fan_handle_t fan,
                      f_source_handle_t source, f_curve_handle_t curve,
                      f_schedule_handle_t schedule, f_config_handle_t config) {
    if(!handle) return ESP_ERR_INVALID_ARG;
    f_coap_handle_t h=calloc(1,sizeof(struct f_coap));
    if(!h) return ESP_ERR_NO_MEM;
    h->fan=fan; h->source=source; h->curve=curve; h->schedule=schedule; h->config=config;
    h->sock=-1; h->running=false;
    esp_event_handler_register(ESPFM_EVENT,ESPFM_EVENT_WIFI_CONNECTED,_on_wifi_connected,h);
    esp_event_handler_register(ESPFM_EVENT,ESPFM_EVENT_WIFI_DISCONNECTED,_on_wifi_disconnected,h);
    *handle=h;
    ESP_LOGI(TAG,"CoAP server initialized (WiFi-aware)");
    return ESP_OK;
}

esp_err_t f_coap_start(f_coap_handle_t h) {
    if(!h||h->running) return ESP_OK;
    h->sock=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(h->sock<0){ESP_LOGE(TAG,"socket failed");return ESP_FAIL;}
    struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(COAP_PORT),.sin_addr.s_addr=INADDR_ANY};
    if(bind(h->sock,(struct sockaddr*)&a,sizeof(a))<0){close(h->sock);h->sock=-1;ESP_LOGE(TAG,"bind failed");return ESP_FAIL;}
    h->running=true;
    xTaskCreate(_coap_task,"coap_srv",COAP_TASK_STACK,h,COAP_TASK_PRIO,&h->task);
    ESP_LOGI(TAG,"CoAP server started on port %d",COAP_PORT);
    return ESP_OK;
}

esp_err_t f_coap_stop(f_coap_handle_t h) {
    if(!h||!h->running) return ESP_OK;
    h->running=false;
    if(h->sock>=0){close(h->sock);h->sock=-1;}
    ESP_LOGI(TAG,"CoAP server stopped");
    return ESP_OK;
}
