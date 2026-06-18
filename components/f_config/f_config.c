#include "f_config.h"
#include "f_constraints.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "f_config";

#define CONFIG_FILENAME "/littlefs/config.json"
#define SAVE_DEBOUNCE_US 3000000  /* 3 seconds */

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

esp_err_t f_config_save_all(f_config_handle_t handle, f_fan_handle_t fan,
                            f_source_handle_t source, f_curve_handle_t curve,
                            f_schedule_handle_t schedule) {
    if (handle == NULL || !handle->mounted) return ESP_ERR_INVALID_STATE;

    /* Debounce: skip if last save was less than 3 seconds ago */
    uint64_t now = esp_timer_get_time();
    if (now - handle->last_save_us < SAVE_DEBOUNCE_US) {
        ESP_LOGD(TAG, "Save debounced (last=%llu, now=%llu)",
                 (unsigned long long)handle->last_save_us,
                 (unsigned long long)now);
        return ESP_OK;
    }
    handle->last_save_us = now;

    cJSON *root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "v", "2.0");

    /* -- Fans -- */
    cJSON *fans = cJSON_AddArrayToObject(root, "fans");
    for (uint8_t i = 0; i < F_FAN_MAX_COUNT; i++) {
        f_fan_info_t info;
        if (f_fan_get_info(fan, i, &info) != ESP_OK) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id",   info.id);
        cJSON_AddStringToObject(o, "n",    info.name);
        cJSON_AddNumberToObject(o, "m",    info.mode);
        cJSON_AddNumberToObject(o, "d",    info.duty);
        cJSON_AddBoolToObject(o,   "e",    info.enabled);
        cJSON_AddBoolToObject(o,   "inv",  info.inverted);
        cJSON_AddNumberToObject(o, "pg",   info.pwm_gpio);
        cJSON_AddNumberToObject(o, "tg",   info.tach_gpio);
        cJSON_AddNumberToObject(o, "sid",  info.source_id);
        cJSON_AddNumberToObject(o, "cid",  info.curve_id);
        cJSON_AddNumberToObject(o, "scid", info.schedule_id);
        cJSON_AddNumberToObject(o, "gid",  info.group_id);
        cJSON_AddItemToArray(fans, o);
    }

    /* -- Sources -- */
    cJSON *sources = cJSON_AddArrayToObject(root, "sources");
    for (uint8_t i = 0; i < F_SOURCE_MAX_COUNT; i++) {
        f_source_info_t info;
        if (f_source_get_info(source, i, &info) != ESP_OK) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", info.id);
        cJSON_AddStringToObject(o, "n",  info.name);
        cJSON_AddNumberToObject(o, "t",  info.type);
        cJSON_AddNumberToObject(o, "gp", info.gpio);
        cJSON_AddItemToArray(sources, o);
    }

    /* -- Curves -- */
    cJSON *curves = cJSON_AddArrayToObject(root, "curves");
    for (uint8_t i = 0; i < F_CURVE_MAX_COUNT; i++) {
        f_curve_info_t info;
        if (f_curve_get_info(curve, i, &info) != ESP_OK) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id",  info.id);
        cJSON_AddStringToObject(o, "n",   info.name);
        cJSON_AddNumberToObject(o, "np",  info.num_points);
        cJSON *pts = cJSON_AddArrayToObject(o, "pts");
        for (int j = 0; j < info.num_points; j++) {
            cJSON *p = cJSON_CreateObject();
            cJSON_AddNumberToObject(p, "tc",  info.points[j].temp_c);
            cJSON_AddNumberToObject(p, "dty", info.points[j].duty);
            cJSON_AddItemToArray(pts, p);
        }
        cJSON_AddItemToArray(curves, o);
    }

    /* -- Schedules -- */
    cJSON *scheds = cJSON_AddArrayToObject(root, "schedules");
    if (schedule != NULL) {
        for (uint8_t i = 0; i < F_SCHEDULE_MAX_COUNT; i++) {
            f_schedule_info_t info;
            if (f_schedule_get_info(schedule, i, &info) != ESP_OK) continue;
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "id",  info.id);
            cJSON_AddNumberToObject(o, "fid", info.fan_id);
            cJSON_AddNumberToObject(o, "d",   info.duty);
            cJSON_AddNumberToObject(o, "sm",  info.start_min);
            cJSON_AddNumberToObject(o, "em",  info.end_min);
            cJSON_AddBoolToObject(o,   "e",   info.enabled);
            cJSON_AddItemToArray(scheds, o);
        }
    }

    char *json_str = cJSON_Print(root);
    if (json_str == NULL) { cJSON_Delete(root); return ESP_ERR_NO_MEM; }

    FILE *f = fopen(CONFIG_FILENAME, "w");
    if (f == NULL) { free(json_str); cJSON_Delete(root); return ESP_FAIL; }

    size_t fan_count = cJSON_GetArraySize(fans);
    size_t src_count = cJSON_GetArraySize(sources);
    size_t cur_count = cJSON_GetArraySize(curves);
    size_t sched_count = schedule ? cJSON_GetArraySize(scheds) : 0;

    fprintf(f, "%s", json_str);
    fclose(f);
    free(json_str);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Config saved (fans=%zu, sources=%zu, curves=%zu, schedules=%zu)",
             fan_count, src_count, cur_count, sched_count);
    return ESP_OK;
}

