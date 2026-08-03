#include "f_schedule.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>

static const char *TAG = "f_schedule";

#define SCHEDULE_PERIOD_MS 60000

struct f_schedule {
    f_fan_handle_t fan;
    f_schedule_info_t rules[F_SCHEDULE_MAX_COUNT];
    bool slot_used[F_SCHEDULE_MAX_COUNT];
    uint8_t count;
    TimerHandle_t timer;
    bool last_active[F_SCHEDULE_MAX_COUNT];
};

/* Timer callback — runs on the FreeRTOS timer service task.
 * CAUTION: the timer service task has a limited default stack (configTIMER_TASK_STACK_DEPTH,
 * typically 4096 bytes on ESP-IDF). This callback must NOT perform deep recursion,
 * large stack allocations, or blocking I/O. Current work is safe: struct timeval,
 * a uint16_t, and a short loop with light f_fan_set_duty calls. */
static void _sched_timer_cb(TimerHandle_t timer)
{
    f_schedule_handle_t sched = (f_schedule_handle_t)pvTimerGetTimerID(timer);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    /* Guard: skip schedule evaluation until SNTP has set a reasonable time */
    if (tv.tv_sec < 1700000000) return;
    uint16_t now_min = (uint16_t)((tv.tv_sec / 60) % 1440);

    for (int i = 0; i < F_SCHEDULE_MAX_COUNT; i++) {
        if (!sched->slot_used[i] || !sched->rules[i].enabled) continue;
        f_schedule_info_t *r = &sched->rules[i];

        bool in_window       = false;
        if (r->start_min <= r->end_min) {
            in_window = (now_min >= r->start_min && now_min < r->end_min);
        } else {
            /* Overnight schedule (e.g., 22:00-02:00) */
            in_window = (now_min >= r->start_min || now_min < r->end_min);
        }

        if (in_window && !sched->last_active[i]) {
            esp_err_t err = f_fan_set_duty(sched->fan, r->fan_id, r->duty);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Schedule %d: failed to set duty on fan %d: %s", r->id, r->fan_id,
                         esp_err_to_name(err));
            }
            sched->last_active[i] = true;
            ESP_LOGD(TAG, "Schedule %d active: fan %d duty %d%%", r->id, r->fan_id, r->duty);
        } else if (!in_window && sched->last_active[i]) {
            sched->last_active[i] = false;
            ESP_LOGD(TAG, "Schedule %d expired: fan %d", r->id, r->fan_id);
        }
    }
}

esp_err_t f_schedule_init(f_schedule_handle_t *handle, f_fan_handle_t fan)
{
    if (handle == NULL || fan == NULL) return ESP_ERR_INVALID_ARG;
    f_schedule_handle_t h = calloc(1, sizeof(struct f_schedule));
    if (h == NULL) return ESP_ERR_NO_MEM;
    h->fan = fan;

    h->timer =
        xTimerCreate("sched_timer", pdMS_TO_TICKS(SCHEDULE_PERIOD_MS), pdTRUE, h, _sched_timer_cb);
    if (h->timer == NULL) {
        free(h);
        return ESP_ERR_NO_MEM;
    }

    *handle = h;
    ESP_LOGI(TAG, "Schedule service initialized (period=%ds)", SCHEDULE_PERIOD_MS / 1000);
    return ESP_OK;
}

esp_err_t f_schedule_add(f_schedule_handle_t handle, const f_schedule_info_t *info, uint8_t *id_out)
{
    if (handle == NULL || info == NULL || id_out == NULL) return ESP_ERR_INVALID_ARG;
    f_fan_info_t tmp;
    if (f_fan_get_info(handle->fan, info->fan_id, &tmp) != ESP_OK) return ESP_ERR_INVALID_ARG;
    int slot = -1;
    for (int i = 0; i < F_SCHEDULE_MAX_COUNT; i++) {
        if (!handle->slot_used[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return ESP_ERR_NO_MEM;

    memcpy(&handle->rules[slot], info, sizeof(f_schedule_info_t));
    handle->rules[slot].id  = (uint8_t)slot;
    handle->slot_used[slot] = true;
    handle->count++;
    *id_out = (uint8_t)slot;
    ESP_LOGI(TAG, "Schedule %d added: fan %d, %d-%d min, duty %d%%", slot, info->fan_id,
             info->start_min, info->end_min, info->duty);
    return ESP_OK;
}

esp_err_t f_schedule_remove(f_schedule_handle_t handle, uint8_t id)
{
    if (handle == NULL || id >= F_SCHEDULE_MAX_COUNT) return ESP_ERR_INVALID_ARG;
    if (!handle->slot_used[id]) return ESP_ERR_NOT_FOUND;
    memset(&handle->rules[id], 0, sizeof(f_schedule_info_t));
    handle->slot_used[id] = false;
    handle->count--;
    if (handle->count == 0) {
        xTimerStop(handle->timer, 0);
        ESP_LOGI(TAG, "Schedule timer stopped (no remaining schedules)");
    }
    return ESP_OK;
}

esp_err_t f_schedule_update(f_schedule_handle_t handle, uint8_t id, const f_schedule_info_t *info)
{
    if (handle == NULL || info == NULL || id >= F_SCHEDULE_MAX_COUNT) return ESP_ERR_INVALID_ARG;
    if (!handle->slot_used[id]) return ESP_ERR_NOT_FOUND;
    f_fan_info_t tmp;
    if (f_fan_get_info(handle->fan, info->fan_id, &tmp) != ESP_OK) return ESP_ERR_INVALID_ARG;

    handle->rules[id].fan_id    = info->fan_id;
    handle->rules[id].duty      = info->duty;
    handle->rules[id].start_min = info->start_min;
    handle->rules[id].end_min   = info->end_min;
    handle->rules[id].enabled   = info->enabled;
    strncpy(handle->rules[id].name, info->name, ESPFM_NAME_MAX - 1);
    handle->rules[id].name[ESPFM_NAME_MAX - 1] = '\0';
    return ESP_OK;
}

esp_err_t f_schedule_start(f_schedule_handle_t handle)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    xTimerStart(handle->timer, 0);
    ESP_LOGI(TAG, "Schedule timer started");
    return ESP_OK;
}

esp_err_t f_schedule_stop(f_schedule_handle_t handle)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    xTimerStop(handle->timer, 0);
    ESP_LOGI(TAG, "Schedule timer stopped");
    return ESP_OK;
}

uint8_t f_schedule_get_count(f_schedule_handle_t handle)
{
    return handle ? handle->count : 0;
}

esp_err_t f_schedule_get_info(f_schedule_handle_t handle, uint8_t id, f_schedule_info_t *info_out)
{
    if (handle == NULL || id >= F_SCHEDULE_MAX_COUNT || info_out == NULL)
        return ESP_ERR_INVALID_ARG;
    if (!handle->slot_used[id]) return ESP_ERR_NOT_FOUND;
    memcpy(info_out, &handle->rules[id], sizeof(f_schedule_info_t));
    return ESP_OK;
}
