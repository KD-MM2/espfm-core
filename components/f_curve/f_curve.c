#include "f_curve.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "f_curve";

struct f_curve {
    f_curve_info_t curves[F_CURVE_MAX_COUNT];
    bool slot_used[F_CURVE_MAX_COUNT];
    uint8_t count;
    SemaphoreHandle_t mutex;
};

esp_err_t f_curve_init(f_curve_handle_t *handle)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    f_curve_handle_t h = calloc(1, sizeof(struct f_curve));
    if (h == NULL) return ESP_ERR_NO_MEM;
    h->mutex = xSemaphoreCreateRecursiveMutex();
    if (h->mutex == NULL) {
        free(h);
        return ESP_ERR_NO_MEM;
    }
    *handle = h;
    ESP_LOGI(TAG, "Curve registry initialized (max %d)", F_CURVE_MAX_COUNT);
    return ESP_OK;
}

esp_err_t f_curve_upsert(f_curve_handle_t handle, const f_curve_info_t *info, uint8_t *id_out)
{
    if (handle == NULL || info == NULL || id_out == NULL) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);

    if (info->num_points == 0 || info->num_points > F_CURVE_MAX_POINTS) {
        ret = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    /* If id is valid (not 0xFF), update existing; else create new */
    int slot = info->id < F_CURVE_MAX_COUNT && handle->slot_used[info->id] ? info->id : -1;
    if (slot < 0) {
        for (int i = 0; i < F_CURVE_MAX_COUNT; i++) {
            if (!handle->slot_used[i]) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            ret = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        handle->slot_used[slot] = true;
        handle->count++;
    }

    memcpy(&handle->curves[slot], info, sizeof(f_curve_info_t));
    handle->curves[slot].id = (uint8_t)slot;
    *id_out                 = (uint8_t)slot;
    ESP_LOGI(TAG, "Curve %d upserted: '%s' (%d points)", slot, info->name, info->num_points);

cleanup:
    xSemaphoreGiveRecursive(handle->mutex);
    return ret;
}

esp_err_t f_curve_lookup(f_curve_handle_t handle, uint8_t id, float temp_c, uint8_t *duty_out)
{
    if (handle == NULL || id >= F_CURVE_MAX_COUNT || duty_out == NULL) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);

    if (!handle->slot_used[id]) {
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    f_curve_info_t *c = &handle->curves[id];
    if (c->num_points == 0) {
        ret = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    /* Below first point: return first duty */
    if (temp_c <= c->points[0].temp_c) {
        *duty_out = c->points[0].duty;
        goto cleanup;
    }
    /* Above last point: return last duty */
    if (temp_c >= c->points[c->num_points - 1].temp_c) {
        *duty_out = c->points[c->num_points - 1].duty;
        goto cleanup;
    }
    /* Linear interpolation */
    for (int i = 0; i < c->num_points - 1; i++) {
        if (temp_c >= c->points[i].temp_c && temp_c <= c->points[i + 1].temp_c) {
            float t_range = c->points[i + 1].temp_c - c->points[i].temp_c;
            float ratio   = (temp_c - c->points[i].temp_c) / t_range;
            float d       = c->points[i].duty + ratio * (c->points[i + 1].duty - c->points[i].duty);
            *duty_out     = (uint8_t)(d + 0.5f);
            goto cleanup;
        }
    }
    /* Unreachable safety fallback: the interpolation loop above covers all
     * in-range temperatures when points are sorted by temp_c. Kept as a
     * defensive guard against unsorted or corrupt curve data. */
    ret = ESP_ERR_NOT_FOUND;

cleanup:
    xSemaphoreGiveRecursive(handle->mutex);
    return ret;
}

esp_err_t f_curve_remove(f_curve_handle_t handle, uint8_t id)
{
    if (handle == NULL || id >= F_CURVE_MAX_COUNT) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);

    if (!handle->slot_used[id]) {
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    memset(&handle->curves[id], 0, sizeof(f_curve_info_t));
    handle->slot_used[id] = false;
    handle->count--;

cleanup:
    xSemaphoreGiveRecursive(handle->mutex);
    return ret;
}

uint8_t f_curve_get_count(f_curve_handle_t handle)
{
    if (handle == NULL) return 0;
    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);
    uint8_t count = handle->count;
    xSemaphoreGiveRecursive(handle->mutex);
    return count;
}

esp_err_t f_curve_get_info(f_curve_handle_t handle, uint8_t id, f_curve_info_t *info_out)
{
    if (handle == NULL || id >= F_CURVE_MAX_COUNT || info_out == NULL) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);

    if (!handle->slot_used[id]) {
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    memcpy(info_out, &handle->curves[id], sizeof(f_curve_info_t));

cleanup:
    xSemaphoreGiveRecursive(handle->mutex);
    return ret;
}
