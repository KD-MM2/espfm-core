#include "f_config.h"
#include "f_constraints.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "esp_timer.h"
#include "espfm_conv.h"
#include "espfm.pb.h"
#include "pb_encode.h"
#include "pb_decode.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "f_config";

#define CONFIG_FILENAME     "/littlefs/config.pb"
#define SAVE_DEBOUNCE_US    3000000 /* 3 seconds */
#define CONFIG_ENC_BUF_SIZE 8192
#define CONFIG_FILE_MAX     8192

struct f_config {
    char partition_label[32];
    bool mounted;
    uint64_t last_save_us;
};

esp_err_t f_config_init(f_config_handle_t *handle, const char *partition_label,
                        const char *mount_point)
{
    if (handle == NULL || partition_label == NULL || mount_point == NULL)
        return ESP_ERR_INVALID_ARG;

    f_config_handle_t h = calloc(1, sizeof(struct f_config));
    if (h == NULL) return ESP_ERR_NO_MEM;
    strncpy(h->partition_label, partition_label, sizeof(h->partition_label) - 1);

    esp_vfs_littlefs_conf_t conf = {
        .base_path              = mount_point,
        .partition_label        = partition_label,
        .format_if_mount_failed = true,
        .dont_mount             = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount LittleFS: %s", esp_err_to_name(err));
        free(h);
        return err;
    }
    h->mounted   = true;

    size_t total = 0, used = 0;
    esp_littlefs_info(partition_label, &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted at %s (total=%d, used=%d)", mount_point, total, used);

    *handle = h;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Save                                                               */
/* ------------------------------------------------------------------ */

esp_err_t f_config_export_all(f_fan_handle_t fan, f_source_handle_t source, f_curve_handle_t curve,
                              f_schedule_handle_t schedule, uint8_t **buf_out, size_t *len_out)
{
    if (buf_out == NULL || len_out == NULL) return ESP_ERR_INVALID_ARG;

    /* Build ConfigFile on heap (large struct, ~3757 bytes) */
    ConfigFile *cfg = calloc(1, sizeof(ConfigFile));
    if (cfg == NULL) return ESP_ERR_NO_MEM;
    *cfg = (ConfigFile)ConfigFile_init_default;
    strncpy(cfg->version, "3.0", sizeof(cfg->version) - 1);

    /* Fans */
    cfg->has_fans = true;
    cfg->fans     = (FanList)FanList_init_default;
    if (fan) {
        for (uint8_t i = 0; i < F_FAN_MAX_COUNT; i++) {
            f_fan_info_t fi;
            if (f_fan_get_info(fan, i, &fi) == ESP_OK)
                espfm_fan_to_pb(&fi, &cfg->fans.fans[cfg->fans.fans_count++]);
        }
    }

    /* Sources */
    cfg->has_sources = true;
    cfg->sources     = (SourceList)SourceList_init_default;
    if (source) {
        for (uint8_t i = 0; i < F_SOURCE_MAX_COUNT; i++) {
            f_source_info_t si;
            if (f_source_get_info(source, i, &si) == ESP_OK)
                espfm_source_to_pb(&si, &cfg->sources.sources[cfg->sources.sources_count++]);
        }
    }

    /* Curves */
    cfg->has_curves = true;
    cfg->curves     = (CurveList)CurveList_init_default;
    if (curve) {
        for (uint8_t i = 0; i < F_CURVE_MAX_COUNT; i++) {
            f_curve_info_t ci;
            if (f_curve_get_info(curve, i, &ci) == ESP_OK)
                espfm_curve_to_pb(&ci, &cfg->curves.curves[cfg->curves.curves_count++]);
        }
    }

    /* Schedules */
    if (schedule != NULL) {
        cfg->has_schedules = true;
        cfg->schedules     = (ScheduleList)ScheduleList_init_default;
        for (uint8_t i = 0; i < F_SCHEDULE_MAX_COUNT; i++) {
            f_schedule_info_t si;
            if (f_schedule_get_info(schedule, i, &si) == ESP_OK)
                espfm_schedule_to_pb(&si,
                                     &cfg->schedules.schedules[cfg->schedules.schedules_count++]);
        }
    }

    /* Encode to heap-allocated buffer sized to nanopb worst case */
    uint8_t *enc_buf = calloc(1, ConfigFile_size);
    if (enc_buf == NULL) {
        free(cfg);
        return ESP_ERR_NO_MEM;
    }
    pb_ostream_t os = pb_ostream_from_buffer(enc_buf, ConfigFile_size);
    if (!pb_encode(&os, &ConfigFile_msg, cfg)) {
        ESP_LOGE(TAG, "Protobuf encode failed: %s", os.errmsg);
        free(cfg);
        free(enc_buf);
        return ESP_FAIL;
    }

    *buf_out = enc_buf;
    *len_out = os.bytes_written;
    free(cfg);
    return ESP_OK;
}

static esp_err_t f_config_save_all_internal(f_config_handle_t handle, f_fan_handle_t fan,
                                            f_source_handle_t source, f_curve_handle_t curve,
                                            f_schedule_handle_t schedule, bool force)
{
    if (handle == NULL || !handle->mounted) return ESP_ERR_INVALID_STATE;

    /* Debounce (skipped entirely when force is true, e.g. config import before reboot) */
    if (!force) {
        uint64_t now = esp_timer_get_time();
        if (now - handle->last_save_us < SAVE_DEBOUNCE_US) {
            ESP_LOGD(TAG, "Save debounced");
            return ESP_OK;
        }
        handle->last_save_us = now;
    }

    /* Export ConfigFile to heap-allocated protobuf bytes */
    uint8_t *enc_buf = NULL;
    size_t enc_len   = 0;
    esp_err_t err    = f_config_export_all(fan, source, curve, schedule, &enc_buf, &enc_len);
    if (err != ESP_OK) return err;

    /* Write to LittleFS */
    FILE *f = fopen(CONFIG_FILENAME, "w");
    if (f == NULL) {
        free(enc_buf);
        return ESP_FAIL;
    }
    size_t written = fwrite(enc_buf, 1, enc_len, f);
    fclose(f);
    free(enc_buf);

    if (written != enc_len) {
        ESP_LOGE(TAG, "Short write (%zu/%zu)", written, enc_len);
        return ESP_FAIL;
    }

    size_t fan_n = 0, src_n = 0, cur_n = 0, sched_n = 0;
    if (fan) fan_n = f_fan_get_count(fan);
    if (source) src_n = f_source_get_count(source);
    if (curve) cur_n = f_curve_get_count(curve);
    if (schedule) sched_n = f_schedule_get_count(schedule);

    ESP_LOGI(TAG,
             "Config saved as protobuf (%zu bytes, %zu fans, %zu sources, "
             "%zu curves, %zu schedules)",
             enc_len, fan_n, src_n, cur_n, sched_n);
    return ESP_OK;
}

esp_err_t f_config_save_all(f_config_handle_t handle, f_fan_handle_t fan, f_source_handle_t source,
                            f_curve_handle_t curve, f_schedule_handle_t schedule)
{
    return f_config_save_all_internal(handle, fan, source, curve, schedule, false);
}

esp_err_t f_config_save_all_forced(f_config_handle_t handle, f_fan_handle_t fan,
                                   f_source_handle_t source, f_curve_handle_t curve,
                                   f_schedule_handle_t schedule)
{
    return f_config_save_all_internal(handle, fan, source, curve, schedule, true);
}

/* ------------------------------------------------------------------ */
/*  Load                                                               */
/* ------------------------------------------------------------------ */

esp_err_t f_config_load_all(f_config_handle_t handle, f_fan_handle_t fan, f_source_handle_t source,
                            f_curve_handle_t curve, f_schedule_handle_t schedule)
{
    if (handle == NULL || !handle->mounted) return ESP_ERR_INVALID_STATE;

    FILE *f = fopen(CONFIG_FILENAME, "r");
    if (f == NULL) {
        ESP_LOGI(TAG, "No config.pb found, starting fresh");
        return ESP_OK;
    }

    /* Read file into buffer */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > CONFIG_FILE_MAX) {
        ESP_LOGW(TAG, "Config file size %ld out of range, skipping", fsize);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buf = malloc((size_t)fsize);
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t bytes_read = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    if (bytes_read != (size_t)fsize) {
        ESP_LOGE(TAG, "Short read (%zu/%ld)", bytes_read, fsize);
        free(buf);
        return ESP_FAIL;
    }

    /* Decode protobuf */
    ConfigFile *cfg = calloc(1, sizeof(ConfigFile));
    if (cfg == NULL) {
        free(buf);
        return ESP_ERR_NO_MEM;
    }
    *cfg            = (ConfigFile)ConfigFile_init_default;

    pb_istream_t is = pb_istream_from_buffer(buf, (size_t)fsize);
    if (!pb_decode(&is, &ConfigFile_msg, cfg)) {
        ESP_LOGE(TAG, "Protobuf decode failed: %s", is.errmsg);
        free(cfg);
        free(buf);
        return ESP_ERR_INVALID_ARG;
    }
    free(buf);

    size_t fan_count = 0, src_count = 0, cur_count = 0, sched_count = 0;

    /* -- Fans -- */
    if (cfg->has_fans) {
        for (pb_size_t i = 0; i < cfg->fans.fans_count; i++) {
            const FanInfo *pb = &cfg->fans.fans[i];
            if (f_constraints_gpio((int)pb->pwm_gpio, NULL) != ESP_OK) continue;

            uint8_t new_id;
            if (f_fan_add(fan, (uint8_t)pb->pwm_gpio, (uint8_t)pb->tach_gpio, pb->name, &new_id,
                          NULL) != ESP_OK)
                continue;

            if (f_constraints_mode((int)pb->mode, NULL) == ESP_OK)
                f_fan_set_mode(fan, new_id, (fan_mode_t)pb->mode);

            if (f_constraints_duty((int)pb->duty, NULL) == ESP_OK)
                f_fan_set_duty(fan, new_id, (uint8_t)pb->duty);

            f_fan_set_group(fan, new_id, (uint8_t)pb->group_id);
            f_fan_set_inverted(fan, new_id, pb->inverted);

            if (pb->source_id != 0xFF) f_fan_set_source(fan, new_id, (uint8_t)pb->source_id);
            if (pb->curve_id != 0xFF) f_fan_set_curve(fan, new_id, (uint8_t)pb->curve_id);
            if (pb->schedule_id != 0xFF) f_fan_set_schedule(fan, new_id, (uint8_t)pb->schedule_id);

            fan_count++;
        }
    }

    /* -- Sources -- */
    if (cfg->has_sources) {
        for (pb_size_t i = 0; i < cfg->sources.sources_count; i++) {
            const SourceInfo *pb = &cfg->sources.sources[i];
            source_type_t stype  = pb_to_source_type(pb->type);
            uint8_t new_id;

            if (stype == SOURCE_TYPE_DS18B20) {
                if (f_source_add_ds18b20(source, pb->ds18b20_rom_code, pb->name, &new_id) != ESP_OK)
                    continue;
            } else {
                /* Manual sources use gpio=255 (GPIO_NONE), skip GPIO validation */
                if (stype != SOURCE_TYPE_MANUAL &&
                    f_constraints_gpio((int)pb->gpio, NULL) != ESP_OK)
                    continue;
                if (f_source_add(source, stype, (uint8_t)pb->gpio, pb->name, &new_id, NULL) !=
                    ESP_OK)
                    continue;
                if (stype == SOURCE_TYPE_MANUAL && pb->temp_c != 0.0f)
                    f_source_update_manual(source, new_id, pb->temp_c);
            }
            src_count++;
        }
    }

    /* -- Curves -- */
    if (cfg->has_curves) {
        for (pb_size_t i = 0; i < cfg->curves.curves_count; i++) {
            const CurveInfo *pb = &cfg->curves.curves[i];
            f_curve_info_t ci;
            memset(&ci, 0, sizeof(ci));
            ci.id = (uint8_t)pb->id;
            strncpy(ci.name, pb->name, sizeof(ci.name) - 1);
            ci.name[sizeof(ci.name) - 1] = '\0';
            ci.num_points                = pb->points_count;
            for (int j = 0; j < pb->points_count && j < F_CURVE_MAX_POINTS; j++) {
                ci.points[j].temp_c = pb->points[j].temp_c;
                ci.points[j].duty   = (uint8_t)pb->points[j].duty;
            }
            uint8_t out_id;
            if (f_curve_upsert(curve, &ci, &out_id) == ESP_OK) cur_count++;
        }
    }

    /* -- Schedules -- */
    if (cfg->has_schedules && schedule != NULL) {
        for (pb_size_t i = 0; i < cfg->schedules.schedules_count; i++) {
            const ScheduleInfo *pb = &cfg->schedules.schedules[i];
            f_schedule_info_t si   = {.fan_id    = (uint8_t)pb->fan_id,
                                      .duty      = (uint8_t)pb->duty,
                                      .start_min = (uint16_t)pb->start_min,
                                      .end_min   = (uint16_t)pb->end_min,
                                      .enabled   = pb->enabled};
            strncpy(si.name, pb->name, ESPFM_NAME_MAX - 1);
            si.name[ESPFM_NAME_MAX - 1] = '\0';
            uint8_t out_id;
            if (f_schedule_add(schedule, &si, &out_id) == ESP_OK) sched_count++;
        }
    }

    free(cfg);

    ESP_LOGI(TAG, "Config loaded (%zu fans, %zu sources, %zu curves, %zu schedules)", fan_count,
             src_count, cur_count, sched_count);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Import (strict validate -> clear -> apply -> force-persist)       */
/* ------------------------------------------------------------------ */

static esp_err_t f_config_validate_import(const ConfigFile *cfg, const char **err_msg)
{
    /* Per-registry capacity first. The count guards test whether one more entry can be
     * added (current >= MAX rejects), so passing count - 1 accepts a full list of exactly
     * MAX entries and rejects count > MAX. A zero-count list is always valid. */
    if (cfg->fans.fans_count > 0 &&
        f_constraints_fan_count((uint8_t)(cfg->fans.fans_count - 1), err_msg) != ESP_OK)
        return ESP_ERR_INVALID_ARG;
    if (cfg->sources.sources_count > 0 &&
        f_constraints_source_count((uint8_t)(cfg->sources.sources_count - 1), err_msg) != ESP_OK)
        return ESP_ERR_INVALID_ARG;
    if (cfg->curves.curves_count > 0 &&
        f_constraints_curve_count((uint8_t)(cfg->curves.curves_count - 1), err_msg) != ESP_OK)
        return ESP_ERR_INVALID_ARG;
    if (cfg->schedules.schedules_count > 0 &&
        f_constraints_schedule_count((uint8_t)(cfg->schedules.schedules_count - 1), err_msg) !=
            ESP_OK)
        return ESP_ERR_INVALID_ARG;

    /* Fans */
    for (pb_size_t i = 0; i < cfg->fans.fans_count; i++) {
        const FanInfo *pb = &cfg->fans.fans[i];
        if (f_constraints_gpio((int)pb->pwm_gpio, err_msg) != ESP_OK) return ESP_ERR_INVALID_ARG;
        if (pb->tach_gpio != F_FAN_TACH_NONE &&
            f_constraints_gpio((int)pb->tach_gpio, err_msg) != ESP_OK)
            return ESP_ERR_INVALID_ARG;
        if (f_constraints_mode((int)pb->mode, err_msg) != ESP_OK) return ESP_ERR_INVALID_ARG;
        if (f_constraints_duty((int)pb->duty, err_msg) != ESP_OK) return ESP_ERR_INVALID_ARG;
    }

    /* Sources: only NTC sources carry a GPIO; MANUAL/DS18B20 use the 255 sentinel.
     * MANUAL temp_c must be within the physical range so the f_source_update_manual
     * call in apply cannot fail after the registries are cleared. */
    for (pb_size_t i = 0; i < cfg->sources.sources_count; i++) {
        const SourceInfo *pb = &cfg->sources.sources[i];
        source_type_t stype  = pb_to_source_type(pb->type);
        if (stype == SOURCE_TYPE_NTC && f_constraints_gpio((int)pb->gpio, err_msg) != ESP_OK)
            return ESP_ERR_INVALID_ARG;
        if (stype == SOURCE_TYPE_MANUAL && f_constraints_temp_c(pb->temp_c, err_msg) != ESP_OK)
            return ESP_ERR_INVALID_ARG;
    }

    /* Curves: point count/order via the curve guard, then per-point duty. */
    for (pb_size_t i = 0; i < cfg->curves.curves_count; i++) {
        const CurveInfo *pb = &cfg->curves.curves[i];
        f_curve_point_t pts[F_CURVE_MAX_POINTS];
        for (pb_size_t j = 0; j < pb->points_count && j < F_CURVE_MAX_POINTS; j++) {
            pts[j].temp_c = pb->points[j].temp_c;
            pts[j].duty   = (uint8_t)pb->points[j].duty;
        }
        if (f_constraints_curve_points(pts, (uint8_t)pb->points_count, err_msg) != ESP_OK)
            return ESP_ERR_INVALID_ARG;
        for (pb_size_t j = 0; j < pb->points_count; j++) {
            if (f_constraints_duty((int)pb->points[j].duty, err_msg) != ESP_OK)
                return ESP_ERR_INVALID_ARG;
        }
    }

    /* Schedules: range/duty plus a structural fan_id < fans_count guarantee so that
     * f_schedule_add cannot fail after the clear (fans occupy slots 0..fans_count-1). */
    for (pb_size_t i = 0; i < cfg->schedules.schedules_count; i++) {
        const ScheduleInfo *pb = &cfg->schedules.schedules[i];
        if (f_constraints_schedule_time((int)pb->start_min, (int)pb->end_min, err_msg) != ESP_OK)
            return ESP_ERR_INVALID_ARG;
        if (f_constraints_duty((int)pb->duty, err_msg) != ESP_OK) return ESP_ERR_INVALID_ARG;
        if (pb->fan_id >= cfg->fans.fans_count) {
            if (err_msg) *err_msg = "schedule references non-existent fan";
            return ESP_ERR_INVALID_ARG;
        }
    }

    return ESP_OK;
}

static esp_err_t f_config_clear_all(f_fan_handle_t fan, f_source_handle_t source,
                                    f_curve_handle_t curve, f_schedule_handle_t schedule)
{
    /* Dependency-safe order: schedules first (stops the schedule timer when the count
     * reaches 0), then fans (releases LEDC/PCNT channels), then curves, then sources.
     * Empty slots return ESP_ERR_NOT_FOUND and NULL handles return ESP_ERR_INVALID_ARG;
     * both are ignored. */
    for (uint8_t id = 0; id < F_SCHEDULE_MAX_COUNT; id++) f_schedule_remove(schedule, id);
    for (uint8_t id = 0; id < F_FAN_MAX_COUNT; id++) f_fan_remove(fan, id);
    for (uint8_t id = 0; id < F_CURVE_MAX_COUNT; id++) f_curve_remove(curve, id);
    for (uint8_t id = 0; id < F_SOURCE_MAX_COUNT; id++) f_source_remove(source, id);
    return ESP_OK;
}

static esp_err_t f_config_apply_import(f_fan_handle_t fan, f_source_handle_t source,
                                       f_curve_handle_t curve, f_schedule_handle_t schedule,
                                       const ConfigFile *cfg)
{
    /* Fans first: f_fan_add assigns free slots 0..fans_count-1 in file order, which is
     * what the schedule fan_id structural check relies on. */
    for (pb_size_t i = 0; i < cfg->fans.fans_count; i++) {
        const FanInfo *pb = &cfg->fans.fans[i];
        uint8_t new_id;
        esp_err_t e =
            f_fan_add(fan, (uint8_t)pb->pwm_gpio, (uint8_t)pb->tach_gpio, pb->name, &new_id, NULL);
        if (e != ESP_OK) return e;
        e = f_fan_set_mode(fan, new_id, (fan_mode_t)pb->mode);
        if (e != ESP_OK) return e;
        e = f_fan_set_duty(fan, new_id, (uint8_t)pb->duty);
        if (e != ESP_OK) return e;
        e = f_fan_set_group(fan, new_id, (uint8_t)pb->group_id);
        if (e != ESP_OK) return e;
        e = f_fan_set_inverted(fan, new_id, pb->inverted);
        if (e != ESP_OK) return e;
        if (pb->source_id != 0xFF) {
            e = f_fan_set_source(fan, new_id, (uint8_t)pb->source_id);
            if (e != ESP_OK) return e;
        }
        if (pb->curve_id != 0xFF) {
            e = f_fan_set_curve(fan, new_id, (uint8_t)pb->curve_id);
            if (e != ESP_OK) return e;
        }
        if (pb->schedule_id != 0xFF) {
            e = f_fan_set_schedule(fan, new_id, (uint8_t)pb->schedule_id);
            if (e != ESP_OK) return e;
        }
    }

    /* Sources */
    for (pb_size_t i = 0; i < cfg->sources.sources_count; i++) {
        const SourceInfo *pb = &cfg->sources.sources[i];
        source_type_t stype  = pb_to_source_type(pb->type);
        uint8_t new_id;
        if (stype == SOURCE_TYPE_DS18B20) {
            esp_err_t e = f_source_add_ds18b20(source, pb->ds18b20_rom_code, pb->name, &new_id);
            if (e != ESP_OK) return e;
        } else {
            esp_err_t e = f_source_add(source, stype, (uint8_t)pb->gpio, pb->name, &new_id, NULL);
            if (e != ESP_OK) return e;
            if (stype == SOURCE_TYPE_MANUAL && pb->temp_c != 0.0f) {
                e = f_source_update_manual(source, new_id, pb->temp_c);
                if (e != ESP_OK) return e;
            }
        }
    }

    /* Curves: mirrors f_config_load_all; f_curve_upsert uses the requested id when its
     * slot is already occupied and otherwise assigns a free slot. */
    for (pb_size_t i = 0; i < cfg->curves.curves_count; i++) {
        const CurveInfo *pb = &cfg->curves.curves[i];
        f_curve_info_t ci;
        memset(&ci, 0, sizeof(ci));
        ci.id = (uint8_t)pb->id;
        strncpy(ci.name, pb->name, sizeof(ci.name) - 1);
        ci.name[sizeof(ci.name) - 1] = '\0';
        ci.num_points                = (uint8_t)pb->points_count;
        for (pb_size_t j = 0; j < pb->points_count && j < F_CURVE_MAX_POINTS; j++) {
            ci.points[j].temp_c = pb->points[j].temp_c;
            ci.points[j].duty   = (uint8_t)pb->points[j].duty;
        }
        uint8_t out_id;
        esp_err_t e = f_curve_upsert(curve, &ci, &out_id);
        if (e != ESP_OK) return e;
    }

    /* Schedules last: f_schedule_add re-validates the fan_id against the live registry. */
    for (pb_size_t i = 0; i < cfg->schedules.schedules_count; i++) {
        const ScheduleInfo *pb = &cfg->schedules.schedules[i];
        f_schedule_info_t si   = {.fan_id    = (uint8_t)pb->fan_id,
                                  .duty      = (uint8_t)pb->duty,
                                  .start_min = (uint16_t)pb->start_min,
                                  .end_min   = (uint16_t)pb->end_min,
                                  .enabled   = pb->enabled};
        strncpy(si.name, pb->name, ESPFM_NAME_MAX - 1);
        si.name[ESPFM_NAME_MAX - 1] = '\0';
        uint8_t out_id;
        esp_err_t e = f_schedule_add(schedule, &si, &out_id);
        if (e != ESP_OK) return e;
    }

    return ESP_OK;
}

esp_err_t f_config_import_all(f_config_handle_t handle, f_fan_handle_t fan,
                              f_source_handle_t source, f_curve_handle_t curve,
                              f_schedule_handle_t schedule, const ConfigFile *cfg,
                              const char **err_msg)
{
    if (err_msg != NULL) *err_msg = NULL;
    if (handle == NULL || !handle->mounted || cfg == NULL) return ESP_ERR_INVALID_ARG;

    /* Strict validate-then-mutate: any invalid entry aborts with zero mutation. */
    esp_err_t e = f_config_validate_import(cfg, err_msg);
    if (e != ESP_OK) return e;

    f_config_clear_all(fan, source, curve, schedule);

    e = f_config_apply_import(fan, source, curve, schedule, cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "import apply failed: %s", esp_err_to_name(e));
        return e;
    }

    /* Force-persist past the 3s save debounce (the import path reboots shortly after). */
    e = f_config_save_all_forced(handle, fan, source, curve, schedule);
    if (e != ESP_OK) return e;

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  DS18B20 GPIO persistence (simple text file)                       */
/* ------------------------------------------------------------------ */

esp_err_t f_config_save_ds18b20_gpio(f_config_handle_t handle, uint8_t gpio)
{
    if (handle == NULL || !handle->mounted) return ESP_ERR_INVALID_STATE;
    FILE *f = fopen("/littlefs/ds18b20_gpio", "w");
    if (f == NULL) return ESP_FAIL;
    fprintf(f, "%d", gpio);
    fclose(f);
    ESP_LOGI(TAG, "DS18B20 GPIO saved: %d", gpio);
    return ESP_OK;
}

esp_err_t f_config_load_ds18b20_gpio(f_config_handle_t handle, uint8_t *gpio_out)
{
    if (handle == NULL || !handle->mounted || gpio_out == NULL) return ESP_ERR_INVALID_STATE;
    FILE *f = fopen("/littlefs/ds18b20_gpio", "r");
    if (f == NULL) return ESP_ERR_NOT_FOUND;
    int gpio;
    if (fscanf(f, "%d", &gpio) != 1) {
        fclose(f);
        return ESP_ERR_INVALID_ARG;
    }
    fclose(f);
    *gpio_out = (uint8_t)gpio;
    return ESP_OK;
}
