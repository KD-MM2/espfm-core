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

static const char *TAG = "f_config";

#define CONFIG_FILENAME    "/littlefs/config.pb"
#define SAVE_DEBOUNCE_US   3000000  /* 3 seconds */
#define CONFIG_ENC_BUF_SIZE 8192
#define CONFIG_FILE_MAX     8192

struct f_config {
    char partition_label[32];
    bool mounted;
    uint64_t last_save_us;
};

esp_err_t f_config_init(f_config_handle_t *handle, const char *partition_label,
                        const char *mount_point) {
    if (handle == NULL || partition_label == NULL || mount_point == NULL)
        return ESP_ERR_INVALID_ARG;

    f_config_handle_t h = calloc(1, sizeof(struct f_config));
    if (h == NULL) return ESP_ERR_NO_MEM;
    strncpy(h->partition_label, partition_label, sizeof(h->partition_label) - 1);

    esp_vfs_littlefs_conf_t conf = {
        .base_path = mount_point,
        .partition_label = partition_label,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount LittleFS: %s", esp_err_to_name(err));
        free(h);
        return err;
    }
    h->mounted = true;

    size_t total = 0, used = 0;
    esp_littlefs_info(partition_label, &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted at %s (total=%d, used=%d)", mount_point, total, used);

    *handle = h;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Save                                                               */
/* ------------------------------------------------------------------ */

esp_err_t f_config_save_all(f_config_handle_t handle, f_fan_handle_t fan,
                            f_source_handle_t source, f_curve_handle_t curve,
                            f_schedule_handle_t schedule) {
    if (handle == NULL || !handle->mounted) return ESP_ERR_INVALID_STATE;

    /* Debounce */
    uint64_t now = esp_timer_get_time();
    if (now - handle->last_save_us < SAVE_DEBOUNCE_US) {
        ESP_LOGD(TAG, "Save debounced");
        return ESP_OK;
    }
    handle->last_save_us = now;

    /* Build ConfigFile on heap (large struct, ~3757 bytes) */
    ConfigFile *cfg = calloc(1, sizeof(ConfigFile));
    if (cfg == NULL) return ESP_ERR_NO_MEM;
    *cfg = (ConfigFile)ConfigFile_init_default;
    strncpy(cfg->version, "3.0", sizeof(cfg->version) - 1);

    /* Fans */
    cfg->has_fans = true;
    cfg->fans = (FanList)FanList_init_default;
    if (fan) {
        for (uint8_t i = 0; i < F_FAN_MAX_COUNT; i++) {
            f_fan_info_t fi;
            if (f_fan_get_info(fan, i, &fi) == ESP_OK)
                espfm_fan_to_pb(&fi, &cfg->fans.fans[cfg->fans.fans_count++]);
        }
    }

    /* Sources */
    cfg->has_sources = true;
    cfg->sources = (SourceList)SourceList_init_default;
    if (source) {
        for (uint8_t i = 0; i < F_SOURCE_MAX_COUNT; i++) {
            f_source_info_t si;
            if (f_source_get_info(source, i, &si) == ESP_OK)
                espfm_source_to_pb(&si, &cfg->sources.sources[cfg->sources.sources_count++]);
        }
    }

    /* Curves */
    cfg->has_curves = true;
    cfg->curves = (CurveList)CurveList_init_default;
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
        cfg->schedules = (ScheduleList)ScheduleList_init_default;
        for (uint8_t i = 0; i < F_SCHEDULE_MAX_COUNT; i++) {
            f_schedule_info_t si;
            if (f_schedule_get_info(schedule, i, &si) == ESP_OK)
                espfm_schedule_to_pb(&si,
                    &cfg->schedules.schedules[cfg->schedules.schedules_count++]);
        }
    }

    /* Encode to buffer */
    static uint8_t enc_buf[CONFIG_ENC_BUF_SIZE];
    pb_ostream_t os = pb_ostream_from_buffer(enc_buf, sizeof(enc_buf));
    if (!pb_encode(&os, &ConfigFile_msg, cfg)) {
        ESP_LOGE(TAG, "Protobuf encode failed: %s", os.errmsg);
        free(cfg);
        return ESP_FAIL;
    }

    size_t enc_len = os.bytes_written;
    free(cfg);

    /* Write to LittleFS */
    FILE *f = fopen(CONFIG_FILENAME, "w");
    if (f == NULL) return ESP_FAIL;
    size_t written = fwrite(enc_buf, 1, enc_len, f);
    fclose(f);

    if (written != enc_len) {
        ESP_LOGE(TAG, "Short write (%zu/%zu)", written, enc_len);
        return ESP_FAIL;
    }

    size_t fan_n   = 0, src_n = 0, cur_n = 0, sched_n = 0;
    if (fan)      fan_n   = f_fan_get_count(fan);
    if (source)   src_n   = f_source_get_count(source);
    if (curve)    cur_n   = f_curve_get_count(curve);
    if (schedule) sched_n = f_schedule_get_count(schedule);

    ESP_LOGI(TAG, "Config saved as protobuf (%zu bytes, %zu fans, %zu sources, "
             "%zu curves, %zu schedules)", enc_len, fan_n, src_n, cur_n, sched_n);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Load                                                               */
/* ------------------------------------------------------------------ */

esp_err_t f_config_load_all(f_config_handle_t handle, f_fan_handle_t fan,
                            f_source_handle_t source, f_curve_handle_t curve,
                            f_schedule_handle_t schedule) {
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
    if (buf == NULL) { fclose(f); return ESP_ERR_NO_MEM; }

    size_t bytes_read = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    if (bytes_read != (size_t)fsize) {
        ESP_LOGE(TAG, "Short read (%zu/%ld)", bytes_read, fsize);
        free(buf);
        return ESP_FAIL;
    }

    /* Decode protobuf */
    ConfigFile *cfg = calloc(1, sizeof(ConfigFile));
    if (cfg == NULL) { free(buf); return ESP_ERR_NO_MEM; }
    *cfg = (ConfigFile)ConfigFile_init_default;

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
            FanInfo *pb = &cfg->fans.fans[i];
            if (f_constraints_gpio((int)pb->pwm_gpio, NULL) != ESP_OK) continue;

            uint8_t new_id;
            if (f_fan_add(fan, (uint8_t)pb->pwm_gpio, (uint8_t)pb->tach_gpio,
                          pb->name, &new_id) != ESP_OK)
                continue;

            if (f_constraints_mode((int)pb->mode, NULL) == ESP_OK)
                f_fan_set_mode(fan, new_id, (fan_mode_t)pb->mode);

            if (f_constraints_duty((int)pb->duty, NULL) == ESP_OK)
                f_fan_set_duty(fan, new_id, (uint8_t)pb->duty);

            f_fan_set_group(fan, new_id, (uint8_t)pb->group_id);
            f_fan_set_inverted(fan, new_id, pb->inverted);

            if (pb->source_id   != 0xFF) f_fan_set_source(fan, new_id,
                                               (uint8_t)pb->source_id);
            if (pb->curve_id    != 0xFF) f_fan_set_curve(fan, new_id,
                                               (uint8_t)pb->curve_id);
            if (pb->schedule_id != 0xFF) f_fan_set_schedule(fan, new_id,
                                               (uint8_t)pb->schedule_id);

            fan_count++;
        }
    }

    /* -- Sources -- */
    if (cfg->has_sources) {
        for (pb_size_t i = 0; i < cfg->sources.sources_count; i++) {
            SourceInfo *pb = &cfg->sources.sources[i];
            /* Manual sources use gpio=255 (GPIO_NONE), skip GPIO validation */
            if (pb_to_source_type(pb->type) != SOURCE_TYPE_MANUAL &&
                f_constraints_gpio((int)pb->gpio, NULL) != ESP_OK) continue;
            uint8_t new_id;
            if (f_source_add(source, pb_to_source_type(pb->type),
                             (uint8_t)pb->gpio, pb->name, &new_id) == ESP_OK) {
                src_count++;
                /* Restore manual temperature from saved config */
                if (pb_to_source_type(pb->type) == SOURCE_TYPE_MANUAL && pb->temp_c != 0.0f)
                    f_source_update_manual(source, new_id, pb->temp_c);
            }
        }
    }

    /* -- Curves -- */
    if (cfg->has_curves) {
        for (pb_size_t i = 0; i < cfg->curves.curves_count; i++) {
            CurveInfo *pb = &cfg->curves.curves[i];
            f_curve_info_t ci;
            memset(&ci, 0, sizeof(ci));
            ci.id = (uint8_t)pb->id;
            strncpy(ci.name, pb->name, sizeof(ci.name) - 1);
            ci.name[sizeof(ci.name) - 1] = '\0';
            ci.num_points = pb->points_count;
            for (int j = 0; j < pb->points_count && j < F_CURVE_MAX_POINTS; j++) {
                ci.points[j].temp_c = pb->points[j].temp_c;
                ci.points[j].duty   = (uint8_t)pb->points[j].duty;
            }
            uint8_t out_id;
            if (f_curve_upsert(curve, &ci, &out_id) == ESP_OK)
                cur_count++;
        }
    }

    /* -- Schedules -- */
    if (cfg->has_schedules && schedule != NULL) {
        for (pb_size_t i = 0; i < cfg->schedules.schedules_count; i++) {
            ScheduleInfo *pb = &cfg->schedules.schedules[i];
            f_schedule_info_t si = {
                .fan_id    = (uint8_t)pb->fan_id,
                .duty      = (uint8_t)pb->duty,
                .start_min = (uint16_t)pb->start_min,
                .end_min   = (uint16_t)pb->end_min,
                .enabled   = pb->enabled
            };
            uint8_t out_id;
            if (f_schedule_add(schedule, &si, &out_id) == ESP_OK)
                sched_count++;
        }
    }

    free(cfg);

    ESP_LOGI(TAG, "Config loaded (%zu fans, %zu sources, %zu curves, %zu schedules)",
             fan_count, src_count, cur_count, sched_count);
    return ESP_OK;
}
