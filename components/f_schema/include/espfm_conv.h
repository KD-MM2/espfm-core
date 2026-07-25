/* espfm_conv.h — shared protobuf <-> native type conversion helpers */
#pragma once
#include "f_core.h"
#include "f_fan.h"
#include "f_source.h"
#include "f_curve.h"
#include "f_schedule.h"
#include "espfm.pb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Enum conversion */
SourceType source_type_to_pb(source_type_t t);
source_type_t pb_to_source_type(SourceType t);

/* Struct conversion: native info -> protobuf message */
void espfm_fan_to_pb(const f_fan_info_t *fi, FanInfo *pb);
void espfm_source_to_pb(const f_source_info_t *si, SourceInfo *pb);
void espfm_curve_to_pb(const f_curve_info_t *ci, CurveInfo *pb);
void espfm_schedule_to_pb(const f_schedule_info_t *si, ScheduleInfo *pb);

#ifdef __cplusplus
}
#endif
