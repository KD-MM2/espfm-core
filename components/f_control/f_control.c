#include "f_control.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <math.h>

static const char *TAG = "f_control";

#define CONTROL_PERIOD_MS  1000
#define CONTROL_PRIORITY    3
#define CONTROL_STACK       3072
#define DEFAULT_HYSTERESIS  3
#define DEFAULT_RAMP_UP     10
#define DEFAULT_RAMP_DOWN   3
#define DEFAULT_SAFE_DUTY   50

struct f_control {
    f_fan_handle_t fan;
    f_source_handle_t source;
    f_curve_handle_t curve;
    TaskHandle_t task;
    bool running;
    uint8_t hysteresis_pct;
    uint8_t ramp_up_pct;
    uint8_t ramp_down_pct;
    failsafe_policy_t failsafe_policy;
    uint8_t failsafe_safe_duty;
    uint8_t stall_counter[F_FAN_MAX_COUNT];
    uint8_t prev_duty[F_FAN_MAX_COUNT];
};

/* --- Private helpers --- */

static uint8_t apply_hysteresis(uint8_t current, uint8_t target, uint8_t threshold) {
    int diff = (int)target - (int)current;
    if (abs(diff) <= (int)threshold) return current;
    return target;
}

static uint8_t apply_ramp(uint8_t current, uint8_t target,
                           uint8_t max_up, uint8_t max_down) {
    if (target > current) {
        return (target - current > max_up) ? (current + max_up) : target;
    } else if (target < current) {
        return (current - target > max_down) ? (current - max_down) : target;
    }
    return current;
}

static void _ctrl_callback(const f_fan_info_t *fan, void *ctx) {
    f_control_handle_t ctrl = (f_control_handle_t)ctx;
    if (fan->mode != FAN_MODE_AUTO || !fan->enabled) return;

    /* 1. Read source temperature */
    float temp_c;
    source_status_t status;
    esp_err_t err = f_source_get_reading(ctrl->source, fan->source_id, &temp_c, &status);
    if (err != ESP_OK || status == SOURCE_STATUS_INVALID) {
        /* 2. Fail-safe */
        switch (ctrl->failsafe_policy) {
            case FAILSAFE_HOLD:
                break;
            case FAILSAFE_FULL_SPEED:
                f_fan_set_duty(ctrl->fan, fan->id, 100);
                break;
            case FAILSAFE_SAFE_DUTY:
                f_fan_set_duty(ctrl->fan, fan->id, ctrl->failsafe_safe_duty);
                break;
            case FAILSAFE_ALT_SOURCE:
                /* TODO: alt source not yet implemented */
                break;
        }
        esp_event_post(ESPFM_EVENT, ESPFM_EVENT_SOURCE_INVALID, NULL, 0, 0);
        return;
    }

    /* 3. Look up target duty from curve */
    uint8_t target_duty;
    if (f_curve_lookup(ctrl->curve, fan->curve_id, temp_c, &target_duty) != ESP_OK) {
        return; /* No curve assigned, skip */
    }

    /* 4. Apply hysteresis + ramp-up/down */
    uint8_t current = ctrl->prev_duty[fan->id];
    uint8_t after_hyst = apply_hysteresis(current, target_duty, ctrl->hysteresis_pct);
    uint8_t final_duty = apply_ramp(current, after_hyst, ctrl->ramp_up_pct, ctrl->ramp_down_pct);

    f_fan_set_duty(ctrl->fan, fan->id, final_duty);
    ctrl->prev_duty[fan->id] = final_duty;

    /* 5. Update RPM */
    f_fan_update_rpm(ctrl->fan, fan->id);

    /* 6. Diagnostics: stall detection */
    f_fan_info_t info;
    f_fan_get_info(ctrl->fan, fan->id, &info);
    if (info.duty > 0 && info.rpm == 0) {
        ctrl->stall_counter[fan->id]++;
        if (ctrl->stall_counter[fan->id] >= 5) {
            ESP_LOGW(TAG, "Fan %d STALL alarm (duty=%d, rpm=0)", fan->id, info.duty);
            esp_event_post(ESPFM_EVENT, ESPFM_EVENT_FAN_ALARM, NULL, 0, 0);
        }
    } else {
        if (ctrl->stall_counter[fan->id] >= 5) {
            esp_event_post(ESPFM_EVENT, ESPFM_EVENT_FAN_ALARM_CLEAR, NULL, 0, 0);
        }
        ctrl->stall_counter[fan->id] = 0;
    }
}

