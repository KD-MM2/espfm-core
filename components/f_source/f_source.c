#include "f_source.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "f_source";

/* Default NTC params for a 10k NTC with 10k divider at 3.3V */
#define NTC_BETA             3950.0f
#define NTC_R0               10000.0f
#define NTC_T0_K             298.15f
#define NTC_R_DIV            10000.0f
#define NTC_VCC              3.3f
#define NTC_VREF_MV          3300

#define STALE_THRESHOLD_US   5000000LL  /* 5 seconds */
#define INVALID_THRESHOLD_US 30000000LL /* 30 seconds */

struct f_source {
    f_adc_handle_t adc;
    f_ds18b20_handle_t ds18b20;
    f_source_info_t sources[F_SOURCE_MAX_COUNT];
    bool slot_used[F_SOURCE_MAX_COUNT];
    uint8_t count;
    SemaphoreHandle_t mutex;
};

esp_err_t f_source_init(f_source_handle_t *handle, f_adc_handle_t adc, f_ds18b20_handle_t ds18b20)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    f_source_handle_t h = calloc(1, sizeof(struct f_source));
    if (h == NULL) return ESP_ERR_NO_MEM;
    h->adc     = adc;
    h->ds18b20 = ds18b20;
    h->mutex   = xSemaphoreCreateRecursiveMutex();
    if (h->mutex == NULL) {
        free(h);
        return ESP_ERR_NO_MEM;
    }
    *handle = h;
    ESP_LOGI(TAG, "Source registry initialized (max %d)", F_SOURCE_MAX_COUNT);
    return ESP_OK;
}