static inline int cjson_get_int(cJSON *obj, const char *key, int def)
{
    cJSON *v = cJSON_GetObjectItem(obj, key);
    return (v && cJSON_IsNumber(v)) ? v->valueint : def;
}

static inline bool cjson_get_bool(cJSON *obj, const char *key, bool def)
{
    cJSON *v = cJSON_GetObjectItem(obj, key);
    return (v && cJSON_IsBool(v)) ? cJSON_IsTrue(v) : def;
}

esp_err_t f_config_load_all(f_config_handle_t handle, f_fan_handle_t fan,
                            f_source_handle_t source, f_curve_handle_t curve,
                            f_schedule_handle_t schedule) {
    if (handle == NULL || !handle->mounted) return ESP_ERR_INVALID_STATE;

    FILE *f = fopen(CONFIG_FILENAME, "r");
    if (f == NULL) {
        ESP_LOGI(TAG, "No config file found, starting fresh");
        return ESP_OK;
    }

    /* Check file size */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 8192) {
        ESP_LOGW(TAG, "Config file size %ld out of range, skipping", fsize);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = malloc((size_t)fsize + 1);
    if (buf == NULL) { fclose(f); return ESP_ERR_NO_MEM; }

    size_t bytes_read = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    buf[bytes_read] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse config JSON");
        return ESP_ERR_INVALID_ARG;
    }

    size_t fan_count = 0, src_count = 0, cur_count = 0, sched_count = 0;

    /* -- Fans -- */
    cJSON *fans = cJSON_GetObjectItem(root, "fans");
    if (cJSON_IsArray(fans)) {
        cJSON *item;
        cJSON_ArrayForEach(item, fans) {
            int pwm_gpio   = cjson_get_int(item, "pg", -1);
            int tach_gpio  = cjson_get_int(item, "tg", F_FAN_TACH_NONE);
            const char *name = cJSON_GetObjectItem(item, "n")
                               ? cJSON_GetObjectItem(item, "n")->valuestring : NULL;
            if (pwm_gpio < 0 || name == NULL) continue;

            uint8_t new_id;
            if (f_fan_add(fan, (uint8_t)pwm_gpio, (uint8_t)tach_gpio,
                          name, &new_id) != ESP_OK) continue;

            int mode = cjson_get_int(item, "m", FAN_MODE_MANUAL);
            if (f_constraints_mode(mode, NULL) == ESP_OK)
                f_fan_set_mode(fan, new_id, (fan_mode_t)mode);
            else
                ESP_LOGW(TAG, "Fan %d: invalid mode %d, using default", new_id, mode);

            int duty = cjson_get_int(item, "d", 0);
            if (f_constraints_duty(duty, NULL) == ESP_OK)
                f_fan_set_duty(fan, new_id, (uint8_t)duty);
            else
                ESP_LOGW(TAG, "Fan %d: invalid duty %d, using default", new_id, duty);

            int gid = cjson_get_int(item, "gid", 0);
            f_fan_set_group(fan, new_id, (uint8_t)gid);

            bool inv = cjson_get_bool(item, "inv", false);
            f_fan_set_inverted(fan, new_id, inv);

            int sid  = cjson_get_int(item, "sid", 0xFF);
            int cid  = cjson_get_int(item, "cid", 0xFF);
            int scid = cjson_get_int(item, "scid", 0xFF);
            if (sid  != 0xFF) f_fan_set_source(fan, new_id, (uint8_t)sid);
            if (cid  != 0xFF) f_fan_set_curve(fan, new_id, (uint8_t)cid);
            if (scid != 0xFF) f_fan_set_schedule(fan, new_id, (uint8_t)scid);

            fan_count++;
        }
    }

    /* -- Sources -- */
    cJSON *sources = cJSON_GetObjectItem(root, "sources");
    if (cJSON_IsArray(sources)) {
        cJSON *item;
        cJSON_ArrayForEach(item, sources) {
            int type = cjson_get_int(item, "t", -1);
            int gpio = cjson_get_int(item, "gp", F_SOURCE_GPIO_NONE);
            const char *name = cJSON_GetObjectItem(item, "n")
                               ? cJSON_GetObjectItem(item, "n")->valuestring : NULL;
            if (type < 0 || name == NULL) continue;

            uint8_t new_id;
            if (f_source_add(source, (source_type_t)type, (uint8_t)gpio,
                             name, &new_id) == ESP_OK) {
                src_count++;
            }
        }
    }

    /* -- Curves -- */
    cJSON *curves = cJSON_GetObjectItem(root, "curves");
    if (cJSON_IsArray(curves)) {
        cJSON *item;
        cJSON_ArrayForEach(item, curves) {
            const char *name = cJSON_GetObjectItem(item, "n")
                               ? cJSON_GetObjectItem(item, "n")->valuestring : NULL;
            if (name == NULL) continue;

            f_curve_info_t ci;
            memset(&ci, 0, sizeof(ci));
            strncpy(ci.name, name, ESPFM_NAME_MAX - 1);
            ci.num_points = (uint8_t)cjson_get_int(item, "np", 0);

            cJSON *pts = cJSON_GetObjectItem(item, "pts");
            if (cJSON_IsArray(pts)) {
                int pcount = cJSON_GetArraySize(pts);
                if (pcount > F_CURVE_MAX_POINTS) pcount = F_CURVE_MAX_POINTS;
                ci.num_points = (uint8_t)pcount;
                for (int j = 0; j < pcount; j++) {
                    cJSON *p = cJSON_GetArrayItem(pts, j);
                    cJSON *tc = cJSON_GetObjectItem(p, "tc");
                    ci.points[j].temp_c = (tc && cJSON_IsNumber(tc)) ? (float)tc->valuedouble : 0.0f;
                    ci.points[j].duty   = (uint8_t)cjson_get_int(p, "dty", 0);
                }
            }

            uint8_t out_id;
            if (f_curve_upsert(curve, &ci, &out_id) == ESP_OK) {
                cur_count++;
            }
        }
    }

    /* -- Schedules -- */
    cJSON *scheds = cJSON_GetObjectItem(root, "schedules");
    if (cJSON_IsArray(scheds) && schedule != NULL) {
        cJSON *item;
        cJSON_ArrayForEach(item, scheds) {
            f_schedule_info_t si;
            memset(&si, 0, sizeof(si));
            si.fan_id    = (uint8_t)cjson_get_int(item, "fid", 0);
            si.duty      = (uint8_t)cjson_get_int(item, "d", 0);
            si.start_min = (uint16_t)cjson_get_int(item, "sm", 0);
            si.end_min   = (uint16_t)cjson_get_int(item, "em", 0);
            si.enabled   = cjson_get_bool(item, "e", true);

            uint8_t out_id;
            if (f_schedule_add(schedule, &si, &out_id) == ESP_OK) {
                sched_count++;
            }
        }
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Config loaded (%zu fans, %zu sources, %zu curves, %zu schedules)",
             fan_count, src_count, cur_count, sched_count);
    return ESP_OK;
}
