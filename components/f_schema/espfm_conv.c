/* espfm_conv.c — shared protobuf <-> native type conversion helpers */
#include "espfm_conv.h"
#include <string.h>

SourceType source_type_to_pb(source_type_t t)
{
    switch (t) {
    case SOURCE_TYPE_NTC:
        return SourceType_SOURCE_TYPE_NTC;
    case SOURCE_TYPE_DS18B20:
        return SourceType_SOURCE_TYPE_DS18B20;
    case SOURCE_TYPE_MANUAL:
        return SourceType_SOURCE_TYPE_MANUAL;
    default:
        return SourceType_SOURCE_TYPE_MANUAL;
    }
}

source_type_t pb_to_source_type(SourceType t)
{
    switch (t) {
    case SourceType_SOURCE_TYPE_NTC:
        return SOURCE_TYPE_NTC;
    case SourceType_SOURCE_TYPE_DS18B20:
        return SOURCE_TYPE_DS18B20;
    default:
        return SOURCE_TYPE_MANUAL;
    }
}

/* ---- Struct conversion: native info -> protobuf ---- */

void espfm_fan_to_pb(const f_fan_info_t *fi, FanInfo *pb)
{
    *pb             = (FanInfo)FanInfo_init_default;
    pb->id          = fi->id;
    pb->mode        = (FanMode)fi->mode;
    pb->duty        = fi->duty;
    pb->rpm         = fi->rpm;
    pb->enabled     = fi->enabled;
    pb->inverted    = fi->inverted;
    pb->pwm_gpio    = fi->pwm_gpio;
    pb->tach_gpio   = fi->tach_gpio;
    pb->source_id   = fi->source_id;
    pb->curve_id    = fi->curve_id;
    pb->schedule_id = fi->schedule_id;
    pb->group_id    = fi->group_id;
    pb->alarm       = (FanAlarm)fi->alarm;
    strncpy(pb->name, fi->name, sizeof(pb->name) - 1);
    pb->name[sizeof(pb->name) - 1] = '\0';
}

void espfm_source_to_pb(const f_source_info_t *si, SourceInfo *pb)
{
    *pb                  = (SourceInfo)SourceInfo_init_default;
    pb->id               = si->id;
    pb->type             = source_type_to_pb(si->type);
    pb->status           = (SourceStatus)si->status;
    pb->temp_c           = si->temp_c;
    pb->gpio             = si->gpio;
    pb->ds18b20_rom_code = si->ds18b20_rom_code;
    strncpy(pb->name, si->name, sizeof(pb->name) - 1);
    pb->name[sizeof(pb->name) - 1] = '\0';
}

void espfm_curve_to_pb(const f_curve_info_t *ci, CurveInfo *pb)
{
    *pb              = (CurveInfo)CurveInfo_init_default;
    pb->id           = ci->id;
    pb->points_count = ci->num_points;
    strncpy(pb->name, ci->name, sizeof(pb->name) - 1);
    pb->name[sizeof(pb->name) - 1] = '\0';
    for (int i = 0; i < ci->num_points && i < F_CURVE_MAX_POINTS; i++) {
        pb->points[i].temp_c = ci->points[i].temp_c;
        pb->points[i].duty   = ci->points[i].duty;
    }
}

void espfm_schedule_to_pb(const f_schedule_info_t *si, ScheduleInfo *pb)
{
    *pb           = (ScheduleInfo)ScheduleInfo_init_default;
    pb->id        = si->id;
    pb->fan_id    = si->fan_id;
    pb->duty      = si->duty;
    pb->start_min = si->start_min;
    pb->end_min   = si->end_min;
    pb->enabled   = si->enabled;
}