static void _ctrl_task(void *arg) {
    f_control_handle_t ctrl = (f_control_handle_t)arg;
    ESP_LOGI(TAG, "Control loop started (period=%dms, hysteresis=%d%%, ramp=%d%%↑/%d%%↓)",
             CONTROL_PERIOD_MS, ctrl->hysteresis_pct,
             ctrl->ramp_up_pct, ctrl->ramp_down_pct);

    while (ctrl->running) {
        f_fan_for_each(ctrl->fan, _ctrl_callback, ctrl);
        vTaskDelay(pdMS_TO_TICKS(CONTROL_PERIOD_MS));
    }
    vTaskDelete(NULL);
}

/* --- Public API --- */

esp_err_t f_control_init(f_control_handle_t *handle, f_fan_handle_t fan,
                         f_source_handle_t source, f_curve_handle_t curve) {
    if (handle == NULL || fan == NULL) return ESP_ERR_INVALID_ARG;

    f_control_handle_t ctrl = calloc(1, sizeof(struct f_control));
    if (ctrl == NULL) return ESP_ERR_NO_MEM;

    ctrl->fan = fan;
    ctrl->source = source;
    ctrl->curve = curve;
    ctrl->hysteresis_pct = DEFAULT_HYSTERESIS;
    ctrl->ramp_up_pct = DEFAULT_RAMP_UP;
    ctrl->ramp_down_pct = DEFAULT_RAMP_DOWN;
    ctrl->failsafe_policy = FAILSAFE_SAFE_DUTY;
    ctrl->failsafe_safe_duty = DEFAULT_SAFE_DUTY;

    *handle = ctrl;
    ESP_LOGI(TAG, "Control engine initialized");
    return ESP_OK;
}

esp_err_t f_control_start(f_control_handle_t handle) {
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    if (handle->running) return ESP_OK;

    handle->running = true;
    BaseType_t ret = xTaskCreate(_ctrl_task, "ctrl_loop", CONTROL_STACK,
                                  handle, CONTROL_PRIORITY, &handle->task);
    if (ret != pdPASS) {
        handle->running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t f_control_stop(f_control_handle_t handle) {
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    handle->running = false;
    /* Task will exit on next loop iteration */
    return ESP_OK;
}

esp_err_t f_control_set_hysteresis(f_control_handle_t handle, uint8_t threshold_pct) {
    if (handle == NULL || threshold_pct > 100) return ESP_ERR_INVALID_ARG;
    handle->hysteresis_pct = threshold_pct;
    return ESP_OK;
}

esp_err_t f_control_set_ramp_rates(f_control_handle_t handle,
                                    uint8_t max_up_pct, uint8_t max_down_pct) {
    if (handle == NULL || max_up_pct > 100 || max_down_pct > 100)
        return ESP_ERR_INVALID_ARG;
    handle->ramp_up_pct = max_up_pct;
    handle->ramp_down_pct = max_down_pct;
    return ESP_OK;
}

esp_err_t f_control_set_failsafe(f_control_handle_t handle,
                                  failsafe_policy_t policy, uint8_t safe_duty) {
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    handle->failsafe_policy = policy;
    if (safe_duty <= 100) handle->failsafe_safe_duty = safe_duty;
    return ESP_OK;
}
