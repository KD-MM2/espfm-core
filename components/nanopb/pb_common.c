/* pb_common.c: Common support functions for pb_encode.c and pb_decode.c.
 * 2014 Petteri Aimonen <jpa@kapsi.fi>
 */
#include "pb_common.h"

static bool load_descriptor_values(pb_field_iter_t *iter)
{
    uint32_t word0;
    uint32_t data_offset;
    int_least8_t size_offset;
    if (iter->index >= iter->descriptor->field_count) return false;
    word0 = PB_PROGMEM_READU32(iter->descriptor->field_info[iter->field_info_index]);
    iter->type = (pb_type_t)((word0 >> 8) & 0xFF);
    switch(word0 & 3) {
        case 0: iter->array_size=1; iter->tag=(pb_size_t)((word0>>2)&0x3F); size_offset=(int_least8_t)((word0>>24)&0x0F); data_offset=(word0>>16)&0xFF; iter->data_size=(pb_size_t)((word0>>28)&0x0F); break;
        case 1: { uint32_t w1=PB_PROGMEM_READU32(iter->descriptor->field_info[iter->field_info_index+1]); iter->array_size=(pb_size_t)((word0>>16)&0x0FFF); iter->tag=(pb_size_t)(((word0>>2)&0x3F)|((w1>>28)<<6)); size_offset=(int_least8_t)((word0>>28)&0x0F); data_offset=w1&0xFFFF; iter->data_size=(pb_size_t)((w1>>16)&0x0FFF); break; }
        default: { uint32_t w1=PB_PROGMEM_READU32(iter->descriptor->field_info[iter->field_info_index+1]); uint32_t w2=PB_PROGMEM_READU32(iter->descriptor->field_info[iter->field_info_index+2]); uint32_t w3=PB_PROGMEM_READU32(iter->descriptor->field_info[iter->field_info_index+3]); iter->array_size=(pb_size_t)(word0>>16); iter->tag=(pb_size_t)(((word0>>2)&0x3F)|((w1>>8)<<6)); size_offset=(int_least8_t)(w1&0xFF); data_offset=w2; iter->data_size=(pb_size_t)w3; break; }
    }
    if(!iter->message){iter->pField=NULL;iter->pSize=NULL;}
    else{iter->pField=(char*)iter->message+data_offset;
        if(size_offset)iter->pSize=(char*)iter->pField-size_offset;
        else if(PB_HTYPE(iter->type)==PB_HTYPE_REPEATED&&(PB_ATYPE(iter->type)==PB_ATYPE_STATIC||PB_ATYPE(iter->type)==PB_ATYPE_POINTER))iter->pSize=&iter->array_size;
        else iter->pSize=NULL;
        if(PB_ATYPE(iter->type)==PB_ATYPE_POINTER&&iter->pField!=NULL)iter->pData=*(void**)iter->pField; else iter->pData=iter->pField;}
    if(PB_LTYPE_IS_SUBMSG(iter->type))iter->submsg_desc=iter->descriptor->submsg_info[iter->submessage_index];else iter->submsg_desc=NULL;
    return true;
}

static void advance_iterator(pb_field_iter_t *iter)
{
    iter->index++;
    if(iter->index>=iter->descriptor->field_count){iter->index=0;iter->field_info_index=0;iter->submessage_index=0;iter->required_field_index=0;}
    else{uint32_t prev=PB_PROGMEM_READU32(iter->descriptor->field_info[iter->field_info_index]); pb_type_t pt=(prev>>8)&0xFF; pb_size_t dl=(pb_size_t)(1<<(prev&3));
        iter->field_info_index=(pb_size_t)(iter->field_info_index+dl);
        iter->required_field_index=(pb_size_t)(iter->required_field_index+(PB_HTYPE(pt)==PB_HTYPE_REQUIRED));
        iter->submessage_index=(pb_size_t)(iter->submessage_index+PB_LTYPE_IS_SUBMSG(pt));}
}

bool pb_field_iter_begin(pb_field_iter_t *iter, const pb_msgdesc_t *desc, void *message) { memset(iter,0,sizeof(*iter)); iter->descriptor=desc; iter->message=message; return load_descriptor_values(iter); }
bool pb_field_iter_next(pb_field_iter_t *iter) { advance_iterator(iter); (void)load_descriptor_values(iter); return iter->index!=0; }
bool pb_field_iter_find(pb_field_iter_t *iter, uint32_t tag)
{
    if(iter->tag==tag)return true;
    if(tag>iter->descriptor->largest_tag)return false;
    pb_size_t start=iter->index; uint32_t fi;
    if(tag<iter->tag)iter->index=iter->descriptor->field_count;
    do{advance_iterator(iter); fi=PB_PROGMEM_READU32(iter->descriptor->field_info[iter->field_info_index]);
        if(((fi>>2)&0x3F)==(tag&0x3F)){(void)load_descriptor_values(iter); if(iter->tag==tag&&PB_LTYPE(iter->type)!=PB_LTYPE_EXTENSION)return true;}}while(iter->index!=start);
    (void)load_descriptor_values(iter); return false;
}

static void *pb_const_cast(const void *p){union{void *p1;const void *p2;}t;t.p2=p;return t.p1;}
bool pb_field_iter_begin_const(pb_field_iter_t *iter, const pb_msgdesc_t *desc, const void *message){return pb_field_iter_begin(iter,desc,pb_const_cast(message));}
bool pb_default_field_callback(pb_istream_t *istream, pb_ostream_t *ostream, const pb_field_t *field) { return true; }
