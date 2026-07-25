/* f_coap_conv.c — thin wrappers delegating to shared espfm_conv helpers */
#include "f_coap_internal.h"
#include "espfm_conv.h"

void f_coap_fan_to_pb(const f_fan_info_t *fi, FanInfo *pb)
{
    espfm_fan_to_pb(fi, pb);
}

void f_coap_source_to_pb(const f_source_info_t *si, SourceInfo *pb)
{
    espfm_source_to_pb(si, pb);
}

void f_coap_curve_to_pb(const f_curve_info_t *ci, CurveInfo *pb)
{
    espfm_curve_to_pb(ci, pb);
}

void f_coap_schedule_to_pb(const f_schedule_info_t *sci, ScheduleInfo *pb)
{
    espfm_schedule_to_pb(sci, pb);
}
