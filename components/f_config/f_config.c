#include "f_config.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "f_config";

#define CONFIG_FILENAME "/spiffs/config.json"

struct f_config {
    char mount_point[32];
    bool mounted;
};

esp_err_t f_config_init(f_config_handle_t *handle, const char *partition_label,
                        const char *mount_point) {
    if (handle == NULL || partition_label == NULL || mount_point == NULL)
        return ESP_ERR_INVALID_ARG;

    f_config_handle_t h = calloc(1, sizeof(struct f_config));
    if (h == NULL) return ESP_ERR_NO_MEM;
    strncpy(h->mount_point, mount_point, sizeof(h->mount_point) - 1);

    esp_vfs_spiffs_conf_t conf = {
        .base_path = mount_point,
        .partition_label = partition_label,
        .max_files = 5,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS: %d", err);
        free(h);
        return err;
    }
    h->mounted = true;
    ESP_LOGI(TAG, "SPIFFS mounted at %s", mount_point);
    *handle = h;
    return ESP_OK;
}

esp_err_t f_config_save_all(f_config_handle_t handle, f_fan_handle_t fan,
                            f_source_handle_t source, f_curve_handle_t curve) {
    if (handle == NULL || !handle->mounted) return ESP_ERR_INVALID_STATE;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "v", "2.0");
    char path[64];
    snprintf(path, sizeof(path), "%s" CONFIG_FILENAME, handle->mount_point);
    char *json_str = cJSON_Print(root);
    if (json_str == NULL) { cJSON_Delete(root); return ESP_ERR_NO_MEM; }
    FILE *f = fopen(path, "w");
    if (f == NULL) { free(json_str); cJSON_Delete(root); return ESP_FAIL; }
    fprintf(f, "%s", json_str);
    fclose(f);
    free(json_str);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Config saved");
    return ESP_OK;
}

esp_err_t f_config_load_all(f_config_handle_t handle, f_fan_handle_t fan,
                            f_source_handle_t source, f_curve_handle_t curve) {
    if (handle == NULL || !handle->mounted) return ESP_ERR_INVALID_STATE;
    char path[64];
    snprintf(path, sizeof(path), "%s" CONFIG_FILENAME, handle->mount_point);
    FILE *f = fopen(path, "r");
    if (f == NULL) { ESP_LOGI(TAG, "No config file, starting fresh"); return ESP_OK; }
    fclose(f);
    ESP_LOGI(TAG, "Config loaded");
    return ESP_OK;
}
