#include "f_fan.h"
#include "f_constraints.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "f_fan";

struct f_fan {
    f_ledc_handle_t ledc;
    f_pcnt_handle_t pcnt;
    f_fan_info_t channels[F_FAN_MAX_COUNT];
    bool slot_used[F_FAN_MAX_COUNT];
    uint8_t ledc_channel_id[F_FAN_MAX_COUNT];
    uint8_t pcnt_unit_id[F_FAN_MAX_COUNT];
    uint8_t count;
    SemaphoreHandle_t mutex;
};

esp_err_t f_fan_init(f_fan_handle_t *handle, f_ledc_handle_t ledc,
                     f_pcnt_handle_t pcnt) {
    if (handle == NULL || ledc == NULL) return ESP_ERR_INVALID_ARG;
    f_fan_handle_t h = calloc(1, sizeof(struct f_fan));
    if (h == NULL) return ESP_ERR_NO_MEM;
    h->ledc = ledc;
    h->pcnt = pcnt;
    h->mutex = xSemaphoreCreateRecursiveMutex();
    if (h->mutex == NULL) {
        free(h);
        return ESP_ERR_NO_MEM;
    }
    *handle = h;
    ESP_LOGI(TAG, "Fan registry initialized (max %d)", F_FAN_MAX_COUNT);
    return ESP_OK;
}

esp_err_t f_fan_add(f_fan_handle_t handle, uint8_t pwm_gpio, uint8_t tach_gpio,
                    const char *name, uint8_t *id_out) {
    if (handle == NULL || name == NULL || id_out == NULL) return ESP_ERR_INVALID_ARG;
    if (f_constraints_gpio((int)pwm_gpio, NULL) != ESP_OK) return ESP_ERR_INVALID_ARG;
    if (tach_gpio != F_FAN_TACH_NONE &&
        f_constraints_gpio((int)tach_gpio, NULL) != ESP_OK)
        return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);

    int slot = -1;
    for (int i = 0; i < F_FAN_MAX_COUNT; i++) {
        if (!handle->slot_used[i]) { slot = i; break; }
    }
    if (slot < 0) { ret = ESP_ERR_NO_MEM; goto cleanup; }

    uint8_t ledc_ch;
    ESP_ERROR_CHECK(f_ledc_add_channel(handle->ledc, pwm_gpio, &ledc_ch));

    uint8_t pcnt_unit = 0xFF;
    if (tach_gpio != F_FAN_TACH_NONE && handle->pcnt != NULL) {
        ESP_ERROR_CHECK(f_pcnt_add_input(handle->pcnt, tach_gpio, &pcnt_unit));
    }

    f_fan_info_t *ch = &handle->channels[slot];
    ch->id = (uint8_t)slot;
    strncpy(ch->name, name, ESPFM_NAME_MAX - 1);
    ch->name[ESPFM_NAME_MAX - 1] = '\0';
    ch->mode = FAN_MODE_MANUAL;
    ch->duty = 0;
    ch->rpm = 0;
    ch->alarm = FAN_ALARM_NONE;
    ch->enabled = true;
    ch->inverted = false;
    ch->pwm_gpio = pwm_gpio;
    ch->tach_gpio = tach_gpio;
    ch->source_id = 0xFF;
    ch->curve_id = 0xFF;
    ch->schedule_id = 0xFF;
    ch->group_id = 0;

    handle->slot_used[slot] = true;
    handle->ledc_channel_id[slot] = ledc_ch;
    handle->pcnt_unit_id[slot] = pcnt_unit;
    handle->count++;

    *id_out = (uint8_t)slot;
    ESP_LOGI(TAG, "Fan %d added: '%s' PWM_GPIO=%d TACH_GPIO=%d",
             slot, ch->name, pwm_gpio, tach_gpio);

cleanup:
    xSemaphoreGiveRecursive(handle->mutex);
    return ret;
}

esp_err_t f_fan_remove(f_fan_handle_t handle, uint8_t id) {
    if (handle == NULL || id >= F_FAN_MAX_COUNT) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);

    if (!handle->slot_used[id]) { ret = ESP_ERR_NOT_FOUND; goto cleanup; }

    f_ledc_remove_channel(handle->ledc, handle->ledc_channel_id[id]);
    if (handle->pcnt_unit_id[id] != 0xFF && handle->pcnt != NULL) {
        f_pcnt_remove_input(handle->pcnt, handle->pcnt_unit_id[id]);
    }

    memset(&handle->channels[id], 0, sizeof(f_fan_info_t));
    handle->slot_used[id] = false;
    handle->count--;

    ESP_LOGI(TAG, "Fan %d removed", id);

cleanup:
    xSemaphoreGiveRecursive(handle->mutex);
    return ret;
}

esp_err_t f_fan_set_duty(f_fan_handle_t handle, uint8_t id, uint8_t duty) {
    if (handle == NULL || id >= F_FAN_MAX_COUNT) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);

    if (!handle->slot_used[id]) { ret = ESP_ERR_NOT_FOUND; goto cleanup; }
    if (duty > 100) duty = 100;

    f_fan_info_t *ch = &handle->channels[id];
    uint8_t effective = ch->inverted ? (100 - duty) : duty;
    float pct = (float)effective;

    ESP_ERROR_CHECK(f_ledc_set_duty(handle->ledc, handle->ledc_channel_id[id], pct));
    ch->duty = duty;

cleanup:
    xSemaphoreGiveRecursive(handle->mutex);
    return ret;
}

