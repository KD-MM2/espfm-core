#include "f_pcnt.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "f_pcnt";

struct f_pcnt {
    pcnt_unit_handle_t units[F_PCNT_MAX_UNITS];
    bool unit_in_use[F_PCNT_MAX_UNITS];
    uint8_t unit_gpio[F_PCNT_MAX_UNITS];
};

esp_err_t f_pcnt_init(f_pcnt_handle_t *handle) {
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    f_pcnt_handle_t h = calloc(1, sizeof(struct f_pcnt));
    if (h == NULL) return ESP_ERR_NO_MEM;
    *handle = h;
    ESP_LOGI(TAG, "PCNT driver initialized (max %d units)", F_PCNT_MAX_UNITS);
    return ESP_OK;
}

esp_err_t f_pcnt_add_input(f_pcnt_handle_t handle, uint8_t gpio, uint8_t *unit_id_out) {
    if (handle == NULL || unit_id_out == NULL) return ESP_ERR_INVALID_ARG;

    int slot = -1;
    for (int i = 0; i < F_PCNT_MAX_UNITS; i++) {
        if (!handle->unit_in_use[i]) { slot = i; break; }
    }
    if (slot < 0) {
        ESP_LOGE(TAG, "No free PCNT units");
        return ESP_ERR_NO_MEM;
    }

    pcnt_unit_config_t unit_cfg = {
        .high_limit = 32767,
        .low_limit = -32768,
    };
    pcnt_unit_handle_t unit = NULL;
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_cfg, &unit));

    pcnt_glitch_filter_config_t filter_cfg = { .max_glitch_ns = 1000 };
    pcnt_unit_set_glitch_filter(unit, &filter_cfg);

    pcnt_chan_config_t chan_cfg = {
        .edge_gpio_num = gpio,
        .level_gpio_num = -1,
    };
    pcnt_channel_handle_t chan = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(unit, &chan_cfg, &chan));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chan,
        PCNT_CHANNEL_EDGE_ACTION_HOLD, PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(chan,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_KEEP));

    ESP_ERROR_CHECK(pcnt_unit_enable(unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(unit));
    ESP_ERROR_CHECK(pcnt_unit_start(unit));

    handle->units[slot] = unit;
    handle->unit_in_use[slot] = true;
    handle->unit_gpio[slot] = gpio;
    *unit_id_out = (uint8_t)slot;

    ESP_LOGI(TAG, "PCNT unit %d added on GPIO %d", slot, gpio);
    return ESP_OK;
}

esp_err_t f_pcnt_read_and_clear(f_pcnt_handle_t handle, uint8_t unit_id, int *count_out) {
    if (handle == NULL || unit_id >= F_PCNT_MAX_UNITS || count_out == NULL)
        return ESP_ERR_INVALID_ARG;
    if (!handle->unit_in_use[unit_id]) return ESP_ERR_INVALID_STATE;

    ESP_ERROR_CHECK(pcnt_unit_get_count(handle->units[unit_id], count_out));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(handle->units[unit_id]));
    return ESP_OK;
}

esp_err_t f_pcnt_compute_rpm(int pulse_count, uint32_t interval_ms,
                              uint16_t pulses_per_rev, uint16_t *rpm_out) {
    if (pulses_per_rev == 0 || rpm_out == NULL) return ESP_ERR_INVALID_ARG;
    if (pulse_count <= 0) { *rpm_out = 0; return ESP_OK; }
    /* RPM = (pulses / pulses_per_rev) * (60000 / interval_ms) */
    *rpm_out = (uint16_t)(((uint32_t)pulse_count * 60000UL) /
                          ((uint32_t)pulses_per_rev * interval_ms));
    return ESP_OK;
}

esp_err_t f_pcnt_remove_input(f_pcnt_handle_t handle, uint8_t unit_id) {
    if (handle == NULL || unit_id >= F_PCNT_MAX_UNITS) return ESP_ERR_INVALID_ARG;
    if (!handle->unit_in_use[unit_id]) return ESP_ERR_INVALID_STATE;

    pcnt_unit_stop(handle->units[unit_id]);
    pcnt_unit_disable(handle->units[unit_id]);
    pcnt_del_unit(handle->units[unit_id]);
    handle->units[unit_id] = NULL;
    handle->unit_in_use[unit_id] = false;
    handle->unit_gpio[unit_id] = 0;

    ESP_LOGI(TAG, "PCNT unit %d removed", unit_id);
    return ESP_OK;
}
