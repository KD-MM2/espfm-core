/* Minimal nanopb runtime — self-contained */
#ifndef PB_H
#define PB_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t pb_byte_t;
typedef unsigned int pb_size_t;

/* Type enums */
typedef enum { PB_HTYPE_REQUIRED=0,PB_HTYPE_OPTIONAL=1,PB_HTYPE_REPEATED=2 } pb_htype_t;
typedef enum { PB_ATYPE_STATIC=0 } pb_atype_t;

/* Non-overlapping field L-types for switch */
#define PB_LTYPE_UVARINT 0
#define PB_LTYPE_UINT32  0
#define PB_LTYPE_UENUM   0
#define PB_LTYPE_BOOL    0
#define PB_LTYPE_SVARINT 1
#define PB_LTYPE_SINT32  1
#define PB_LTYPE_FIXED32 2
#define PB_LTYPE_FLOAT   2
#define PB_LTYPE_FIXED64 3
#define PB_LTYPE_STRING  4
#define PB_LTYPE_BYTES   4
#define PB_LTYPE_SUBMESSAGE 5
#define PB_LTYPE_MESSAGE 5
#define PB_LTYPE_SUBMSG_W_CB 5

/* Helpers */
#define SIZEOF_MEMBER(s,f) sizeof(((s*)0)->f)
#define PB_FIELD(tag,type,htype,atype,first,struc,fld,...) \
    {tag,PB_LTYPE_##type,PB_HTYPE_##htype,PB_ATYPE_##atype,0, \
     offsetof(struc,fld),SIZEOF_MEMBER(struc,fld), \
     offsetof(struc,first),__VA_ARGS__}
#define PB_LAST_FIELD {0,0,0,0,0,0,0,0,0}

/* Field descriptor */
typedef struct pb_field_s {
    uint32_t tag;
    uint8_t type, htype, atype, _pad;
    size_t data_offset, data_size, size_offset;
    const void *ptr;
} pb_field_t;

/* Streams */
struct pb_ostream_s;
struct pb_istream_s;
typedef struct pb_ostream_s {
    bool (*callback)(struct pb_ostream_s*,const pb_byte_t*,size_t);
    void *state; size_t max_size, bytes_written; bool has_error;
} pb_ostream_t;
typedef struct pb_istream_s {
    bool (*callback)(struct pb_istream_s*,pb_byte_t*,size_t);
    void *state; size_t bytes_left; bool has_error;
} pb_istream_t;

static inline pb_ostream_t pb_ostream_from_buffer(pb_byte_t *buf, size_t sz) {
    pb_ostream_t s={0}; s.max_size=sz; s.state=buf; return s;
}
static inline pb_istream_t pb_istream_from_buffer(const pb_byte_t *buf, size_t sz) {
    pb_istream_t s={0}; s.bytes_left=sz; s.state=(void*)buf; return s;
}

static inline bool pb_write(pb_ostream_t *s, const pb_byte_t *d, size_t n) {
    if(s->bytes_written+n>s->max_size){s->has_error=1;return false;}
    memcpy((pb_byte_t*)s->state+s->bytes_written,d,n); s->bytes_written+=n; return true;
}
static inline bool pb_read(pb_istream_t *s, pb_byte_t *d, size_t n) {
    if(s->bytes_left<n){s->has_error=1;return false;}
    memcpy(d,s->state,n); s->state=(pb_byte_t*)s->state+n; s->bytes_left-=n; return true;
}

static bool pb_encode_varint(pb_ostream_t *s, uint64_t v) {
    while(v>0x7F){pb_byte_t b=(v&0x7F)|0x80;if(!pb_write(s,&b,1))return false;v>>=7;}
    return pb_write(s,(pb_byte_t[]){v&0x7F},1);
}
static bool pb_decode_varint(pb_istream_t *s, uint64_t *v) {
    *v=0; int sh=0;
    for(int i=0;i<10;i++){pb_byte_t b;if(!pb_read(s,&b,1))return false;*v|=((uint64_t)(b&0x7F))<<sh;sh+=7;if(!(b&0x80))return true;}
    return false;
}
static uint8_t pb_wt(uint8_t lt) {
    switch(lt){case 0:case 1:return 0;case 3:return 1;case 4:case 5:return 2;case 2:return 5;default:return 0;}
}
static bool pb_encode_tag(pb_ostream_t *s, uint8_t lt, uint32_t tag) {
    return pb_encode_varint(s,((uint64_t)tag<<3)|pb_wt(lt));
}