esp_err_t f_source_add(f_source_handle_t handle, source_type_t type, uint8_t gpio, const char *name,
                       uint8_t *id_out)
{
    if (handle == NULL || name == NULL || id_out == NULL) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);

    int slot = -1;
    for (int i = 0; i < F_SOURCE_MAX_COUNT; i++) {
        if (!handle->slot_used[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    f_source_info_t *s = &handle->sources[slot];
    s->id              = (uint8_t)slot;
    strncpy(s->name, name, ESPFM_NAME_MAX - 1);
    s->name[ESPFM_NAME_MAX - 1] = '\0';
    s->type                     = type;
    s->status                   = SOURCE_STATUS_INVALID;
    s->temp_c                   = 0.0f;
    s->gpio                     = gpio;
    s->ds18b20_rom_code         = 0;
    s->last_update_us           = 0;

    /* For manual-type sources, mark as valid immediately */
    if (type == SOURCE_TYPE_MANUAL) {
        s->status = SOURCE_STATUS_VALID;
    }

    handle->slot_used[slot] = true;
    handle->count++;
    *id_out = (uint8_t)slot;
    ESP_LOGI(TAG, "Source %d added: '%s' type=%d GPIO=%d", slot, s->name, type, gpio);

cleanup:
    xSemaphoreGiveRecursive(handle->mutex);
    return ret;
}

esp_err_t f_source_add_ds18b20(f_source_handle_t handle, uint64_t rom_code, const char *name,
                               uint8_t *id_out)
{
    if (handle == NULL || name == NULL || id_out == NULL) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);

    int slot = -1;
    for (int i = 0; i < F_SOURCE_MAX_COUNT; i++) {
        if (!handle->slot_used[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    f_source_info_t *s = &handle->sources[slot];
    s->id              = (uint8_t)slot;
    strncpy(s->name, name, ESPFM_NAME_MAX - 1);
    s->name[ESPFM_NAME_MAX - 1] = '\0';
    s->type                     = SOURCE_TYPE_DS18B20;
    s->status                   = SOURCE_STATUS_INVALID;
    s->temp_c                   = 0.0f;
    s->gpio                     = F_SOURCE_GPIO_NONE;
    s->ds18b20_rom_code         = rom_code;
    s->last_update_us           = 0;

    handle->slot_used[slot]     = true;
    handle->count++;
    *id_out = (uint8_t)slot;
    ESP_LOGI(TAG, "Source %d added: '%s' type=DS18B20 ROM=0x%016llX", slot, s->name,
             (unsigned long long)rom_code);

cleanup:
    xSemaphoreGiveRecursive(handle->mutex);
    return ret;
}

esp_err_t f_source_trigger_ds18b20(f_source_handle_t handle)
{
    if (handle == NULL || handle->ds18b20 == NULL) return ESP_ERR_INVALID_STATE;
    return f_ds18b20_trigger_all(handle->ds18b20);
}

esp_err_t f_source_remove(f_source_handle_t handle, uint8_t id)
{
    if (handle == NULL || id >= F_SOURCE_MAX_COUNT) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);

    if (!handle->slot_used[id]) {
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    memset(&handle->sources[id], 0, sizeof(f_source_info_t));
    handle->slot_used[id] = false;
    handle->count--;

cleanup:
    xSemaphoreGiveRecursive(handle->mutex);
    return ret;
}

esp_err_t f_source_get_reading(f_source_handle_t handle, uint8_t id, float *temp_c_out,
                               source_status_t *status_out)
{
    if (handle == NULL || id >= F_SOURCE_MAX_COUNT) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);

    if (!handle->slot_used[id]) {
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    f_source_info_t *s = &handle->sources[id];

    /* Read hardware sources */
    if (s->type == SOURCE_TYPE_NTC && handle->adc) {
        int raw;
        esp_err_t err = f_adc_read_raw(handle->adc, s->gpio, &raw);
        if (err != ESP_OK) {
            s->status = SOURCE_STATUS_INVALID;
            if (temp_c_out) *temp_c_out = s->temp_c;
            if (status_out) *status_out = s->status;
            ret = err;
            goto cleanup;
        }
        float voltage;
        f_adc_raw_to_voltage(raw, NTC_VREF_MV, &voltage);
        f_adc_ntc_temp(voltage, NTC_VCC, NTC_R_DIV, NTC_BETA, NTC_R0, NTC_T0_K, &s->temp_c);
        s->last_update_us = esp_timer_get_time();
        s->status         = SOURCE_STATUS_VALID;
    } else if (s->type == SOURCE_TYPE_DS18B20 && handle->ds18b20) {
        uint8_t idx;
        if (f_ds18b20_find_by_rom(handle->ds18b20, s->ds18b20_rom_code, &idx) != ESP_OK) {
            s->status = SOURCE_STATUS_INVALID;
        } else {
            esp_err_t err = f_ds18b20_read_temp(handle->ds18b20, idx, &s->temp_c);
            if (err != ESP_OK) {
                s->status = SOURCE_STATUS_INVALID;
            } else {
                s->last_update_us = esp_timer_get_time();
                s->status         = SOURCE_STATUS_VALID;
            }
        }
    }

    /* Check staleness */
    if (s->status == SOURCE_STATUS_VALID) {
        int64_t age = esp_timer_get_time() - s->last_update_us;
        if (age > INVALID_THRESHOLD_US)
            s->status = SOURCE_STATUS_INVALID;
        else if (age > STALE_THRESHOLD_US)
            s->status = SOURCE_STATUS_STALE;
    }

    if (temp_c_out) *temp_c_out = s->temp_c;
    if (status_out) *status_out = s->status;

cleanup:
    xSemaphoreGiveRecursive(handle->mutex);
    return ret;
}

esp_err_t f_source_update_manual(f_source_handle_t handle, uint8_t id, float temp_c)
{
    if (handle == NULL || id >= F_SOURCE_MAX_COUNT) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);

    if (!handle->slot_used[id]) {
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    if (handle->sources[id].type != SOURCE_TYPE_MANUAL) {
        ret = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    handle->sources[id].temp_c         = temp_c;
    handle->sources[id].last_update_us = esp_timer_get_time();
    handle->sources[id].status         = SOURCE_STATUS_VALID;

cleanup:
    xSemaphoreGiveRecursive(handle->mutex);
    return ret;
}

uint8_t f_source_get_count(f_source_handle_t handle)
{
    if (handle == NULL) return 0;
    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);
    uint8_t count = handle->count;
    xSemaphoreGiveRecursive(handle->mutex);
    return count;
}

esp_err_t f_source_get_info(f_source_handle_t handle, uint8_t id, f_source_info_t *info_out)
{
    if (handle == NULL || id >= F_SOURCE_MAX_COUNT || info_out == NULL) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);

    if (!handle->slot_used[id]) {
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    memcpy(info_out, &handle->sources[id], sizeof(f_source_info_t));

cleanup:
    xSemaphoreGiveRecursive(handle->mutex);
    return ret;
}

esp_err_t f_source_for_each(f_source_handle_t handle, void (*cb)(const f_source_info_t *, void *),
                            void *ctx)
{
    if (handle == NULL || cb == NULL) return ESP_ERR_INVALID_ARG;

    xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY);
    for (int i = 0; i < F_SOURCE_MAX_COUNT; i++) {
        if (handle->slot_used[i]) cb(&handle->sources[i], ctx);
    }
    xSemaphoreGiveRecursive(handle->mutex);
    return ESP_OK;
}
