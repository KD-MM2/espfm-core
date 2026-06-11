#include "f_ledc.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include <string.h>
#include <math.h>

static const char *TAG = "f_ledc";

struct f_ledc {
    ledc_mode_t speed_mode;
    ledc_timer_t timer_num;
    uint32_t freq_hz;
    ledc_timer_bit_t duty_resolution;
    uint32_t max_duty_raw;
    bool channel_in_use[F_LEDC_MAX_CHANNELS];
    uint8_t channel_gpio[F_LEDC_MAX_CHANNELS];
    float current_duty[F_LEDC_MAX_CHANNELS];
};

static uint32_t duty_percent_to_raw(float percent, uint32_t max_raw) {
    if (percent < 0.0f) percent = 0.0f;
    if (percent > 100.0f) percent = 100.0f;
    return (uint32_t)(percent / 100.0f * (float)max_raw);
}

esp_err_t f_ledc_init(f_ledc_handle_t *handle, uint32_t freq_hz,
                      ledc_timer_bit_t duty_resolution) {
    if (handle == NULL) return ESP_ERR_INVALID_ARG;

    f_ledc_handle_t h = calloc(1, sizeof(struct f_ledc));
    if (h == NULL) return ESP_ERR_NO_MEM;

    h->speed_mode = LEDC_LOW_SPEED_MODE;
    h->timer_num = LEDC_TIMER_0;
    h->freq_hz = freq_hz;
    h->duty_resolution = duty_resolution;
    h->max_duty_raw = (1UL << duty_resolution) - 1;

    ledc_timer_config_t timer_cfg = {
        .speed_mode = h->speed_mode,
        .duty_resolution = h->duty_resolution,
        .timer_num = h->timer_num,
        .freq_hz = h->freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ESP_LOGI(TAG, "PWM init: %lu Hz, %d-bit (%lu steps)",
             freq_hz, (int)duty_resolution, h->max_duty_raw);
    *handle = h;
    return ESP_OK;
}

esp_err_t f_ledc_add_channel(f_ledc_handle_t handle, uint8_t gpio,
                              uint8_t *channel_id_out) {
    if (handle == NULL || channel_id_out == NULL) return ESP_ERR_INVALID_ARG;

    /* Find free channel */
    int ch = -1;
    for (int i = 0; i < F_LEDC_MAX_CHANNELS; i++) {
        if (!handle->channel_in_use[i]) { ch = i; break; }
    }
    if (ch < 0) {
        ESP_LOGE(TAG, "No free LEDC channels");
        return ESP_ERR_NO_MEM;
    }

    ledc_channel_config_t ch_cfg = {
        .speed_mode = handle->speed_mode,
        .channel = ch,
        .timer_sel = handle->timer_num,
        .gpio_num = gpio,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

    handle->channel_in_use[ch] = true;
    handle->channel_gpio[ch] = gpio;
    handle->current_duty[ch] = 0.0f;
    *channel_id_out = (uint8_t)ch;

    ESP_LOGI(TAG, "Channel %d added on GPIO %d", ch, gpio);
    return ESP_OK;
}

esp_err_t f_ledc_set_duty(f_ledc_handle_t handle, uint8_t channel_id,
                           float duty_percent) {
    if (handle == NULL || channel_id >= F_LEDC_MAX_CHANNELS) return ESP_ERR_INVALID_ARG;
    if (!handle->channel_in_use[channel_id]) return ESP_ERR_INVALID_STATE;

    uint32_t raw = duty_percent_to_raw(duty_percent, handle->max_duty_raw);
    ESP_ERROR_CHECK(ledc_set_duty(handle->speed_mode, channel_id, raw));
    ESP_ERROR_CHECK(ledc_update_duty(handle->speed_mode, channel_id));
    handle->current_duty[channel_id] = duty_percent;
    return ESP_OK;
}

esp_err_t f_ledc_get_duty(f_ledc_handle_t handle, uint8_t channel_id,
                           float *duty_out) {
    if (handle == NULL || channel_id >= F_LEDC_MAX_CHANNELS || duty_out == NULL)
        return ESP_ERR_INVALID_ARG;
    if (!handle->channel_in_use[channel_id]) return ESP_ERR_INVALID_STATE;
    *duty_out = handle->current_duty[channel_id];
    return ESP_OK;
}

esp_err_t f_ledc_remove_channel(f_ledc_handle_t handle, uint8_t channel_id) {
    if (handle == NULL || channel_id >= F_LEDC_MAX_CHANNELS) return ESP_ERR_INVALID_ARG;
    if (!handle->channel_in_use[channel_id]) return ESP_ERR_INVALID_STATE;

    /* Set duty to 0 before removing */
    ledc_set_duty(handle->speed_mode, channel_id, 0);
    ledc_update_duty(handle->speed_mode, channel_id);
    ledc_stop(handle->speed_mode, channel_id, 0);

    handle->channel_in_use[channel_id] = false;
    handle->channel_gpio[channel_id] = 0;
    handle->current_duty[channel_id] = 0.0f;

    ESP_LOGI(TAG, "Channel %d removed", channel_id);
    return ESP_OK;
}