/* Core encode/decode */
bool pb_encode(pb_ostream_t *s, const pb_field_t f[], const void *src);
bool pb_decode(pb_istream_t *s, const pb_field_t f[], void *dest);

bool pb_encode(pb_ostream_t *s, const pb_field_t f[], const void *src) {
    for(int i=0;f[i].tag;i++) {
        const pb_field_t *fd=&f[i];
        const pb_byte_t *p=((const pb_byte_t*)src)+fd->data_offset;
        size_t cnt=1;
        if(fd->htype==PB_HTYPE_OPTIONAL){int zero=1;for(size_t j=0;j<fd->data_size;j++)if(p[j]){zero=0;}if(zero)continue;}
        if(fd->htype==PB_HTYPE_REPEATED){cnt=*(const pb_size_t*)(((const pb_byte_t*)src)+fd->size_offset);if(!cnt)continue;}
        if(!pb_encode_tag(s,fd->type,fd->tag))return false;
        switch(fd->type) {
        case 0:{uint64_t v=0;memcpy(&v,p,fd->data_size<=4?fd->data_size:4);if(!pb_encode_varint(s,v))return false;}break;
        case 1:{int64_t sv=0;memcpy(&sv,p,fd->data_size<=4?fd->data_size:4);uint64_t v=(uint64_t)((sv<<1)^(sv>>63));if(!pb_encode_varint(s,v))return false;}break;
        case 2: if(!pb_write(s,p,4))return false; break;
        case 4: if(!pb_encode_varint(s,strnlen((const char*)p,fd->data_size))||!pb_write(s,p,strnlen((const char*)p,fd->data_size)))return false; break;
        case 5: if(fd->htype==PB_HTYPE_REPEATED){for(size_t k=0;k<cnt;k++){if(!pb_encode(s,(const pb_field_t*)fd->ptr,(void*)(p+k*fd->data_size)))return false;}}
                else{if(!pb_encode(s,(const pb_field_t*)fd->ptr,(void*)p))return false;} break;
        default:break;
        }
    }
    return !s->has_error;
}

bool pb_decode(pb_istream_t *s, const pb_field_t f[], void *dest) {
    while(s->bytes_left>0) {
        uint64_t tw; if(!pb_decode_varint(s,&tw))return !s->has_error;
        uint32_t tag=tw>>3; uint8_t wt=tw&7;
        int fi=-1; for(int i=0;f[i].tag;i++){if(f[i].tag==tag){fi=i;break;}}
        if(fi<0) {
            if(wt==0){uint64_t v;pb_decode_varint(s,&v);}
            else if(wt==5){pb_byte_t b[4];pb_read(s,b,4);}
            else if(wt==2){uint64_t len;pb_decode_varint(s,&len);s->state=(pb_byte_t*)s->state+len;s->bytes_left-=len;}
            continue;
        }
        const pb_field_t *fd=&f[fi];
        pb_byte_t *p=((pb_byte_t*)dest)+fd->data_offset;
        if(wt==0){uint64_t v;pb_decode_varint(s,&v);if(fd->type==1){int64_t sv=(int64_t)(v>>1);if(v&1)sv=~sv;memcpy(p,&sv,fd->data_size<=4?fd->data_size:4);}else memcpy(p,&v,fd->data_size<=4?fd->data_size:4);}
        else if(wt==5)pb_read(s,p,4);
        else if(wt==2){uint64_t len;pb_decode_varint(s,&len);if(fd->type==4){size_t l=len;if(l>fd->data_size-1)l=fd->data_size-1;pb_read(s,p,l);p[l]=0;}else if(fd->type==5){pb_istream_t sub={0};sub.bytes_left=len;sub.state=s->state;pb_decode(&sub,(const pb_field_t*)fd->ptr,(void*)p);s->state=sub.state;s->bytes_left-=len;}}
        if(fd->htype==PB_HTYPE_REPEATED){pb_size_t *c=(pb_size_t*)(((pb_byte_t*)dest)+fd->size_offset);(*c)++;}
        if(fd->htype==PB_HTYPE_OPTIONAL&&fd->ptr){*(bool*)(((pb_byte_t*)dest)+(uintptr_t)fd->ptr)=true;}
    }
    return !s->has_error;
}

#ifdef __cplusplus
}
#endif
#endif