esp_err_t f_fan_set_mode(f_fan_handle_t handle, uint8_t id, fan_mode_t mode) {
    if (handle == NULL || id >= F_FAN_MAX_COUNT) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);

    if (!handle->slot_used[id]) { ret = ESP_ERR_NOT_FOUND; goto cleanup; }
    handle->channels[id].mode = mode;

cleanup:
    xSemaphoreGiveRecursive(handle->mutex);
    return ret;
}

esp_err_t f_fan_set_source(f_fan_handle_t handle, uint8_t id, uint8_t source_id) {
    if (handle == NULL || id >= F_FAN_MAX_COUNT) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);

    if (!handle->slot_used[id]) { ret = ESP_ERR_NOT_FOUND; goto cleanup; }
    handle->channels[id].source_id = source_id;

cleanup:
    xSemaphoreGiveRecursive(handle->mutex);
    return ret;
}

esp_err_t f_fan_set_curve(f_fan_handle_t handle, uint8_t id, uint8_t curve_id) {
    if (handle == NULL || id >= F_FAN_MAX_COUNT) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);

    if (!handle->slot_used[id]) { ret = ESP_ERR_NOT_FOUND; goto cleanup; }
    handle->channels[id].curve_id = curve_id;

cleanup:
    xSemaphoreGiveRecursive(handle->mutex);
    return ret;
}

esp_err_t f_fan_set_schedule(f_fan_handle_t handle, uint8_t id, uint8_t schedule_id) {
    if (handle == NULL || id >= F_FAN_MAX_COUNT) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);

    if (!handle->slot_used[id]) { ret = ESP_ERR_NOT_FOUND; goto cleanup; }
    handle->channels[id].schedule_id = schedule_id;

cleanup:
    xSemaphoreGiveRecursive(handle->mutex);
    return ret;
}

esp_err_t f_fan_set_enabled(f_fan_handle_t handle, uint8_t id, bool enabled) {
    if (handle == NULL || id >= F_FAN_MAX_COUNT) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);

    if (!handle->slot_used[id]) { ret = ESP_ERR_NOT_FOUND; goto cleanup; }
    handle->channels[id].enabled = enabled;

cleanup:
    xSemaphoreGiveRecursive(handle->mutex);
    return ret;
}

esp_err_t f_fan_set_inverted(f_fan_handle_t handle, uint8_t id, bool inverted) {
    if (handle == NULL || id >= F_FAN_MAX_COUNT) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);

    if (!handle->slot_used[id]) { ret = ESP_ERR_NOT_FOUND; goto cleanup; }
    handle->channels[id].inverted = inverted;
    /* Re-apply current duty with new inversion */
    uint8_t cur = handle->channels[id].duty;
    uint8_t effective = inverted ? (100 - cur) : cur;
    float pct = (float)effective;
    f_ledc_set_duty(handle->ledc, handle->ledc_channel_id[id], pct);

cleanup:
    xSemaphoreGiveRecursive(handle->mutex);
    return ret;
}

esp_err_t f_fan_set_group(f_fan_handle_t handle, uint8_t id, uint8_t group_id) {
    if (handle == NULL || id >= F_FAN_MAX_COUNT) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);

    if (!handle->slot_used[id]) { ret = ESP_ERR_NOT_FOUND; goto cleanup; }
    handle->channels[id].group_id = group_id;

cleanup:
    xSemaphoreGiveRecursive(handle->mutex);
    return ret;
}

esp_err_t f_fan_update_rpm(f_fan_handle_t handle, uint8_t id) {
    if (handle == NULL || id >= F_FAN_MAX_COUNT) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);

    if (!handle->slot_used[id]) { ret = ESP_ERR_NOT_FOUND; goto cleanup; }

    if (handle->pcnt_unit_id[id] == 0xFF || handle->pcnt == NULL) {
        handle->channels[id].rpm = 0;
        goto cleanup;
    }

    int count;
    ESP_ERROR_CHECK(f_pcnt_read_and_clear(handle->pcnt, handle->pcnt_unit_id[id], &count));
    uint16_t rpm;
    ESP_ERROR_CHECK(f_pcnt_compute_rpm(count, 1000, 2, &rpm));
    handle->channels[id].rpm = rpm;

cleanup:
    xSemaphoreGiveRecursive(handle->mutex);
    return ret;
}

esp_err_t f_fan_get_info(f_fan_handle_t handle, uint8_t id, f_fan_info_t *info_out) {
    if (handle == NULL || id >= F_FAN_MAX_COUNT || info_out == NULL)
        return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);

    if (!handle->slot_used[id]) { ret = ESP_ERR_NOT_FOUND; goto cleanup; }
    memcpy(info_out, &handle->channels[id], sizeof(f_fan_info_t));

cleanup:
    xSemaphoreGiveRecursive(handle->mutex);
    return ret;
}

uint8_t f_fan_get_count(f_fan_handle_t handle) {
    if (handle == NULL) return 0;
    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);
    uint8_t count = handle->count;
    xSemaphoreGiveRecursive(handle->mutex);
    return count;
}

esp_err_t f_fan_for_each(f_fan_handle_t handle,
                          void (*callback)(const f_fan_info_t *, void *), void *ctx) {
    if (handle == NULL || callback == NULL) return ESP_ERR_INVALID_ARG;

    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);
    for (int i = 0; i < F_FAN_MAX_COUNT; i++) {
        if (handle->slot_used[i]) {
            callback(&handle->channels[i], ctx);
        }
    }
    xSemaphoreGiveRecursive(handle->mutex);
    return ESP_OK;
}
