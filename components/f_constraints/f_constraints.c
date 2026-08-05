#include "f_constraints.h"
#include "f_fan.h"
#include "f_source.h"
#include "f_schedule.h"

static const char *ERR_DUTY        = "duty must be 0-100";
static const char *ERR_MODE        = "mode must be 0 (manual) or 1 (auto)";
static const char *ERR_GPIO        = "GPIO must be 0-48";
static const char *ERR_TEMP        = "temp_c must be -40.0 to 125.0";
static const char *ERR_SCHED_RANGE = "start_min/end_min must be 0-1439";
static const char *ERR_CURVE_COUNT = "curve must have 2-16 points";
static const char *ERR_CURVE_ORDER = "curve points must be sorted by temp_c ascending";
static const char *ERR_FAN_FULL    = "max fans reached (8)";
static const char *ERR_SOURCE_FULL = "max sources reached (8)";
static const char *ERR_CURVE_FULL  = "max curves reached (16)";
static const char *ERR_SCHED_FULL  = "max schedules reached (8)";

esp_err_t f_constraints_duty(int val, const char **err_msg)
{
    if (val < F_CONSTRAINT_DUTY_MIN || val > F_CONSTRAINT_DUTY_MAX) {
        if (err_msg) *err_msg = ERR_DUTY;
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t f_constraints_mode(int val, const char **err_msg)
{
    if (val < F_CONSTRAINT_MODE_MIN || val > F_CONSTRAINT_MODE_MAX) {
        if (err_msg) *err_msg = ERR_MODE;
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t f_constraints_gpio(int val, const char **err_msg)
{
    if (val < F_CONSTRAINT_GPIO_MIN || val > F_CONSTRAINT_GPIO_MAX) {
        if (err_msg) *err_msg = ERR_GPIO;
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t f_constraints_temp_c(float val, const char **err_msg)
{
    if (val < F_CONSTRAINT_TEMP_C_MIN || val > F_CONSTRAINT_TEMP_C_MAX) {
        if (err_msg) *err_msg = ERR_TEMP;
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t f_constraints_schedule_time(int start_min, int end_min, const char **err_msg)
{
    if (start_min < F_CONSTRAINT_SCHED_MIN || start_min > F_CONSTRAINT_SCHED_MAX ||
        end_min < F_CONSTRAINT_SCHED_MIN || end_min > F_CONSTRAINT_SCHED_MAX) {
        if (err_msg) *err_msg = ERR_SCHED_RANGE;
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t f_constraints_curve_points(const f_curve_point_t *points, uint8_t count,
                                     const char **err_msg)
{
    if (count < F_CONSTRAINT_CURVE_POINTS_MIN || count > F_CURVE_MAX_POINTS) {
        if (err_msg) *err_msg = ERR_CURVE_COUNT;
        return ESP_ERR_INVALID_ARG;
    }
    for (uint8_t i = 1; i < count; i++) {
        if (points[i].temp_c <= points[i - 1].temp_c) {
            if (err_msg) *err_msg = ERR_CURVE_ORDER;
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

esp_err_t f_constraints_fan_count(uint8_t current, const char **err_msg)
{
    if (current >= F_FAN_MAX_COUNT) {
        if (err_msg) *err_msg = ERR_FAN_FULL;
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t f_constraints_source_count(uint8_t current, const char **err_msg)
{
    if (current >= F_SOURCE_MAX_COUNT) {
        if (err_msg) *err_msg = ERR_SOURCE_FULL;
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t f_constraints_curve_count(uint8_t current, const char **err_msg)
{
    if (current >= F_CURVE_MAX_COUNT) {
        if (err_msg) *err_msg = ERR_CURVE_FULL;
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t f_constraints_schedule_count(uint8_t current, const char **err_msg)
{
    if (current >= F_SCHEDULE_MAX_COUNT) {
        if (err_msg) *err_msg = ERR_SCHED_FULL;
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}
