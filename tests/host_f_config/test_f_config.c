/*
 * Host-based C unit tests for f_config_export_all / f_config_save_all
 * (components/f_config/f_config.c, spec-01 phase-1).
 *
 * The real f_config.c is compiled against stubbed ESP-IDF layers (see
 * stubs/) and real nanopb + espfm.pb.c / espfm_conv.c.  GNU ld --wrap hooks
 * intercept calloc/free/fopen/fwrite/fclose/pb_encode and esp_timer_get_time
 * is provided directly, so each of the 72 EPA execution paths can be driven
 * deterministically.
 *
 * Built and executed by build_and_run.sh under WSL.
 */
#define _DEFAULT_SOURCE

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_littlefs.h"
#include "espfm.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include "f_config.h"
#include "f_constraints.h"

/* ================================================================
 * --wrap target declarations (resolved by GNU ld --wrap)
 * ================================================================ */

extern void *__real_calloc(size_t n, size_t s);
extern void __real_free(void *p);
extern FILE *__real_fopen(const char *path, const char *mode);
extern size_t __real_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
extern int __real_fclose(FILE *stream);
extern bool __real_pb_encode(pb_ostream_t *stream, const pb_msgdesc_t *fields,
                             const void *src_struct);

/* ================================================================
 * Test-hook globals (controlled per test)
 * ================================================================ */

static int g_fail_calloc_on; /* 0 = never fail; N = fail the Nth calloc call */
static int g_calloc_calls;
static int g_net_allocs; /* live wrapped allocations (calloc - free) */
static int g_fail_pb_encode;
static int g_pb_encode_calls;
static int g_fopen_fail;
static int g_fopen_calls;
static int g_fwrite_short;
static int g_fwrite_calls;
static int g_fclose_calls;
static const char *g_config_path; /* mapped target for "/littlefs/config.pb" */
static uint64_t g_now_us;         /* clock returned by esp_timer_get_time */

/* --- Import-path failure-injection flags (0 = pass, 1 = inject ESP_ERR_INVALID_ARG) --- */
static int g_fail_fan_add;
static int g_fail_fan_set_mode;
static int g_fail_fan_set_duty;
static int g_fail_fan_set_group;
static int g_fail_fan_set_inverted;
static int g_fail_fan_set_enabled;
static int g_fail_fan_set_source;
static int g_fail_fan_set_curve;
static int g_fail_fan_set_schedule;
static int g_fail_source_add;
static int g_fail_source_add_ds18b20;
static int g_fail_source_update_manual;
static int g_fail_curve_upsert;
static int g_fail_schedule_add;

/* --- Import-path call counters (incremented on every stub invocation) --- */
static int g_fan_remove_calls;
static int g_source_remove_calls;
static int g_curve_remove_calls;
static int g_schedule_remove_calls;
static int g_fan_add_calls;
static int g_fan_set_mode_calls;
static int g_fan_set_duty_calls;
static int g_fan_set_group_calls;
static int g_fan_set_inverted_calls;
static int g_fan_set_enabled_calls;
static int g_fan_set_source_calls;
static int g_fan_set_curve_calls;
static int g_fan_set_schedule_calls;
static int g_source_add_calls;
static int g_source_add_ds18b20_calls;
static int g_source_update_manual_calls;
static int g_curve_upsert_calls;
static int g_schedule_add_calls;

/* Clear-loop order signal: each *_remove stub appends its letter so a reorder
 * of the four clear loops (schedules -> fans -> curves -> sources) fails the
 * sequence assertion in the clear tests. */
static char g_clear_seq[64];
static int g_clear_seq_len;

/* ================================================================
 * Log capture (stub esp_log.h -> __test_log)
 * ================================================================ */

static char g_last_log[1024];
static char g_last_log_level;

void __test_log(char level, const char *tag, const char *fmt, ...)
{
    va_list ap;
    (void)tag;
    g_last_log_level = level;
    va_start(ap, fmt);
    vsnprintf(g_last_log, sizeof(g_last_log), fmt, ap);
    va_end(ap);
}

/* ================================================================
 * --wrap implementations
 * ================================================================ */

void *__wrap_calloc(size_t n, size_t s)
{
    g_calloc_calls++;
    if (g_fail_calloc_on > 0 && g_calloc_calls == g_fail_calloc_on) return NULL;
    void *p = __real_calloc(n, s);
    if (p) g_net_allocs++;
    return p;
}

void __wrap_free(void *p)
{
    if (p) g_net_allocs--;
    __real_free(p);
}

FILE *__wrap_fopen(const char *path, const char *mode)
{
    g_fopen_calls++;
    if (g_fopen_fail) return NULL;
    if (path != NULL && strcmp(path, "/littlefs/config.pb") == 0) {
        if (g_config_path == NULL) return NULL;
        return __real_fopen(g_config_path, mode);
    }
    return __real_fopen(path, mode);
}

size_t __wrap_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    g_fwrite_calls++;
    if (g_fwrite_short) return nmemb / 2;
    return __real_fwrite(ptr, size, nmemb, stream);
}

int __wrap_fclose(FILE *stream)
{
    g_fclose_calls++;
    return __real_fclose(stream);
}

bool __wrap_pb_encode(pb_ostream_t *stream, const pb_msgdesc_t *fields, const void *src_struct)
{
    g_pb_encode_calls++;
    if (g_fail_pb_encode) {
        if (stream != NULL) stream->errmsg = "injected encode failure";
        return false;
    }
    return __real_pb_encode(stream, fields, src_struct);
}

/* Direct (non-wrapped) clock source — no real esp_timer on host. */
uint64_t esp_timer_get_time(void)
{
    return g_now_us;
}

/* ================================================================
 * ESP-IDF / registry stubs (referenced by f_config.c)
 * ================================================================ */

const char *esp_err_to_name(esp_err_t code)
{
    (void)code;
    return "stub";
}

esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *conf)
{
    (void)conf;
    return ESP_OK;
}

esp_err_t esp_littlefs_info(const char *partition_label, size_t *total_bytes, size_t *used_bytes)
{
    (void)partition_label;
    if (total_bytes) *total_bytes = 0;
    if (used_bytes) *used_bytes = 0;
    return ESP_OK;
}

/* Constraint stubs mirror components/f_constraints/f_constraints.c semantics
 * exactly so the strict validate-then-mutate contract of f_config_import_all
 * can be driven on the host. */

static const char *ERR_DUTY        = "duty must be 0-100";
static const char *ERR_MODE        = "mode must be 0 (manual) or 1 (auto)";
static const char *ERR_GPIO        = "GPIO must be 0-48";
static const char *ERR_TEMP        = "temp_c must be -40.0 to 125.0";
static const char *ERR_SCHED_RANGE = "start_min/end_min must be 0-1439";
static const char *ERR_CURVE_COUNT = "curve must have 2-16 points";
static const char *ERR_CURVE_ORDER = "curve points must be sorted by temp_c ascending";
static const char *ERR_FAN_FULL    = "max fans reached (8)";
static const char *ERR_SOURCE_FULL = "max sources reached (8)";
static const char *ERR_CURVE_FULL  = "max curves reached (16)";
static const char *ERR_SCHED_FULL  = "max schedules reached (8)";

esp_err_t f_constraints_gpio(int val, const char **err_msg)
{
    if (val < F_CONSTRAINT_GPIO_MIN || val > F_CONSTRAINT_GPIO_MAX) {
        if (err_msg) *err_msg = ERR_GPIO;
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t f_constraints_mode(int val, const char **err_msg)
{
    if (val < F_CONSTRAINT_MODE_MIN || val > F_CONSTRAINT_MODE_MAX) {
        if (err_msg) *err_msg = ERR_MODE;
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t f_constraints_duty(int val, const char **err_msg)
{
    if (val < F_CONSTRAINT_DUTY_MIN || val > F_CONSTRAINT_DUTY_MAX) {
        if (err_msg) *err_msg = ERR_DUTY;
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t f_constraints_schedule_time(int start_min, int end_min, const char **err_msg)
{
    if (start_min < F_CONSTRAINT_SCHED_MIN || start_min > F_CONSTRAINT_SCHED_MAX ||
        end_min < F_CONSTRAINT_SCHED_MIN || end_min > F_CONSTRAINT_SCHED_MAX) {
        if (err_msg) *err_msg = ERR_SCHED_RANGE;
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t f_constraints_curve_points(const f_curve_point_t *points, uint8_t count,
                                     const char **err_msg)
{
    if (count < F_CONSTRAINT_CURVE_POINTS_MIN || count > F_CURVE_MAX_POINTS) {
        if (err_msg) *err_msg = ERR_CURVE_COUNT;
        return ESP_ERR_INVALID_ARG;
    }
    for (uint8_t i = 1; i < count; i++) {
        if (points[i].temp_c <= points[i - 1].temp_c) {
            if (err_msg) *err_msg = ERR_CURVE_ORDER;
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

esp_err_t f_constraints_fan_count(uint8_t current, const char **err_msg)
{
    if (current >= F_FAN_MAX_COUNT) {
        if (err_msg) *err_msg = ERR_FAN_FULL;
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t f_constraints_source_count(uint8_t current, const char **err_msg)
{
    if (current >= F_SOURCE_MAX_COUNT) {
        if (err_msg) *err_msg = ERR_SOURCE_FULL;
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t f_constraints_curve_count(uint8_t current, const char **err_msg)
{
    if (current >= F_CURVE_MAX_COUNT) {
        if (err_msg) *err_msg = ERR_CURVE_FULL;
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t f_constraints_schedule_count(uint8_t current, const char **err_msg)
{
    if (current >= F_SCHEDULE_MAX_COUNT) {
        if (err_msg) *err_msg = ERR_SCHED_FULL;
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t f_constraints_temp_c(float val, const char **err_msg)
{
    if (val < F_CONSTRAINT_TEMP_C_MIN || val > F_CONSTRAINT_TEMP_C_MAX) {
        if (err_msg) *err_msg = ERR_TEMP;
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* --- Registry stub state (per registry: used-flag array + info array) --- */

static bool fan_used[F_FAN_MAX_COUNT];
static f_fan_info_t fan_info[F_FAN_MAX_COUNT];
static bool src_used[F_SOURCE_MAX_COUNT];
static f_source_info_t src_info[F_SOURCE_MAX_COUNT];
static bool cur_used[F_CURVE_MAX_COUNT];
static f_curve_info_t cur_info[F_CURVE_MAX_COUNT];
static bool sched_used[F_SCHEDULE_MAX_COUNT];
static f_schedule_info_t sched_info[F_SCHEDULE_MAX_COUNT];

/* --- Registry stub accessors (the functions f_config.c calls) --- */

esp_err_t f_fan_get_info(f_fan_handle_t handle, uint8_t id, f_fan_info_t *info_out)
{
    if (handle == NULL || id >= F_FAN_MAX_COUNT) return ESP_ERR_INVALID_ARG;
    if (!fan_used[id]) return ESP_ERR_NOT_FOUND;
    *info_out = fan_info[id];
    return ESP_OK;
}

uint8_t f_fan_get_count(f_fan_handle_t handle)
{
    (void)handle;
    uint8_t n = 0;
    for (int i = 0; i < F_FAN_MAX_COUNT; i++)
        if (fan_used[i]) n++;
    return n;
}

esp_err_t f_source_get_info(f_source_handle_t handle, uint8_t id, f_source_info_t *info_out)
{
    if (handle == NULL || id >= F_SOURCE_MAX_COUNT) return ESP_ERR_INVALID_ARG;
    if (!src_used[id]) return ESP_ERR_NOT_FOUND;
    *info_out = src_info[id];
    return ESP_OK;
}

uint8_t f_source_get_count(f_source_handle_t handle)
{
    (void)handle;
    uint8_t n = 0;
    for (int i = 0; i < F_SOURCE_MAX_COUNT; i++)
        if (src_used[i]) n++;
    return n;
}

esp_err_t f_curve_get_info(f_curve_handle_t handle, uint8_t id, f_curve_info_t *info_out)
{
    if (handle == NULL || id >= F_CURVE_MAX_COUNT) return ESP_ERR_INVALID_ARG;
    if (!cur_used[id]) return ESP_ERR_NOT_FOUND;
    *info_out = cur_info[id];
    return ESP_OK;
}

uint8_t f_curve_get_count(f_curve_handle_t handle)
{
    (void)handle;
    uint8_t n = 0;
    for (int i = 0; i < F_CURVE_MAX_COUNT; i++)
        if (cur_used[i]) n++;
    return n;
}

esp_err_t f_schedule_get_info(f_schedule_handle_t handle, uint8_t id, f_schedule_info_t *info_out)
{
    if (handle == NULL || id >= F_SCHEDULE_MAX_COUNT) return ESP_ERR_INVALID_ARG;
    if (!sched_used[id]) return ESP_ERR_NOT_FOUND;
    *info_out = sched_info[id];
    return ESP_OK;
}

uint8_t f_schedule_get_count(f_schedule_handle_t handle)
{
    (void)handle;
    uint8_t n = 0;
    for (int i = 0; i < F_SCHEDULE_MAX_COUNT; i++)
        if (sched_used[i]) n++;
    return n;
}

/* --- Registry mutation stubs (add/remove/set).  Each *_remove increments its
 *     call counter on every invocation (so clear-all loops are observable);
 *     each *_add / set_* records usage into the same fan_used/fan_info/...
 *     arrays the get_info/get_count stubs read, making post-import state
 *     observable.  Failure-injection flags force ESP_ERR_INVALID_ARG. --- */

esp_err_t f_fan_remove(f_fan_handle_t handle, uint8_t id)
{
    g_fan_remove_calls++;
    if (g_clear_seq_len < 63) g_clear_seq[g_clear_seq_len++] = 'F';
    if (handle == NULL || id >= F_FAN_MAX_COUNT) return ESP_ERR_INVALID_ARG;
    if (!fan_used[id]) return ESP_ERR_NOT_FOUND;
    fan_used[id] = false;
    memset(&fan_info[id], 0, sizeof(fan_info[id]));
    return ESP_OK;
}

esp_err_t f_source_remove(f_source_handle_t handle, uint8_t id)
{
    g_source_remove_calls++;
    if (g_clear_seq_len < 63) g_clear_seq[g_clear_seq_len++] = 'R';
    if (handle == NULL || id >= F_SOURCE_MAX_COUNT) return ESP_ERR_INVALID_ARG;
    if (!src_used[id]) return ESP_ERR_NOT_FOUND;
    src_used[id] = false;
    memset(&src_info[id], 0, sizeof(src_info[id]));
    return ESP_OK;
}

esp_err_t f_curve_remove(f_curve_handle_t handle, uint8_t id)
{
    g_curve_remove_calls++;
    if (g_clear_seq_len < 63) g_clear_seq[g_clear_seq_len++] = 'C';
    if (handle == NULL || id >= F_CURVE_MAX_COUNT) return ESP_ERR_INVALID_ARG;
    if (!cur_used[id]) return ESP_ERR_NOT_FOUND;
    cur_used[id] = false;
    memset(&cur_info[id], 0, sizeof(cur_info[id]));
    return ESP_OK;
}

esp_err_t f_schedule_remove(f_schedule_handle_t handle, uint8_t id)
{
    g_schedule_remove_calls++;
    if (g_clear_seq_len < 63) g_clear_seq[g_clear_seq_len++] = 'S';
    if (handle == NULL || id >= F_SCHEDULE_MAX_COUNT) return ESP_ERR_INVALID_ARG;
    if (!sched_used[id]) return ESP_ERR_NOT_FOUND;
    sched_used[id] = false;
    memset(&sched_info[id], 0, sizeof(sched_info[id]));
    return ESP_OK;
}

esp_err_t f_fan_add(f_fan_handle_t handle, uint8_t pwm_gpio, uint8_t tach_gpio, const char *name,
                    uint8_t *id_out, const char **err_msg)
{
    g_fan_add_calls++;
    if (handle == NULL || id_out == NULL) return ESP_ERR_INVALID_ARG;
    if (g_fail_fan_add) return ESP_ERR_INVALID_ARG;
    uint8_t slot = 0;
    while (slot < F_FAN_MAX_COUNT && fan_used[slot]) slot++;
    if (slot >= F_FAN_MAX_COUNT) return ESP_ERR_NO_MEM;
    fan_used[slot] = true;
    memset(&fan_info[slot], 0, sizeof(fan_info[slot]));
    fan_info[slot].id = slot;
    strncpy(fan_info[slot].name, name, ESPFM_NAME_MAX - 1);
    fan_info[slot].name[ESPFM_NAME_MAX - 1] = '\0';
    fan_info[slot].pwm_gpio                 = pwm_gpio;
    fan_info[slot].tach_gpio                = tach_gpio;
    *id_out                                 = slot;
    return ESP_OK;
}

esp_err_t f_fan_set_mode(f_fan_handle_t handle, uint8_t id, fan_mode_t mode)
{
    g_fan_set_mode_calls++;
    if (handle == NULL || id >= F_FAN_MAX_COUNT) return ESP_ERR_INVALID_ARG;
    if (g_fail_fan_set_mode) return ESP_ERR_INVALID_ARG;
    if (!fan_used[id]) return ESP_ERR_NOT_FOUND;
    fan_info[id].mode = mode;
    return ESP_OK;
}

esp_err_t f_fan_set_duty(f_fan_handle_t handle, uint8_t id, uint8_t duty)
{
    g_fan_set_duty_calls++;
    if (handle == NULL || id >= F_FAN_MAX_COUNT) return ESP_ERR_INVALID_ARG;
    if (g_fail_fan_set_duty) return ESP_ERR_INVALID_ARG;
    if (!fan_used[id]) return ESP_ERR_NOT_FOUND;
    fan_info[id].duty = duty;
    return ESP_OK;
}

esp_err_t f_fan_set_group(f_fan_handle_t handle, uint8_t id, uint8_t group_id)
{
    g_fan_set_group_calls++;
    if (handle == NULL || id >= F_FAN_MAX_COUNT) return ESP_ERR_INVALID_ARG;
    if (g_fail_fan_set_group) return ESP_ERR_INVALID_ARG;
    if (!fan_used[id]) return ESP_ERR_NOT_FOUND;
    fan_info[id].group_id = group_id;
    return ESP_OK;
}

esp_err_t f_fan_set_inverted(f_fan_handle_t handle, uint8_t id, bool inverted)
{
    g_fan_set_inverted_calls++;
    if (handle == NULL || id >= F_FAN_MAX_COUNT) return ESP_ERR_INVALID_ARG;
    if (g_fail_fan_set_inverted) return ESP_ERR_INVALID_ARG;
    if (!fan_used[id]) return ESP_ERR_NOT_FOUND;
    fan_info[id].inverted = inverted;
    return ESP_OK;
}

esp_err_t f_fan_set_enabled(f_fan_handle_t handle, uint8_t id, bool enabled)
{
    g_fan_set_enabled_calls++;
    if (handle == NULL || id >= F_FAN_MAX_COUNT) return ESP_ERR_INVALID_ARG;
    if (g_fail_fan_set_enabled) return ESP_ERR_INVALID_ARG;
    if (!fan_used[id]) return ESP_ERR_NOT_FOUND;
    fan_info[id].enabled = enabled;
    return ESP_OK;
}

/* Live f_gpio registry stub. The import-validation live-registry checks are
 * gated on a non-NULL gpio handle. Mirror the real f_gpio_get_caps contract:
 * out-of-range pins return 0 (f_gpio.c), while a pin in g_reserved_gpio[]
 * returns the 0xFFFFFFFF reserved sentinel the real registry stamps at init;
 * otherwise the per-pin caps array (default 0). Tests may install
 * g_reserved_gpio[] / g_caps_override to exercise the "GPIO is reserved" and
 * "GPIO is the active DS18B20 bus pin" branches. */
static uint8_t g_reserved_gpio[8];
static uint32_t g_caps_override[F_GPIO_MAX_PINS];

uint32_t f_gpio_get_caps(f_gpio_handle_t handle, uint8_t pin)
{
    (void)handle;
    if (pin >= F_GPIO_MAX_PINS) return 0; /* real f_gpio_get_caps: OOR is 0, not reserved */
    for (int i = 0; i < 8 && g_reserved_gpio[i] != 0xFF; i++)
        if (g_reserved_gpio[i] == pin) return 0xFFFFFFFF;
    return g_caps_override[pin];
}

esp_err_t f_fan_set_source(f_fan_handle_t handle, uint8_t id, uint8_t source_id)
{
    g_fan_set_source_calls++;
    if (handle == NULL || id >= F_FAN_MAX_COUNT) return ESP_ERR_INVALID_ARG;
    if (g_fail_fan_set_source) return ESP_ERR_INVALID_ARG;
    if (!fan_used[id]) return ESP_ERR_NOT_FOUND;
    fan_info[id].source_id = source_id;
    return ESP_OK;
}

esp_err_t f_fan_set_curve(f_fan_handle_t handle, uint8_t id, uint8_t curve_id)
{
    g_fan_set_curve_calls++;
    if (handle == NULL || id >= F_FAN_MAX_COUNT) return ESP_ERR_INVALID_ARG;
    if (g_fail_fan_set_curve) return ESP_ERR_INVALID_ARG;
    if (!fan_used[id]) return ESP_ERR_NOT_FOUND;
    fan_info[id].curve_id = curve_id;
    return ESP_OK;
}

esp_err_t f_fan_set_schedule(f_fan_handle_t handle, uint8_t id, uint8_t schedule_id)
{
    g_fan_set_schedule_calls++;
    if (handle == NULL || id >= F_FAN_MAX_COUNT) return ESP_ERR_INVALID_ARG;
    if (g_fail_fan_set_schedule) return ESP_ERR_INVALID_ARG;
    if (!fan_used[id]) return ESP_ERR_NOT_FOUND;
    fan_info[id].schedule_id = schedule_id;
    return ESP_OK;
}

esp_err_t f_source_add(f_source_handle_t handle, source_type_t type, uint8_t gpio, const char *name,
                       uint8_t *id_out, const char **err_msg)
{
    g_source_add_calls++;
    if (handle == NULL || id_out == NULL) return ESP_ERR_INVALID_ARG;
    if (g_fail_source_add) return ESP_ERR_INVALID_ARG;
    uint8_t slot = 0;
    while (slot < F_SOURCE_MAX_COUNT && src_used[slot]) slot++;
    if (slot >= F_SOURCE_MAX_COUNT) return ESP_ERR_NO_MEM;
    src_used[slot] = true;
    memset(&src_info[slot], 0, sizeof(src_info[slot]));
    src_info[slot].id = slot;
    strncpy(src_info[slot].name, name, ESPFM_NAME_MAX - 1);
    src_info[slot].name[ESPFM_NAME_MAX - 1] = '\0';
    src_info[slot].type                     = type;
    src_info[slot].gpio                     = gpio;
    *id_out                                 = slot;
    return ESP_OK;
}

esp_err_t f_source_add_ds18b20(f_source_handle_t handle, uint64_t rom_code, const char *name,
                               uint8_t *id_out)
{
    g_source_add_ds18b20_calls++;
    if (handle == NULL || id_out == NULL) return ESP_ERR_INVALID_ARG;
    if (g_fail_source_add_ds18b20) return ESP_ERR_INVALID_ARG;
    uint8_t slot = 0;
    while (slot < F_SOURCE_MAX_COUNT && src_used[slot]) slot++;
    if (slot >= F_SOURCE_MAX_COUNT) return ESP_ERR_NO_MEM;
    src_used[slot] = true;
    memset(&src_info[slot], 0, sizeof(src_info[slot]));
    src_info[slot].id = slot;
    strncpy(src_info[slot].name, name, ESPFM_NAME_MAX - 1);
    src_info[slot].name[ESPFM_NAME_MAX - 1] = '\0';
    src_info[slot].type                     = SOURCE_TYPE_DS18B20;
    src_info[slot].ds18b20_rom_code         = rom_code;
    *id_out                                 = slot;
    return ESP_OK;
}

esp_err_t f_source_update_manual(f_source_handle_t handle, uint8_t id, float temp_c)
{
    g_source_update_manual_calls++;
    if (handle == NULL || id >= F_SOURCE_MAX_COUNT) return ESP_ERR_INVALID_ARG;
    if (g_fail_source_update_manual) return ESP_ERR_INVALID_ARG;
    if (!src_used[id]) return ESP_ERR_NOT_FOUND;
    src_info[id].temp_c = temp_c;
    return ESP_OK;
}

esp_err_t f_curve_upsert(f_curve_handle_t handle, const f_curve_info_t *info, uint8_t *id_out)
{
    g_curve_upsert_calls++;
    if (handle == NULL || info == NULL || id_out == NULL) return ESP_ERR_INVALID_ARG;
    if (g_fail_curve_upsert) return ESP_ERR_INVALID_ARG;
    uint8_t slot = info->id;
    if (slot >= F_CURVE_MAX_COUNT || !cur_used[slot]) {
        slot = 0;
        while (slot < F_CURVE_MAX_COUNT && cur_used[slot]) slot++;
        if (slot >= F_CURVE_MAX_COUNT) return ESP_ERR_NO_MEM;
    }
    cur_used[slot]    = true;
    cur_info[slot]    = *info;
    cur_info[slot].id = slot;
    *id_out           = slot;
    return ESP_OK;
}

esp_err_t f_schedule_add(f_schedule_handle_t handle, const f_schedule_info_t *info, uint8_t *id_out)
{
    g_schedule_add_calls++;
    if (handle == NULL || info == NULL || id_out == NULL) return ESP_ERR_INVALID_ARG;
    if (g_fail_schedule_add) return ESP_ERR_INVALID_ARG;
    uint8_t slot = 0;
    while (slot < F_SCHEDULE_MAX_COUNT && sched_used[slot]) slot++;
    if (slot >= F_SCHEDULE_MAX_COUNT) return ESP_ERR_NO_MEM;
    sched_used[slot]    = true;
    sched_info[slot]    = *info;
    sched_info[slot].id = slot;
    *id_out             = slot;
    return ESP_OK;
}

/* ================================================================
 * Registry setup helpers
 * ================================================================ */

static void regs_reset(void)
{
    memset(fan_used, 0, sizeof(fan_used));
    memset(src_used, 0, sizeof(src_used));
    memset(cur_used, 0, sizeof(cur_used));
    memset(sched_used, 0, sizeof(sched_used));
    memset(fan_info, 0, sizeof(fan_info));
    memset(src_info, 0, sizeof(src_info));
    memset(cur_info, 0, sizeof(cur_info));
    memset(sched_info, 0, sizeof(sched_info));
}

static void reg_fan_add(uint8_t id, const char *name, uint8_t duty, uint8_t pwm_gpio)
{
    fan_used[id] = true;
    memset(&fan_info[id], 0, sizeof(fan_info[id]));
    fan_info[id].id = id;
    strncpy(fan_info[id].name, name, ESPFM_NAME_MAX - 1);
    fan_info[id].name[ESPFM_NAME_MAX - 1] = '\0';
    fan_info[id].duty                     = duty;
    fan_info[id].pwm_gpio                 = pwm_gpio;
}

static void reg_source_add(uint8_t id, const char *name, source_type_t type, float temp_c)
{
    src_used[id] = true;
    memset(&src_info[id], 0, sizeof(src_info[id]));
    src_info[id].id = id;
    strncpy(src_info[id].name, name, ESPFM_NAME_MAX - 1);
    src_info[id].name[ESPFM_NAME_MAX - 1] = '\0';
    src_info[id].type                     = type;
    src_info[id].temp_c                   = temp_c;
}

static void reg_curve_add(uint8_t id, const char *name, uint8_t num_points)
{
    cur_used[id] = true;
    memset(&cur_info[id], 0, sizeof(cur_info[id]));
    cur_info[id].id = id;
    strncpy(cur_info[id].name, name, ESPFM_NAME_MAX - 1);
    cur_info[id].name[ESPFM_NAME_MAX - 1] = '\0';
    cur_info[id].num_points               = num_points;
    if (num_points > 0) {
        cur_info[id].points[0].temp_c = 30.0f;
        cur_info[id].points[0].duty   = 20;
    }
    if (num_points > 1) {
        cur_info[id].points[1].temp_c = 50.0f;
        cur_info[id].points[1].duty   = 50;
    }
}

static void reg_schedule_add(uint8_t id, const char *name, uint8_t fan_id, uint8_t duty,
                             uint16_t start_min, uint16_t end_min, bool enabled)
{
    sched_used[id] = true;
    memset(&sched_info[id], 0, sizeof(sched_info[id]));
    sched_info[id].id = id;
    strncpy(sched_info[id].name, name, ESPFM_NAME_MAX - 1);
    sched_info[id].name[ESPFM_NAME_MAX - 1] = '\0';
    sched_info[id].fan_id                   = fan_id;
    sched_info[id].duty                     = duty;
    sched_info[id].start_min                = start_min;
    sched_info[id].end_min                  = end_min;
    sched_info[id].enabled                  = enabled;
}

/* ================================================================
 * Minimal test framework
 * ================================================================ */

static int g_pass;
static int g_fail;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_fail++;                                                  \
            return;                                                    \
        }                                                              \
    } while (0)

static void reset_test_state(void)
{
    g_fail_calloc_on            = 0;
    g_calloc_calls              = 0;
    g_net_allocs                = 0;
    g_fail_pb_encode            = 0;
    g_pb_encode_calls           = 0;
    g_fopen_fail                = 0;
    g_fopen_calls               = 0;
    g_fwrite_short              = 0;
    g_fwrite_calls              = 0;
    g_fclose_calls              = 0;
    g_config_path               = NULL;
    g_now_us                    = 0;
    g_last_log[0]               = '\0';
    g_last_log_level            = '\0';
    g_fail_fan_add              = 0;
    g_fail_fan_set_mode         = 0;
    g_fail_fan_set_duty         = 0;
    g_fail_fan_set_group        = 0;
    g_fail_fan_set_inverted     = 0;
    g_fail_fan_set_enabled      = 0;
    g_fail_fan_set_source       = 0;
    g_fail_fan_set_curve        = 0;
    g_fail_fan_set_schedule     = 0;
    g_fail_source_add           = 0;
    g_fail_source_add_ds18b20   = 0;
    g_fail_source_update_manual = 0;
    g_fail_curve_upsert         = 0;
    g_fail_schedule_add         = 0;
    g_fan_remove_calls          = 0;
    g_source_remove_calls       = 0;
    g_curve_remove_calls        = 0;
    g_schedule_remove_calls     = 0;
    g_clear_seq[0]              = '\0';
    g_clear_seq_len             = 0;
    g_fan_add_calls             = 0;
    g_fan_set_mode_calls        = 0;
    g_fan_set_duty_calls        = 0;
    g_fan_set_group_calls       = 0;
    g_fan_set_inverted_calls    = 0;
    g_fan_set_enabled_calls     = 0;
    g_fan_set_source_calls      = 0;
    memset(g_reserved_gpio, 0xFF, sizeof(g_reserved_gpio)); /* no reserved pins */
    memset(g_caps_override, 0, sizeof(g_caps_override));
    g_fan_set_curve_calls        = 0;
    g_fan_set_schedule_calls     = 0;
    g_source_add_calls           = 0;
    g_source_add_ds18b20_calls   = 0;
    g_source_update_manual_calls = 0;
    g_curve_upsert_calls         = 0;
    g_schedule_add_calls         = 0;
    regs_reset();
}

#define H_FAN ((f_fan_handle_t)0x1)
#define H_SRC ((f_source_handle_t)0x1)
#define H_CUR ((f_curve_handle_t)0x1)
#define H_SCH ((f_schedule_handle_t)0x1)

/* Mirrors the private layout in f_config.c so tests can drive mounted/clock. */
struct f_config {
    char partition_label[32];
    bool mounted;
    uint64_t last_save_us;
};

/* Static-assert the mirror struct layout so a change to f_config.c's private
 * struct (or the mirror) fails the build instead of silently corrupting the
 * save-side tests.  char[32] + bool -> uint64_t aligned to 8 => offset 40. */
_Static_assert(offsetof(struct f_config, partition_label) == 0,
               "f_config mirror: partition_label offset");
_Static_assert(offsetof(struct f_config, mounted) == 32, "f_config mirror: mounted offset");
_Static_assert(offsetof(struct f_config, last_save_us) == 40,
               "f_config mirror: last_save_us offset");
_Static_assert(sizeof(struct f_config) == 48, "f_config mirror: total size");

static int decode_config(const uint8_t *buf, size_t len, ConfigFile *out)
{
    memset(out, 0, sizeof(*out));
    pb_istream_t is = pb_istream_from_buffer(buf, len);
    return pb_decode(&is, &ConfigFile_msg, out);
}

static void populate_all_registries(void)
{
    reg_fan_add(0, "fan0", 50, 15);
    reg_fan_add(1, "fan1", 80, 25);
    reg_source_add(0, "src0", SOURCE_TYPE_MANUAL, 40.0f);
    reg_curve_add(0, "curve0", 2);
    reg_curve_add(1, "curve1", 1);
    reg_schedule_add(0, "sch0", 1, 50, 480, 1080, true);
}

/* Read a saved protobuf file back and decode it into *out. Returns 1 on success. */
static int read_config_file(const char *path, ConfigFile *out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return 0;
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz <= 0 || (unsigned long)fsz > ConfigFile_size) {
        fclose(f);
        return 0;
    }
    uint8_t fdata[ConfigFile_size];
    size_t rd = fread(fdata, 1, (size_t)fsz, f);
    fclose(f);
    if (rd != (size_t)fsz) return 0;
    return decode_config(fdata, (size_t)fsz, out);
}

/* True when no registry mutation or persistence call happened at all. */
static int mutation_clean(void)
{
    return g_fan_remove_calls == 0 && g_source_remove_calls == 0 && g_curve_remove_calls == 0 &&
           g_schedule_remove_calls == 0 && g_fan_add_calls == 0 && g_source_add_calls == 0 &&
           g_curve_upsert_calls == 0 && g_schedule_add_calls == 0 && g_fopen_calls == 0;
}

/* Build a 1-fan ConfigFile. Binding ids default to 0xFF (skipped in apply). */
static void cfg_single_fan(ConfigFile *cfg, uint32_t source_id, uint32_t curve_id,
                           uint32_t schedule_id)
{
    *cfg                 = (ConfigFile)ConfigFile_init_default;
    cfg->has_fans        = true;
    cfg->fans.fans_count = 1;
    FanInfo *f           = &cfg->fans.fans[0];
    *f                   = (FanInfo)FanInfo_init_default;
    strncpy(f->name, "fan0", sizeof(f->name) - 1);
    f->name[sizeof(f->name) - 1] = '\0';
    f->mode                      = 0;
    f->duty                      = 50;
    f->pwm_gpio                  = 15;
    f->tach_gpio                 = 255;
    f->source_id                 = source_id;
    f->curve_id                  = curve_id;
    f->schedule_id               = schedule_id;
    f->group_id                  = 0;
    f->inverted                  = false;
}

/* Build the full H-P1 config: 1 fan, 3 sources (NTC/DS18B20/MANUAL), 1 curve,
 * 1 schedule. */
static void build_happy_cfg(ConfigFile *cfg)
{
    *cfg                 = (ConfigFile)ConfigFile_init_default;

    cfg->has_fans        = true;
    cfg->fans.fans_count = 1;
    FanInfo *fan0        = &cfg->fans.fans[0];
    *fan0                = (FanInfo)FanInfo_init_default;
    strncpy(fan0->name, "fan0", sizeof(fan0->name) - 1);
    fan0->name[sizeof(fan0->name) - 1] = '\0';
    fan0->mode                         = FanMode_FAN_MODE_AUTO;
    fan0->duty                         = 50;
    fan0->pwm_gpio                     = 15;
    fan0->tach_gpio                    = 255;
    fan0->source_id                    = 0;
    fan0->curve_id                     = 0;
    fan0->schedule_id                  = 0;
    fan0->group_id                     = 2;
    fan0->inverted                     = true;

    cfg->has_sources                   = true;
    cfg->sources.sources_count         = 3;
    SourceInfo *s0                     = &cfg->sources.sources[0];
    *s0                                = (SourceInfo)SourceInfo_init_default;
    strncpy(s0->name, "ntc0", sizeof(s0->name) - 1);
    s0->name[sizeof(s0->name) - 1] = '\0';
    s0->type                       = SourceType_SOURCE_TYPE_NTC;
    s0->gpio                       = 34;

    SourceInfo *s1                 = &cfg->sources.sources[1];
    *s1                            = (SourceInfo)SourceInfo_init_default;
    strncpy(s1->name, "ds0", sizeof(s1->name) - 1);
    s1->name[sizeof(s1->name) - 1] = '\0';
    s1->type                       = SourceType_SOURCE_TYPE_DS18B20;
    s1->ds18b20_rom_code           = 0x28ABCDULL;

    SourceInfo *s2                 = &cfg->sources.sources[2];
    *s2                            = (SourceInfo)SourceInfo_init_default;
    strncpy(s2->name, "man0", sizeof(s2->name) - 1);
    s2->name[sizeof(s2->name) - 1] = '\0';
    s2->type                       = SourceType_SOURCE_TYPE_MANUAL;
    s2->gpio                       = 255;
    s2->temp_c                     = 25.5f;

    cfg->has_curves                = true;
    cfg->curves.curves_count       = 1;
    CurveInfo *c0                  = &cfg->curves.curves[0];
    *c0                            = (CurveInfo)CurveInfo_init_default;
    c0->id                         = 0;
    strncpy(c0->name, "curve0", sizeof(c0->name) - 1);
    c0->name[sizeof(c0->name) - 1] = '\0';
    c0->points_count               = 2;
    c0->points[0].temp_c           = 30.0f;
    c0->points[0].duty             = 20;
    c0->points[1].temp_c           = 50.0f;
    c0->points[1].duty             = 60;

    cfg->has_schedules             = true;
    cfg->schedules.schedules_count = 1;
    ScheduleInfo *sch0             = &cfg->schedules.schedules[0];
    *sch0                          = (ScheduleInfo)ScheduleInfo_init_default;
    sch0->fan_id                   = 0;
    sch0->duty                     = 50;
    sch0->start_min                = 480;
    sch0->end_min                  = 1080;
    sch0->enabled                  = true;
    strncpy(sch0->name, "sch0", sizeof(sch0->name) - 1);
    sch0->name[sizeof(sch0->name) - 1] = '\0';
}

/* ================================================================
 * f_config_export_all tests
 * ================================================================ */

/* P1 — reject NULL buf_out */
static void test_f_config_export_all_rejects_null_buf_out(void)
{
    reset_test_state();
    size_t len    = 0;
    esp_err_t err = f_config_export_all(H_FAN, H_SRC, H_CUR, H_SCH, NULL, &len);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(len == 0);
    CHECK(g_calloc_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P2 — reject NULL len_out */
static void test_f_config_export_all_rejects_null_len_out(void)
{
    reset_test_state();
    uint8_t *buf  = NULL;
    esp_err_t err = f_config_export_all(H_FAN, H_SRC, H_CUR, H_SCH, &buf, NULL);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(buf == NULL);
    CHECK(g_calloc_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P3 — first calloc (ConfigFile) fails */
static void test_f_config_export_all_cfg_calloc_failure(void)
{
    reset_test_state();
    g_fail_calloc_on = 1;
    uint8_t *buf     = (uint8_t *)1;
    size_t len       = 123;
    esp_err_t err    = f_config_export_all(H_FAN, H_SRC, H_CUR, H_SCH, &buf, &len);
    CHECK(err == ESP_ERR_NO_MEM);
    CHECK(buf == (uint8_t *)1);
    CHECK(len == 123);
    CHECK(g_calloc_calls == 1);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P4 — happy path: decode back and check version + counts */
static void test_f_config_export_all_success_decodes_version_and_counts(void)
{
    reset_test_state();
    populate_all_registries();
    uint8_t *buf  = NULL;
    size_t len    = 0;
    esp_err_t err = f_config_export_all(H_FAN, H_SRC, H_CUR, H_SCH, &buf, &len);
    CHECK(err == ESP_OK);
    CHECK(buf != NULL);
    CHECK(len > 0);
    ConfigFile cfg;
    CHECK(decode_config(buf, len, &cfg));
    CHECK(strcmp(cfg.version, "3.0") == 0);
    CHECK(cfg.has_fans && cfg.fans.fans_count == 2);
    CHECK(cfg.has_sources && cfg.sources.sources_count == 1);
    CHECK(cfg.has_curves && cfg.curves.curves_count == 2);
    CHECK(cfg.has_schedules && cfg.schedules.schedules_count == 1);
    free(buf);
    CHECK(g_net_allocs == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P5 — NULL fan: has_fans stays true, fans_count 0 */
static void test_f_config_export_all_null_fan_leaves_has_fans_true_empty(void)
{
    reset_test_state();
    reg_source_add(0, "src0", SOURCE_TYPE_MANUAL, 40.0f);
    reg_curve_add(0, "curve0", 2);
    reg_schedule_add(0, "sch0", 1, 50, 480, 1080, true);
    uint8_t *buf  = NULL;
    size_t len    = 0;
    esp_err_t err = f_config_export_all(NULL, H_SRC, H_CUR, H_SCH, &buf, &len);
    CHECK(err == ESP_OK);
    ConfigFile cfg;
    CHECK(decode_config(buf, len, &cfg));
    CHECK(cfg.has_fans == true);
    CHECK(cfg.fans.fans_count == 0);
    CHECK(cfg.has_sources && cfg.sources.sources_count == 1);
    CHECK(cfg.has_curves && cfg.curves.curves_count == 1);
    CHECK(cfg.has_schedules && cfg.schedules.schedules_count == 1);
    free(buf);
    CHECK(g_net_allocs == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P6 — NULL source: has_sources stays true, sources_count 0 */
static void test_f_config_export_all_null_source_leaves_has_sources_true_empty(void)
{
    reset_test_state();
    reg_fan_add(0, "fan0", 50, 15);
    reg_curve_add(0, "curve0", 2);
    reg_schedule_add(0, "sch0", 1, 50, 480, 1080, true);
    uint8_t *buf  = NULL;
    size_t len    = 0;
    esp_err_t err = f_config_export_all(H_FAN, NULL, H_CUR, H_SCH, &buf, &len);
    CHECK(err == ESP_OK);
    ConfigFile cfg;
    CHECK(decode_config(buf, len, &cfg));
    CHECK(cfg.has_sources == true);
    CHECK(cfg.sources.sources_count == 0);
    CHECK(cfg.has_fans && cfg.fans.fans_count == 1);
    CHECK(cfg.has_curves && cfg.curves.curves_count == 1);
    CHECK(cfg.has_schedules && cfg.schedules.schedules_count == 1);
    free(buf);
    CHECK(g_net_allocs == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P7 — NULL curve: has_curves stays true, curves_count 0 */
static void test_f_config_export_all_null_curve_leaves_has_curves_true_empty(void)
{
    reset_test_state();
    reg_fan_add(0, "fan0", 50, 15);
    reg_source_add(0, "src0", SOURCE_TYPE_MANUAL, 40.0f);
    reg_schedule_add(0, "sch0", 1, 50, 480, 1080, true);
    uint8_t *buf  = NULL;
    size_t len    = 0;
    esp_err_t err = f_config_export_all(H_FAN, H_SRC, NULL, H_SCH, &buf, &len);
    CHECK(err == ESP_OK);
    ConfigFile cfg;
    CHECK(decode_config(buf, len, &cfg));
    CHECK(cfg.has_curves == true);
    CHECK(cfg.curves.curves_count == 0);
    CHECK(cfg.has_fans && cfg.fans.fans_count == 1);
    CHECK(cfg.has_sources && cfg.sources.sources_count == 1);
    CHECK(cfg.has_schedules && cfg.schedules.schedules_count == 1);
    free(buf);
    CHECK(g_net_allocs == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P8 — NULL schedule: has_schedules stays false, schedules_count 0 */
static void test_f_config_export_all_null_schedule_sets_has_schedules_false(void)
{
    reset_test_state();
    reg_fan_add(0, "fan0", 50, 15);
    reg_source_add(0, "src0", SOURCE_TYPE_MANUAL, 40.0f);
    reg_curve_add(0, "curve0", 2);
    uint8_t *buf  = NULL;
    size_t len    = 0;
    esp_err_t err = f_config_export_all(H_FAN, H_SRC, H_CUR, NULL, &buf, &len);
    CHECK(err == ESP_OK);
    ConfigFile cfg;
    CHECK(decode_config(buf, len, &cfg));
    CHECK(cfg.has_schedules == false);
    CHECK(cfg.schedules.schedules_count == 0);
    CHECK(cfg.has_fans && cfg.fans.fans_count == 1);
    CHECK(cfg.has_sources && cfg.sources.sources_count == 1);
    CHECK(cfg.has_curves && cfg.curves.curves_count == 1);
    free(buf);
    CHECK(g_net_allocs == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P9 — all four registries NULL: empty lists, version still encodes */
static void test_f_config_export_all_all_null_registries_empty_lists(void)
{
    reset_test_state();
    uint8_t *buf  = NULL;
    size_t len    = 0;
    esp_err_t err = f_config_export_all(NULL, NULL, NULL, NULL, &buf, &len);
    CHECK(err == ESP_OK);
    CHECK(buf != NULL);
    CHECK(len > 0);
    ConfigFile cfg;
    CHECK(decode_config(buf, len, &cfg));
    CHECK(strcmp(cfg.version, "3.0") == 0);
    CHECK(cfg.has_fans == true && cfg.fans.fans_count == 0);
    CHECK(cfg.has_sources == true && cfg.sources.sources_count == 0);
    CHECK(cfg.has_curves == true && cfg.curves.curves_count == 0);
    CHECK(cfg.has_schedules == false && cfg.schedules.schedules_count == 0);
    free(buf);
    CHECK(g_net_allocs == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P10 — second calloc (encode buffer) fails; cfg is freed */
static void test_f_config_export_all_enc_buf_calloc_failure(void)
{
    reset_test_state();
    g_fail_calloc_on = 2;
    populate_all_registries();
    uint8_t *buf  = (uint8_t *)1;
    size_t len    = 7;
    esp_err_t err = f_config_export_all(H_FAN, H_SRC, H_CUR, H_SCH, &buf, &len);
    CHECK(err == ESP_ERR_NO_MEM);
    CHECK(buf == (uint8_t *)1);
    CHECK(len == 7);
    CHECK(g_calloc_calls == 2);
    CHECK(g_net_allocs == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P11 — pb_encode fails; both buffers freed, out-params untouched */
static void test_f_config_export_all_encode_failure_frees_buffers(void)
{
    reset_test_state();
    g_fail_pb_encode = 1;
    populate_all_registries();
    uint8_t *buf  = NULL;
    size_t len    = 0;
    esp_err_t err = f_config_export_all(H_FAN, H_SRC, H_CUR, H_SCH, &buf, &len);
    CHECK(err == ESP_FAIL);
    CHECK(buf == NULL);
    CHECK(len == 0);
    CHECK(g_pb_encode_calls == 1);
    CHECK(g_net_allocs == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P12 — partial registries: counts match used slots exactly */
static void test_f_config_export_all_partial_registries_counts_match(void)
{
    reset_test_state();
    reg_fan_add(0, "fan0", 50, 15);
    reg_fan_add(2, "fan2", 60, 17);
    reg_fan_add(5, "fan5", 70, 20);
    /* source: zero used slots */
    reg_curve_add(0, "curve0", 2);
    reg_curve_add(15, "curve15", 1);
    reg_schedule_add(1, "sch1", 2, 80, 600, 1200, false);
    uint8_t *buf  = NULL;
    size_t len    = 0;
    esp_err_t err = f_config_export_all(H_FAN, H_SRC, H_CUR, H_SCH, &buf, &len);
    CHECK(err == ESP_OK);
    ConfigFile cfg;
    CHECK(decode_config(buf, len, &cfg));
    CHECK(strcmp(cfg.version, "3.0") == 0);
    CHECK(cfg.fans.fans_count == 3);
    CHECK(cfg.sources.sources_count == 0);
    CHECK(cfg.curves.curves_count == 2);
    CHECK(cfg.schedules.schedules_count == 1);
    free(buf);
    CHECK(g_net_allocs == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* ================================================================
 * f_config_save_all tests
 * ================================================================ */

#define SAVE_DEBOUNCE_US 3000000UL

/* S1 — reject NULL handle */
static void test_f_config_save_all_rejects_null_handle(void)
{
    reset_test_state();
    esp_err_t err = f_config_save_all(NULL, H_FAN, H_SRC, H_CUR, H_SCH);
    CHECK(err == ESP_ERR_INVALID_STATE);
    CHECK(g_fopen_calls == 0);
    CHECK(g_calloc_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* S2 — reject unmounted config */
static void test_f_config_save_all_rejects_unmounted(void)
{
    reset_test_state();
    struct f_config cfg = {"", false, 0};
    esp_err_t err       = f_config_save_all(&cfg, H_FAN, H_SRC, H_CUR, H_SCH);
    CHECK(err == ESP_ERR_INVALID_STATE);
    CHECK(g_fopen_calls == 0);
    CHECK(g_calloc_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* S3 — second save within the 3 s debounce window is skipped */
static void test_f_config_save_all_debounced_returns_ok_without_write(void)
{
    reset_test_state();
    static char cfgpath[] = "/tmp/fconfig_test_config.pb";
    remove(cfgpath);
    g_config_path       = cfgpath;

    struct f_config cfg = {"", true, 0};
    const uint64_t T    = SAVE_DEBOUNCE_US * 3; /* safely past the debounce window */
    g_now_us            = T;

    esp_err_t e1        = f_config_save_all(&cfg, H_FAN, H_SRC, H_CUR, H_SCH);
    CHECK(e1 == ESP_OK);
    CHECK(g_fopen_calls == 1);
    CHECK(g_fwrite_calls == 1);
    CHECK(cfg.last_save_us == T);

    g_now_us     = T + SAVE_DEBOUNCE_US / 2; /* inside the debounce window */
    esp_err_t e2 = f_config_save_all(&cfg, H_FAN, H_SRC, H_CUR, H_SCH);
    CHECK(e2 == ESP_OK);
    CHECK(g_fopen_calls == 1); /* not opened a second time */
    CHECK(g_fwrite_calls == 1);
    CHECK(cfg.last_save_us == T); /* unchanged by the debounced call */
    CHECK(g_net_allocs == 0);
    remove(cfgpath);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* S4 — export error propagates before any file open */
static void test_f_config_save_all_propagates_export_error(void)
{
    reset_test_state();
    g_fail_calloc_on = 2; /* cfg calloc OK, enc_buf calloc fails -> ESP_ERR_NO_MEM */
    populate_all_registries();
    struct f_config cfg = {"", true, 0};
    g_now_us            = 10000000;
    esp_err_t err       = f_config_save_all(&cfg, H_FAN, H_SRC, H_CUR, H_SCH);
    CHECK(err == ESP_ERR_NO_MEM);
    CHECK(g_fopen_calls == 0);
    CHECK(g_net_allocs == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* S5 — fopen failure frees the exported buffer and returns ESP_FAIL */
static void test_f_config_save_all_fopen_failure_frees_buffer_returns_fail(void)
{
    reset_test_state();
    g_fopen_fail = 1;
    populate_all_registries();
    struct f_config cfg = {"", true, 0};
    g_now_us            = 10000000;
    esp_err_t err       = f_config_save_all(&cfg, H_FAN, H_SRC, H_CUR, H_SCH);
    CHECK(err == ESP_FAIL);
    CHECK(g_fopen_calls == 1);
    CHECK(g_net_allocs == 0); /* exported enc_buf freed on the fopen==NULL path */
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* S6 — short write: fclose called, buffer freed, returns ESP_FAIL */
static void test_f_config_save_all_short_write_returns_fail(void)
{
    reset_test_state();
    static char cfgpath[] = "/tmp/fconfig_test_config.pb";
    remove(cfgpath);
    g_config_path  = cfgpath;
    g_fwrite_short = 1;
    populate_all_registries();
    struct f_config cfg = {"", true, 0};
    g_now_us            = 10000000;
    esp_err_t err       = f_config_save_all(&cfg, H_FAN, H_SRC, H_CUR, H_SCH);
    CHECK(err == ESP_FAIL);
    CHECK(g_fopen_calls == 1);
    CHECK(g_fwrite_calls == 1);
    CHECK(g_fclose_calls == 1);
    CHECK(g_net_allocs == 0);
    CHECK(strstr(g_last_log, "Short write") != NULL);
    remove(cfgpath);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* S7 — success: file written, decodable, summary logged, no leaks */
static void test_f_config_save_all_success_writes_file_returns_ok(void)
{
    reset_test_state();
    static char cfgpath[] = "/tmp/fconfig_test_config.pb";
    remove(cfgpath);
    g_config_path = cfgpath;
    populate_all_registries();
    struct f_config cfg = {"", true, 0};
    g_now_us            = 10000000;
    esp_err_t err       = f_config_save_all(&cfg, H_FAN, H_SRC, H_CUR, H_SCH);
    CHECK(err == ESP_OK);
    CHECK(g_fopen_calls == 1);
    CHECK(g_fwrite_calls == 1);
    CHECK(g_fclose_calls == 1);
    CHECK(g_net_allocs == 0);

    /* Read the file back and decode it. */
    FILE *f = fopen(cfgpath, "rb");
    CHECK(f != NULL);
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    CHECK(fsz > 0);
    uint8_t fdata[ConfigFile_size];
    size_t rd = fread(fdata, 1, (size_t)fsz, f);
    fclose(f);
    CHECK(rd == (size_t)fsz);

    ConfigFile rcfg;
    CHECK(decode_config(fdata, (size_t)fsz, &rcfg));
    CHECK(strcmp(rcfg.version, "3.0") == 0);
    CHECK(rcfg.has_fans && rcfg.fans.fans_count == 2);
    CHECK(rcfg.has_sources && rcfg.sources.sources_count == 1);
    CHECK(rcfg.has_curves && rcfg.curves.curves_count == 2);
    CHECK(rcfg.has_schedules && rcfg.schedules.schedules_count == 1);
    CHECK(strstr(g_last_log, "Config saved as protobuf") != NULL);
    remove(cfgpath);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* S8 — export cfg-calloc failure (calloc #1) propagates through the debounced save */
static void test_f_config_save_all_cfg_calloc_failure_propagates(void)
{
    reset_test_state();
    g_fail_calloc_on = 1;
    populate_all_registries();
    struct f_config cfg = {"", true, 0};
    g_now_us            = 10000000;
    esp_err_t err       = f_config_save_all(&cfg, H_FAN, H_SRC, H_CUR, H_SCH);
    CHECK(err == ESP_ERR_NO_MEM);
    CHECK(g_fopen_calls == 0);
    CHECK(g_calloc_calls == 1);
    CHECK(g_net_allocs == 0);
    CHECK(cfg.last_save_us == 10000000); /* last_save_us set before export at L152 */
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* S9 — pb_encode failure propagates through the debounced save */
static void test_f_config_save_all_encode_failure_propagates(void)
{
    reset_test_state();
    g_fail_pb_encode = 1;
    populate_all_registries();
    struct f_config cfg = {"", true, 0};
    g_now_us            = 10000000;
    esp_err_t err       = f_config_save_all(&cfg, H_FAN, H_SRC, H_CUR, H_SCH);
    CHECK(err == ESP_FAIL);
    CHECK(g_pb_encode_calls == 1);
    CHECK(g_fopen_calls == 0);
    CHECK(g_net_allocs == 0);
    CHECK(cfg.last_save_us == 10000000); /* last_save_us set before export at L152 */
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* ================================================================
 * f_config_save_all_forced tests
 * ================================================================ */

/* F1 — forced save rejects NULL handle */
static void test_f_config_save_all_forced_rejects_null_handle(void)
{
    reset_test_state();
    esp_err_t err = f_config_save_all_forced(NULL, H_FAN, H_SRC, H_CUR, H_SCH);
    CHECK(err == ESP_ERR_INVALID_STATE);
    CHECK(g_fopen_calls == 0);
    CHECK(g_calloc_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* F2 — forced save rejects unmounted config */
static void test_f_config_save_all_forced_rejects_unmounted(void)
{
    reset_test_state();
    struct f_config cfg = {"", false, 0};
    esp_err_t err       = f_config_save_all_forced(&cfg, H_FAN, H_SRC, H_CUR, H_SCH);
    CHECK(err == ESP_ERR_INVALID_STATE);
    CHECK(g_fopen_calls == 0);
    CHECK(g_calloc_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* F3 — forced save inside the debounce window still opens + writes, last_save_us untouched */
static void test_f_config_save_all_forced_bypasses_debounce_writes(void)
{
    reset_test_state();
    static char cfgpath[] = "/tmp/fconfig_forced_test.pb";
    remove(cfgpath);
    g_config_path = cfgpath;
    populate_all_registries();
    struct f_config cfg = {"", true, 0};
    const uint64_t T    = SAVE_DEBOUNCE_US * 3;
    cfg.last_save_us    = T;
    g_now_us            = T + SAVE_DEBOUNCE_US / 2; /* inside the 3 s window */
    esp_err_t err       = f_config_save_all_forced(&cfg, H_FAN, H_SRC, H_CUR, H_SCH);
    CHECK(err == ESP_OK);
    CHECK(g_fopen_calls == 1);
    CHECK(g_fwrite_calls == 1);
    CHECK(g_fclose_calls == 1);
    CHECK(g_net_allocs == 0);
    CHECK(cfg.last_save_us == T); /* debounce bypassed, clock not perturbed */
    remove(cfgpath);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* F4a — forced save propagates export cfg-calloc (calloc #1) failure, last_save_us untouched */
static void test_f_config_save_all_forced_propagates_export_cfg_calloc_failure(void)
{
    reset_test_state();
    g_fail_calloc_on = 1;
    populate_all_registries();
    struct f_config cfg = {"", true, 0};
    g_now_us            = 10000000;
    esp_err_t err       = f_config_save_all_forced(&cfg, H_FAN, H_SRC, H_CUR, H_SCH);
    CHECK(err == ESP_ERR_NO_MEM);
    CHECK(g_fopen_calls == 0);
    CHECK(g_calloc_calls == 1);
    CHECK(g_net_allocs == 0);
    CHECK(cfg.last_save_us == 0); /* force skips L152, clock never written */
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* F4b — forced save propagates export enc-buf-calloc (calloc #2) failure, last_save_us untouched */
static void test_f_config_save_all_forced_propagates_export_enc_buf_calloc_failure(void)
{
    reset_test_state();
    g_fail_calloc_on = 2;
    populate_all_registries();
    struct f_config cfg = {"", true, 0};
    g_now_us            = 10000000;
    esp_err_t err       = f_config_save_all_forced(&cfg, H_FAN, H_SRC, H_CUR, H_SCH);
    CHECK(err == ESP_ERR_NO_MEM);
    CHECK(g_fopen_calls == 0);
    CHECK(g_calloc_calls == 2);
    CHECK(g_net_allocs == 0);
    CHECK(cfg.last_save_us == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* F4c — forced save propagates export pb_encode failure, last_save_us untouched */
static void test_f_config_save_all_forced_propagates_export_encode_failure(void)
{
    reset_test_state();
    g_fail_pb_encode = 1;
    populate_all_registries();
    struct f_config cfg = {"", true, 0};
    g_now_us            = 10000000;
    esp_err_t err       = f_config_save_all_forced(&cfg, H_FAN, H_SRC, H_CUR, H_SCH);
    CHECK(err == ESP_FAIL);
    CHECK(g_pb_encode_calls == 1);
    CHECK(g_fopen_calls == 0);
    CHECK(g_net_allocs == 0);
    CHECK(cfg.last_save_us == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* F5 — forced save fopen failure frees the exported buffer, last_save_us untouched */
static void test_f_config_save_all_forced_fopen_failure_frees_buffer(void)
{
    reset_test_state();
    g_fopen_fail = 1;
    populate_all_registries();
    struct f_config cfg = {"", true, 0};
    g_now_us            = 10000000;
    esp_err_t err       = f_config_save_all_forced(&cfg, H_FAN, H_SRC, H_CUR, H_SCH);
    CHECK(err == ESP_FAIL);
    CHECK(g_fopen_calls == 1);
    CHECK(g_net_allocs == 0); /* exported enc_buf freed on the fopen==NULL path */
    CHECK(cfg.last_save_us == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* F6 — forced save short write returns ESP_FAIL, last_save_us untouched */
static void test_f_config_save_all_forced_short_write_returns_fail(void)
{
    reset_test_state();
    static char cfgpath[] = "/tmp/fconfig_forced_test.pb";
    remove(cfgpath);
    g_config_path  = cfgpath;
    g_fwrite_short = 1;
    populate_all_registries();
    struct f_config cfg = {"", true, 0};
    g_now_us            = 10000000;
    esp_err_t err       = f_config_save_all_forced(&cfg, H_FAN, H_SRC, H_CUR, H_SCH);
    CHECK(err == ESP_FAIL);
    CHECK(g_fopen_calls == 1);
    CHECK(g_fwrite_calls == 1);
    CHECK(g_fclose_calls == 1);
    CHECK(g_net_allocs == 0);
    CHECK(strstr(g_last_log, "Short write") != NULL);
    CHECK(cfg.last_save_us == 0);
    remove(cfgpath);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* F7 — forced save success: bypasses debounce, decodable file, no leaks, last_save_us untouched */
static void test_f_config_save_all_forced_success_writes_decodable_file(void)
{
    reset_test_state();
    static char cfgpath[] = "/tmp/fconfig_forced_test.pb";
    remove(cfgpath);
    g_config_path = cfgpath;
    populate_all_registries();
    struct f_config cfg = {"", true, 0};
    g_now_us            = 10000000;
    esp_err_t err       = f_config_save_all_forced(&cfg, H_FAN, H_SRC, H_CUR, H_SCH);
    CHECK(err == ESP_OK);
    CHECK(g_fopen_calls == 1);
    CHECK(g_fwrite_calls == 1);
    CHECK(g_fclose_calls == 1);
    CHECK(g_net_allocs == 0);
    CHECK(cfg.last_save_us == 0); /* unchanged — forced save never touches the clock */

    /* Read the file back and decode it. */
    FILE *f = fopen(cfgpath, "rb");
    CHECK(f != NULL);
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    CHECK(fsz > 0);
    uint8_t fdata[ConfigFile_size];
    size_t rd = fread(fdata, 1, (size_t)fsz, f);
    fclose(f);
    CHECK(rd == (size_t)fsz);

    ConfigFile rcfg;
    CHECK(decode_config(fdata, (size_t)fsz, &rcfg));
    CHECK(strcmp(rcfg.version, "3.0") == 0);
    CHECK(rcfg.has_fans && rcfg.fans.fans_count == 2);
    CHECK(rcfg.has_sources && rcfg.sources.sources_count == 1);
    CHECK(rcfg.has_curves && rcfg.curves.curves_count == 2);
    CHECK(rcfg.has_schedules && rcfg.schedules.schedules_count == 1);
    CHECK(strstr(g_last_log, "Config saved as protobuf") != NULL);
    remove(cfgpath);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* ================================================================
 * f_config_import_all tests (spec-02 phase-2, strict validate-then-mutate)
 * ================================================================ */

/* I-P1 — NULL handle rejected, err_msg nulled, no file/registry activity */
static void test_f_config_import_all_rejects_null_handle(void)
{
    reset_test_state();
    ConfigFile cfg = ConfigFile_init_default;
    const char *em = (const char *)0x1;
    esp_err_t err  = f_config_import_all(NULL, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(em == NULL);
    CHECK(g_fopen_calls == 0);
    CHECK(g_fan_remove_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* I-P2 — unmounted handle rejected */
static void test_f_config_import_all_rejects_unmounted(void)
{
    reset_test_state();
    struct f_config h = {"", false, 0};
    ConfigFile cfg    = ConfigFile_init_default;
    const char *em    = (const char *)0x1;
    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(em == NULL);
    CHECK(g_fopen_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* I-P3 — NULL cfg rejected */
static void test_f_config_import_all_rejects_null_cfg(void)
{
    reset_test_state();
    struct f_config h = {"", true, 0};
    const char *em    = (const char *)0x1;
    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, NULL, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(em == NULL);
    CHECK(g_fopen_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* V-P1 — fans capacity overflow: count 9 > MAX 8 */
static void test_f_config_import_all_rejects_fans_overflow(void)
{
    reset_test_state();
    struct f_config h   = {"", true, 0};
    const char *em      = (const char *)0x1;
    ConfigFile cfg      = ConfigFile_init_default;
    cfg.has_fans        = true;
    cfg.fans.fans_count = 9; /* do NOT fill fans[8]; capacity guard fires first */
    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(strcmp(em, "max fans reached (8)") == 0);
    CHECK(mutation_clean());
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* V-P2 — sources capacity overflow */
static void test_f_config_import_all_rejects_sources_overflow(void)
{
    reset_test_state();
    struct f_config h         = {"", true, 0};
    const char *em            = (const char *)0x1;
    ConfigFile cfg            = ConfigFile_init_default;
    cfg.has_sources           = true;
    cfg.sources.sources_count = 9;
    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(strcmp(em, "max sources reached (8)") == 0);
    CHECK(mutation_clean());
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* V-P3 — curves capacity overflow */
static void test_f_config_import_all_rejects_curves_overflow(void)
{
    reset_test_state();
    struct f_config h       = {"", true, 0};
    const char *em          = (const char *)0x1;
    ConfigFile cfg          = ConfigFile_init_default;
    cfg.has_curves          = true;
    cfg.curves.curves_count = 17;
    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(strcmp(em, "max curves reached (16)") == 0);
    CHECK(mutation_clean());
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* V-P4 — schedules capacity overflow */
static void test_f_config_import_all_rejects_schedules_overflow(void)
{
    reset_test_state();
    struct f_config h             = {"", true, 0};
    const char *em                = (const char *)0x1;
    ConfigFile cfg                = ConfigFile_init_default;
    cfg.has_schedules             = true;
    cfg.schedules.schedules_count = 9;
    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(strcmp(em, "max schedules reached (8)") == 0);
    CHECK(mutation_clean());
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* V-P5 — exactly MAX capacity accepted */
static void test_f_config_import_all_accepts_max_capacity(void)
{
    reset_test_state();
    static char p[] = "/tmp/fconfig_import_max.pb";
    remove(p);
    g_config_path       = p;
    struct f_config h   = {"", true, 0};
    const char *em      = (const char *)0x1;

    ConfigFile cfg      = ConfigFile_init_default;
    cfg.has_fans        = true;
    cfg.fans.fans_count = 8;
    for (int i = 0; i < 8; i++) {
        FanInfo *f = &cfg.fans.fans[i];
        *f         = (FanInfo)FanInfo_init_default;
        snprintf(f->name, sizeof(f->name), "f%d", i);
        f->pwm_gpio  = 10 + i;
        f->tach_gpio = 255;
        f->mode      = 0;
        f->duty      = 50;
    }
    cfg.has_sources           = true;
    cfg.sources.sources_count = 8;
    for (int i = 0; i < 8; i++) {
        SourceInfo *s = &cfg.sources.sources[i];
        *s            = (SourceInfo)SourceInfo_init_default;
        snprintf(s->name, sizeof(s->name), "src%d", i);
        s->type = SourceType_SOURCE_TYPE_NTC;
        s->gpio = 20 + i;
    }
    cfg.has_curves          = true;
    cfg.curves.curves_count = 16;
    for (int i = 0; i < 16; i++) {
        CurveInfo *c = &cfg.curves.curves[i];
        *c           = (CurveInfo)CurveInfo_init_default;
        c->id        = i;
        snprintf(c->name, sizeof(c->name), "c%d", i);
        c->points_count     = 2;
        c->points[0].temp_c = 30.0f + i;
        c->points[0].duty   = 20;
        c->points[1].temp_c = 50.0f + i;
        c->points[1].duty   = 60;
    }
    cfg.has_schedules             = true;
    cfg.schedules.schedules_count = 8;
    for (int i = 0; i < 8; i++) {
        ScheduleInfo *s = &cfg.schedules.schedules[i];
        *s              = (ScheduleInfo)ScheduleInfo_init_default;
        s->fan_id       = i;
        s->duty         = 50;
        s->start_min    = 480;
        s->end_min      = 1080;
        s->enabled      = true;
        snprintf(s->name, sizeof(s->name), "s%d", i);
    }

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_OK);
    CHECK(em == NULL);
    CHECK(g_fan_add_calls == 8);
    CHECK(g_source_add_calls == 8);
    CHECK(g_curve_upsert_calls == 16);
    CHECK(g_schedule_add_calls == 8);
    CHECK(g_fopen_calls == 1);

    ConfigFile rcfg;
    CHECK(read_config_file(p, &rcfg));
    CHECK(rcfg.has_fans && rcfg.fans.fans_count == 8);
    CHECK(rcfg.has_sources && rcfg.sources.sources_count == 8);
    CHECK(rcfg.has_curves && rcfg.curves.curves_count == 16);
    CHECK(rcfg.has_schedules && rcfg.schedules.schedules_count == 8);
    remove(p);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* V-P6 — fan pwm_gpio out of range */
static void test_f_config_import_all_rejects_fan_pwm_gpio(void)
{
    reset_test_state();
    struct f_config h          = {"", true, 0};
    const char *em             = (const char *)0x1;
    ConfigFile cfg             = ConfigFile_init_default;
    cfg.has_fans               = true;
    cfg.fans.fans_count        = 1;
    cfg.fans.fans[0].pwm_gpio  = 255;
    cfg.fans.fans[0].tach_gpio = 255;
    cfg.fans.fans[0].mode      = 0;
    cfg.fans.fans[0].duty      = 50;
    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(strcmp(em, "GPIO must be 0-48") == 0);
    CHECK(mutation_clean());
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* V-P7 — fan tach_gpio out of range (non-FF sentinel) */
static void test_f_config_import_all_rejects_fan_tach_gpio(void)
{
    reset_test_state();
    struct f_config h          = {"", true, 0};
    const char *em             = (const char *)0x1;
    ConfigFile cfg             = ConfigFile_init_default;
    cfg.has_fans               = true;
    cfg.fans.fans_count        = 1;
    cfg.fans.fans[0].pwm_gpio  = 15;
    cfg.fans.fans[0].tach_gpio = 49;
    cfg.fans.fans[0].mode      = 0;
    cfg.fans.fans[0].duty      = 50;
    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(strcmp(em, "GPIO must be 0-48") == 0);
    CHECK(mutation_clean());
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* V-P8 — fan mode out of range */
static void test_f_config_import_all_rejects_fan_mode(void)
{
    reset_test_state();
    struct f_config h          = {"", true, 0};
    const char *em             = (const char *)0x1;
    ConfigFile cfg             = ConfigFile_init_default;
    cfg.has_fans               = true;
    cfg.fans.fans_count        = 1;
    cfg.fans.fans[0].pwm_gpio  = 15;
    cfg.fans.fans[0].tach_gpio = 255;
    cfg.fans.fans[0].mode      = 2;
    cfg.fans.fans[0].duty      = 50;
    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(strcmp(em, "mode must be 0 (manual) or 1 (auto)") == 0);
    CHECK(mutation_clean());
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* V-P9 — fan duty out of range */
static void test_f_config_import_all_rejects_fan_duty(void)
{
    reset_test_state();
    struct f_config h          = {"", true, 0};
    const char *em             = (const char *)0x1;
    ConfigFile cfg             = ConfigFile_init_default;
    cfg.has_fans               = true;
    cfg.fans.fans_count        = 1;
    cfg.fans.fans[0].pwm_gpio  = 15;
    cfg.fans.fans[0].tach_gpio = 255;
    cfg.fans.fans[0].mode      = 0;
    cfg.fans.fans[0].duty      = 101;
    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(strcmp(em, "duty must be 0-100") == 0);
    CHECK(mutation_clean());
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* V-P10 — NTC source gpio out of range */
static void test_f_config_import_all_rejects_ntc_source_gpio(void)
{
    reset_test_state();
    struct f_config h           = {"", true, 0};
    const char *em              = (const char *)0x1;
    ConfigFile cfg              = ConfigFile_init_default;
    cfg.has_sources             = true;
    cfg.sources.sources_count   = 1;
    cfg.sources.sources[0].type = SourceType_SOURCE_TYPE_NTC;
    cfg.sources.sources[0].gpio = 255;
    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(strcmp(em, "GPIO must be 0-48") == 0);
    CHECK(mutation_clean());
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* VI-1 — MANUAL source temp below min rejected before any mutation */
static void test_f_config_import_all_rejects_manual_source_temp_below_min(void)
{
    reset_test_state();
    struct f_config h         = {"", true, 0};
    const char *em            = (const char *)0x1;
    ConfigFile cfg            = ConfigFile_init_default;
    cfg.has_sources           = true;
    cfg.sources.sources_count = 1;
    SourceInfo *s0            = &cfg.sources.sources[0];
    *s0                       = (SourceInfo)SourceInfo_init_default;
    strncpy(s0->name, "man0", sizeof(s0->name) - 1);
    s0->name[sizeof(s0->name) - 1] = '\0';
    s0->type                       = SourceType_SOURCE_TYPE_MANUAL;
    s0->gpio                       = 255;
    s0->temp_c                     = -40.01f;

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(em != NULL && strcmp(em, "temp_c must be -40.0 to 125.0") == 0);
    CHECK(mutation_clean());
    CHECK(g_source_update_manual_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* VI-2 — MANUAL source temp above max rejected before any mutation */
static void test_f_config_import_all_rejects_manual_source_temp_above_max(void)
{
    reset_test_state();
    struct f_config h         = {"", true, 0};
    const char *em            = (const char *)0x1;
    ConfigFile cfg            = ConfigFile_init_default;
    cfg.has_sources           = true;
    cfg.sources.sources_count = 1;
    SourceInfo *s0            = &cfg.sources.sources[0];
    *s0                       = (SourceInfo)SourceInfo_init_default;
    strncpy(s0->name, "man0", sizeof(s0->name) - 1);
    s0->name[sizeof(s0->name) - 1] = '\0';
    s0->type                       = SourceType_SOURCE_TYPE_MANUAL;
    s0->gpio                       = 255;
    s0->temp_c                     = 125.01f;

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(em != NULL && strcmp(em, "temp_c must be -40.0 to 125.0") == 0);
    CHECK(mutation_clean());
    CHECK(g_source_update_manual_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* VI-3 — MANUAL boundary min accepted: source added, temp applied, persisted */
static void test_f_config_import_all_accepts_manual_source_temp_min_boundary(void)
{
    reset_test_state();
    static char p[] = "/tmp/fconfig_import_manmin.pb";
    remove(p);
    g_config_path             = p;
    struct f_config h         = {"", true, 0};
    const char *em            = (const char *)0x1;
    ConfigFile cfg            = ConfigFile_init_default;
    cfg.has_sources           = true;
    cfg.sources.sources_count = 1;
    SourceInfo *s0            = &cfg.sources.sources[0];
    *s0                       = (SourceInfo)SourceInfo_init_default;
    strncpy(s0->name, "man0", sizeof(s0->name) - 1);
    s0->name[sizeof(s0->name) - 1] = '\0';
    s0->type                       = SourceType_SOURCE_TYPE_MANUAL;
    s0->gpio                       = 255;
    s0->temp_c                     = -40.0f;

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_OK);
    CHECK(em == NULL);
    CHECK(g_source_add_calls == 1);
    CHECK(g_source_update_manual_calls == 1);
    CHECK(src_info[0].temp_c == -40.0f);
    CHECK(g_fopen_calls == 1);
    remove(p);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* VI-4 — MANUAL boundary max accepted: source added, temp applied, persisted */
static void test_f_config_import_all_accepts_manual_source_temp_max_boundary(void)
{
    reset_test_state();
    static char p[] = "/tmp/fconfig_import_manmax.pb";
    remove(p);
    g_config_path             = p;
    struct f_config h         = {"", true, 0};
    const char *em            = (const char *)0x1;
    ConfigFile cfg            = ConfigFile_init_default;
    cfg.has_sources           = true;
    cfg.sources.sources_count = 1;
    SourceInfo *s0            = &cfg.sources.sources[0];
    *s0                       = (SourceInfo)SourceInfo_init_default;
    strncpy(s0->name, "man0", sizeof(s0->name) - 1);
    s0->name[sizeof(s0->name) - 1] = '\0';
    s0->type                       = SourceType_SOURCE_TYPE_MANUAL;
    s0->gpio                       = 255;
    s0->temp_c                     = 125.0f;

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_OK);
    CHECK(em == NULL);
    CHECK(g_source_add_calls == 1);
    CHECK(g_source_update_manual_calls == 1);
    CHECK(src_info[0].temp_c == 125.0f);
    CHECK(g_fopen_calls == 1);
    remove(p);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* VI-5 — NTC source temp NOT validated: out-of-range temp still passes import */
static void test_f_config_import_all_ntc_source_out_of_range_temp_passes(void)
{
    reset_test_state();
    static char p[] = "/tmp/fconfig_import_ntc_ot.pb";
    remove(p);
    g_config_path             = p;
    struct f_config h         = {"", true, 0};
    const char *em            = (const char *)0x1;
    ConfigFile cfg            = ConfigFile_init_default;
    cfg.has_sources           = true;
    cfg.sources.sources_count = 1;
    SourceInfo *s0            = &cfg.sources.sources[0];
    *s0                       = (SourceInfo)SourceInfo_init_default;
    strncpy(s0->name, "ntc0", sizeof(s0->name) - 1);
    s0->name[sizeof(s0->name) - 1] = '\0';
    s0->type                       = SourceType_SOURCE_TYPE_NTC;
    s0->gpio                       = 34;
    s0->temp_c                     = 999.0f;

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_OK);
    CHECK(em == NULL);
    CHECK(g_source_add_calls == 1);
    CHECK(g_source_add_ds18b20_calls == 0);
    CHECK(g_source_update_manual_calls == 0);
    CHECK(g_fopen_calls == 1);
    remove(p);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* VI-6 — DS18B20 source temp NOT validated: out-of-range temp still passes import */
static void test_f_config_import_all_ds18b20_source_out_of_range_temp_passes(void)
{
    reset_test_state();
    static char p[] = "/tmp/fconfig_import_ds_ot.pb";
    remove(p);
    g_config_path             = p;
    struct f_config h         = {"", true, 0};
    const char *em            = (const char *)0x1;
    ConfigFile cfg            = ConfigFile_init_default;
    cfg.has_sources           = true;
    cfg.sources.sources_count = 1;
    SourceInfo *s0            = &cfg.sources.sources[0];
    *s0                       = (SourceInfo)SourceInfo_init_default;
    strncpy(s0->name, "ds0", sizeof(s0->name) - 1);
    s0->name[sizeof(s0->name) - 1] = '\0';
    s0->type                       = SourceType_SOURCE_TYPE_DS18B20;
    s0->ds18b20_rom_code           = 0x28ABCDULL;
    s0->temp_c                     = -999.0f;

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_OK);
    CHECK(em == NULL);
    CHECK(g_source_add_ds18b20_calls == 1);
    CHECK(g_source_update_manual_calls == 0);
    CHECK(g_fopen_calls == 1);
    remove(p);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* VI-7 — unknown enum maps to MANUAL: out-of-range temp rejected before mutation */
static void test_f_config_import_all_rejects_unknown_type_out_of_range_temp(void)
{
    reset_test_state();
    struct f_config h         = {"", true, 0};
    const char *em            = (const char *)0x1;
    ConfigFile cfg            = ConfigFile_init_default;
    cfg.has_sources           = true;
    cfg.sources.sources_count = 1;
    SourceInfo *s0            = &cfg.sources.sources[0];
    *s0                       = (SourceInfo)SourceInfo_init_default;
    strncpy(s0->name, "unk0", sizeof(s0->name) - 1);
    s0->name[sizeof(s0->name) - 1] = '\0';
    s0->type                       = (SourceType)5;
    s0->gpio                       = 255;
    s0->temp_c                     = 200.0f;

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(em != NULL && strcmp(em, "temp_c must be -40.0 to 125.0") == 0);
    CHECK(mutation_clean());
    CHECK(g_source_add_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* V-P11a — curve too few points */
static void test_f_config_import_all_rejects_curve_too_few_points(void)
{
    reset_test_state();
    struct f_config h                     = {"", true, 0};
    const char *em                        = (const char *)0x1;
    ConfigFile cfg                        = ConfigFile_init_default;
    cfg.has_curves                        = true;
    cfg.curves.curves_count               = 1;
    cfg.curves.curves[0].points_count     = 1;
    cfg.curves.curves[0].points[0].temp_c = 30.0f;
    cfg.curves.curves[0].points[0].duty   = 20;
    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(strcmp(em, "curve must have 2-16 points") == 0);
    CHECK(mutation_clean());
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* V-P11b — curve too many points (guard fires before the duty loop) */
static void test_f_config_import_all_rejects_curve_too_many_points(void)
{
    reset_test_state();
    struct f_config h                 = {"", true, 0};
    const char *em                    = (const char *)0x1;
    ConfigFile cfg                    = ConfigFile_init_default;
    cfg.has_curves                    = true;
    cfg.curves.curves_count           = 1;
    cfg.curves.curves[0].points_count = 11;
    for (int j = 0; j < 10; j++) {
        cfg.curves.curves[0].points[j].temp_c = 30.0f + j;
        cfg.curves.curves[0].points[j].duty   = 20;
    }
    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(strcmp(em, "curve must have 2-16 points") == 0);
    CHECK(mutation_clean());
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* V-P12 — curve points not ascending */
static void test_f_config_import_all_rejects_curve_unsorted(void)
{
    reset_test_state();
    struct f_config h                     = {"", true, 0};
    const char *em                        = (const char *)0x1;
    ConfigFile cfg                        = ConfigFile_init_default;
    cfg.has_curves                        = true;
    cfg.curves.curves_count               = 1;
    cfg.curves.curves[0].points_count     = 2;
    cfg.curves.curves[0].points[0].temp_c = 50.0f;
    cfg.curves.curves[0].points[0].duty   = 20;
    cfg.curves.curves[0].points[1].temp_c = 30.0f;
    cfg.curves.curves[0].points[1].duty   = 20;
    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(strcmp(em, "curve points must be sorted by temp_c ascending") == 0);
    CHECK(mutation_clean());
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* V-P13 — curve point duty out of range */
static void test_f_config_import_all_rejects_curve_point_duty(void)
{
    reset_test_state();
    struct f_config h                     = {"", true, 0};
    const char *em                        = (const char *)0x1;
    ConfigFile cfg                        = ConfigFile_init_default;
    cfg.has_curves                        = true;
    cfg.curves.curves_count               = 1;
    cfg.curves.curves[0].points_count     = 2;
    cfg.curves.curves[0].points[0].temp_c = 30.0f;
    cfg.curves.curves[0].points[0].duty   = 20;
    cfg.curves.curves[0].points[1].temp_c = 50.0f;
    cfg.curves.curves[0].points[1].duty   = 150;
    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(strcmp(em, "duty must be 0-100") == 0);
    CHECK(mutation_clean());
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* V-P14 — schedule time out of range */
static void test_f_config_import_all_rejects_schedule_time(void)
{
    reset_test_state();
    struct f_config h                    = {"", true, 0};
    const char *em                       = (const char *)0x1;
    ConfigFile cfg                       = ConfigFile_init_default;
    cfg.has_fans                         = true;
    cfg.fans.fans_count                  = 1;
    cfg.fans.fans[0].pwm_gpio            = 15;
    cfg.fans.fans[0].tach_gpio           = 255;
    cfg.fans.fans[0].mode                = 0;
    cfg.fans.fans[0].duty                = 50;
    cfg.fans.fans[0].source_id           = 255; /* unbound: M3 cross-ref check */
    cfg.fans.fans[0].curve_id            = 255;
    cfg.fans.fans[0].schedule_id         = 255;
    cfg.has_schedules                    = true;
    cfg.schedules.schedules_count        = 1;
    cfg.schedules.schedules[0].start_min = 1500;
    cfg.schedules.schedules[0].end_min   = 1080;
    cfg.schedules.schedules[0].duty      = 50;
    cfg.schedules.schedules[0].fan_id    = 0;
    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(strcmp(em, "start_min/end_min must be 0-1439") == 0);
    CHECK(mutation_clean());
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* V-P15 — schedule duty out of range */
static void test_f_config_import_all_rejects_schedule_duty(void)
{
    reset_test_state();
    struct f_config h                    = {"", true, 0};
    const char *em                       = (const char *)0x1;
    ConfigFile cfg                       = ConfigFile_init_default;
    cfg.has_fans                         = true;
    cfg.fans.fans_count                  = 1;
    cfg.fans.fans[0].pwm_gpio            = 15;
    cfg.fans.fans[0].tach_gpio           = 255;
    cfg.fans.fans[0].mode                = 0;
    cfg.fans.fans[0].duty                = 50;
    cfg.fans.fans[0].source_id           = 255; /* unbound: M3 cross-ref check */
    cfg.fans.fans[0].curve_id            = 255;
    cfg.fans.fans[0].schedule_id         = 255;
    cfg.has_schedules                    = true;
    cfg.schedules.schedules_count        = 1;
    cfg.schedules.schedules[0].start_min = 480;
    cfg.schedules.schedules[0].end_min   = 1080;
    cfg.schedules.schedules[0].duty      = 200;
    cfg.schedules.schedules[0].fan_id    = 0;
    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(strcmp(em, "duty must be 0-100") == 0);
    CHECK(mutation_clean());
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* V-P16 — schedule references missing fan */
static void test_f_config_import_all_rejects_schedule_missing_fan(void)
{
    reset_test_state();
    struct f_config h   = {"", true, 0};
    const char *em      = (const char *)0x1;
    ConfigFile cfg      = ConfigFile_init_default;
    cfg.has_fans        = true;
    cfg.fans.fans_count = 2;
    for (int i = 0; i < 2; i++) {
        cfg.fans.fans[i].pwm_gpio    = 15 + i;
        cfg.fans.fans[i].tach_gpio   = 255;
        cfg.fans.fans[i].mode        = 0;
        cfg.fans.fans[i].duty        = 50;
        cfg.fans.fans[i].source_id   = 255; /* unbound: M3 cross-ref check */
        cfg.fans.fans[i].curve_id    = 255;
        cfg.fans.fans[i].schedule_id = 255;
    }
    cfg.has_schedules                    = true;
    cfg.schedules.schedules_count        = 1;
    cfg.schedules.schedules[0].fan_id    = 5;
    cfg.schedules.schedules[0].start_min = 480;
    cfg.schedules.schedules[0].end_min   = 1080;
    cfg.schedules.schedules[0].duty      = 50;
    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(strcmp(em, "schedule references non-existent fan") == 0);
    CHECK(mutation_clean());
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* V-P17 — fan references non-existent source (M3 cross-ref validation) */
static void test_f_config_import_all_rejects_fan_missing_source(void)
{
    reset_test_state();
    struct f_config h = {"", true, 0};
    const char *em    = (const char *)0x1;
    ConfigFile cfg;
    cfg_single_fan(&cfg, 1, 255, 255); /* source_id=1 but 0 sources present */
    cfg.has_sources           = true;
    cfg.sources.sources_count = 1; /* only source slot 0 exists, id 1 is out of range */

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(strcmp(em, "fan references non-existent source") == 0);
    CHECK(mutation_clean());
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* V-P18 — fan references non-existent curve (M3 cross-ref validation) */
static void test_f_config_import_all_rejects_fan_missing_curve(void)
{
    reset_test_state();
    struct f_config h = {"", true, 0};
    const char *em    = (const char *)0x1;
    ConfigFile cfg;
    cfg_single_fan(&cfg, 255, 1, 255); /* curve_id=1 but 0 curves present */

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(strcmp(em, "fan references non-existent curve") == 0);
    CHECK(mutation_clean());
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* V-P19 — fan references non-existent schedule (M3 cross-ref validation) */
static void test_f_config_import_all_rejects_fan_missing_schedule(void)
{
    reset_test_state();
    struct f_config h = {"", true, 0};
    const char *em    = (const char *)0x1;
    ConfigFile cfg;
    cfg_single_fan(&cfg, 255, 255, 1); /* schedule_id=1 but 0 schedules present */

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(strcmp(em, "fan references non-existent schedule") == 0);
    CHECK(mutation_clean());
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* V-P20 — fan pwm_gpio == tach_gpio rejected (within-fan duplicate guard) */
static void test_f_config_import_all_rejects_pwm_tach_same_gpio(void)
{
    reset_test_state();
    struct f_config h   = {"", true, 0};
    const char *em      = (const char *)0x1;
    ConfigFile cfg      = ConfigFile_init_default;
    cfg.has_fans        = true;
    cfg.fans.fans_count = 1;
    FanInfo *f          = &cfg.fans.fans[0];
    *f                  = (FanInfo)FanInfo_init_default;
    strncpy(f->name, "f", sizeof(f->name) - 1);
    f->name[sizeof(f->name) - 1] = '\0';
    f->pwm_gpio                  = 15;
    f->tach_gpio                 = 15; /* same pin */
    f->source_id                 = 255;
    f->curve_id                  = 255;
    f->schedule_id               = 255;
    f->mode                      = 0;
    f->duty                      = 50;

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(strcmp(em, "pwm and tach cannot use the same GPIO") == 0);
    CHECK(mutation_clean());
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* V-P21 — two devices claiming the same GPIO in one file rejected (pin_used) */
static void test_f_config_import_all_rejects_duplicate_gpio_across_devices(void)
{
    reset_test_state();
    struct f_config h   = {"", true, 0};
    const char *em      = (const char *)0x1;
    ConfigFile cfg      = ConfigFile_init_default;
    cfg.has_fans        = true;
    cfg.fans.fans_count = 2;
    for (int i = 0; i < 2; i++) {
        FanInfo *f = &cfg.fans.fans[i];
        *f         = (FanInfo)FanInfo_init_default;
        snprintf(f->name, sizeof(f->name), "f%d", i);
        f->pwm_gpio    = 15; /* BOTH fans on pin 15 */
        f->tach_gpio   = 255;
        f->source_id   = 255;
        f->curve_id    = 255;
        f->schedule_id = 255;
        f->mode        = 0;
        f->duty        = 50;
    }

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(strcmp(em, "GPIO used by multiple devices") == 0);
    CHECK(mutation_clean());
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* V-P22 — live-gpio reserved pin rejected (f_gpio registry, non-NULL handle) */
static void test_f_config_import_all_rejects_live_reserved_gpio(void)
{
    reset_test_state();
    struct f_config h = {"", true, 0};
    const char *em    = (const char *)0x1;
    ConfigFile cfg;
    cfg_single_fan(&cfg, 255, 255, 255); /* fan pwm on GPIO 15 */
    cfg.fans.fans[0].pwm_gpio = 15;
    g_reserved_gpio[0]        = 15; /* live registry marks GPIO 15 reserved */

    esp_err_t err = f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH,
                                        (f_gpio_handle_t)0x1, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(strcmp(em, "GPIO is reserved") == 0);
    CHECK(mutation_clean());
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* V-P23 — live-gpio active DS18B20 bus pin rejected (ONEWIRE claim) */
static void test_f_config_import_all_rejects_live_onewire_gpio(void)
{
    reset_test_state();
    struct f_config h = {"", true, 0};
    const char *em    = (const char *)0x1;
    ConfigFile cfg;
    cfg_single_fan(&cfg, 255, 255, 255); /* fan pwm on GPIO 15 */
    cfg.fans.fans[0].pwm_gpio = 15;
    g_caps_override[15]       = F_GPIO_CAP_ONEWIRE; /* live registry: active 1-Wire bus pin */

    esp_err_t err = f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH,
                                        (f_gpio_handle_t)0x1, &cfg, &em);
    CHECK(err == ESP_ERR_INVALID_ARG);
    CHECK(strcmp(em, "GPIO is the active DS18B20 bus pin") == 0);
    CHECK(mutation_clean());
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* V-P24 — live-gpio PWM claim on a valid pin is ACCEPTED (PWM caps not a
 * hard rejection — the clear releases PWM/TACH/ADC first, so a re-import of
 * the same pins must stay valid). */
static void test_f_config_import_all_accepts_live_pwm_claimed_gpio(void)
{
    reset_test_state();
    static char p[] = "/tmp/fconfig_import_livepwm.pb";
    remove(p);
    g_config_path     = p;
    struct f_config h = {"", true, 0};
    const char *em    = (const char *)0x1;
    ConfigFile cfg;
    cfg_single_fan(&cfg, 255, 255, 255); /* fan pwm on GPIO 15 */
    cfg.fans.fans[0].pwm_gpio = 15;
    g_caps_override[15]       = F_GPIO_CAP_PWM; /* live registry: PWM-claimed, still OK */

    esp_err_t err = f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH,
                                        (f_gpio_handle_t)0x1, &cfg, &em);
    CHECK(err == ESP_OK);
    CHECK(em == NULL);
    CHECK(g_fan_add_calls == 1);
    CHECK(g_fan_set_enabled_calls == 1); /* M2: enabled flag restored in apply */
    CHECK(g_fopen_calls == 1);
    remove(p);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* C-P1 — clear tolerates empty config and NULL registry handles */
static void test_f_config_import_all_clear_tolerates_empty_and_null_handles(void)
{
    reset_test_state();
    static char p[] = "/tmp/fconfig_import_clear.pb";
    remove(p);
    g_config_path     = p;
    struct f_config h = {"", true, 0};
    const char *em    = (const char *)0x1;
    ConfigFile cfg    = ConfigFile_init_default;

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, NULL, NULL, NULL, NULL, NULL, &cfg, &em);
    CHECK(err == ESP_OK);
    CHECK(em == NULL);
    CHECK(g_schedule_remove_calls == F_SCHEDULE_MAX_COUNT);
    CHECK(g_fan_remove_calls == F_FAN_MAX_COUNT);
    CHECK(g_curve_remove_calls == F_CURVE_MAX_COUNT);
    CHECK(g_source_remove_calls == F_SOURCE_MAX_COUNT);
    CHECK(strncmp(g_clear_seq, "SSSSSSSSFFFFFFFFCCCCCCCCCCCCCCCCRRRRRRRR", 40) == 0);
    CHECK(g_fan_add_calls == 0);
    CHECK(g_source_add_calls == 0);
    CHECK(g_curve_upsert_calls == 0);
    CHECK(g_schedule_add_calls == 0);
    CHECK(g_fopen_calls == 1);

    ConfigFile rcfg;
    CHECK(read_config_file(p, &rcfg));
    CHECK(rcfg.has_fans && rcfg.fans.fans_count == 0);
    CHECK(rcfg.has_sources && rcfg.sources.sources_count == 0);
    CHECK(rcfg.has_curves && rcfg.curves.curves_count == 0);
    CHECK(!rcfg.has_schedules || rcfg.schedules.schedules_count == 0);
    remove(p);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* H-P1 — full happy path across all four registries */
static void test_f_config_import_all_happy_path_all_registries(void)
{
    reset_test_state();
    static char p[] = "/tmp/fconfig_import_happy.pb";
    remove(p);
    g_config_path     = p;
    struct f_config h = {"", true, 0};
    const char *em    = (const char *)0x1;
    ConfigFile cfg;
    build_happy_cfg(&cfg);

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_OK);
    CHECK(em == NULL);
    CHECK(g_schedule_remove_calls == F_SCHEDULE_MAX_COUNT);
    CHECK(g_fan_remove_calls == F_FAN_MAX_COUNT);
    CHECK(g_curve_remove_calls == F_CURVE_MAX_COUNT);
    CHECK(g_source_remove_calls == F_SOURCE_MAX_COUNT);
    CHECK(strncmp(g_clear_seq, "SSSSSSSSFFFFFFFFCCCCCCCCCCCCCCCCRRRRRRRR", 40) == 0);
    CHECK(g_fan_add_calls == 1);
    CHECK(g_fan_set_mode_calls == 1);
    CHECK(g_fan_set_duty_calls == 1);
    CHECK(g_fan_set_group_calls == 1);
    CHECK(g_fan_set_inverted_calls == 1);
    CHECK(g_fan_set_enabled_calls == 1);
    CHECK(g_fan_set_source_calls == 1);
    CHECK(g_fan_set_curve_calls == 1);
    CHECK(g_fan_set_schedule_calls == 1);
    CHECK(g_source_add_ds18b20_calls == 1);
    CHECK(g_source_add_calls == 2);
    CHECK(g_source_update_manual_calls == 1);
    CHECK(g_curve_upsert_calls == 1);
    CHECK(g_schedule_add_calls == 1);
    CHECK(g_fopen_calls == 1);
    CHECK(g_fwrite_calls == 1);
    CHECK(g_fclose_calls == 1);
    CHECK(g_net_allocs == 0);
    CHECK(h.last_save_us == 0);
    CHECK(fan_used[0] == true);

    ConfigFile rcfg;
    CHECK(read_config_file(p, &rcfg));
    CHECK(rcfg.has_fans && rcfg.fans.fans_count == 1);
    CHECK(rcfg.has_sources && rcfg.sources.sources_count == 3);
    CHECK(rcfg.has_curves && rcfg.curves.curves_count == 1);
    CHECK(rcfg.has_schedules && rcfg.schedules.schedules_count == 1);
    CHECK(strstr(g_last_log, "Config saved as protobuf") != NULL);
    remove(p);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* H-P2 — empty config clears all pre-populated registries and persists an empty file */
static void test_f_config_import_all_empty_config_clears_all_registries(void)
{
    reset_test_state();
    static char p[] = "/tmp/fconfig_import_clear2.pb";
    remove(p);
    g_config_path     = p;
    struct f_config h = {"", true, 0};
    const char *em    = (const char *)0x1;

    reg_fan_add(0, "fan0", 50, 15);
    reg_source_add(0, "src0", SOURCE_TYPE_MANUAL, 40.0f);
    reg_curve_add(0, "curve0", 2);
    reg_schedule_add(0, "sch0", 1, 50, 480, 1080, true);

    ConfigFile cfg = ConfigFile_init_default;

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_OK);
    CHECK(em == NULL);
    CHECK(g_schedule_remove_calls == F_SCHEDULE_MAX_COUNT);
    CHECK(g_fan_remove_calls == F_FAN_MAX_COUNT);
    CHECK(g_curve_remove_calls == F_CURVE_MAX_COUNT);
    CHECK(g_source_remove_calls == F_SOURCE_MAX_COUNT);
    CHECK(strncmp(g_clear_seq, "SSSSSSSSFFFFFFFFCCCCCCCCCCCCCCCCRRRRRRRR", 40) == 0);
    CHECK(g_fan_add_calls == 0);
    CHECK(g_source_add_calls == 0);
    CHECK(g_curve_upsert_calls == 0);
    CHECK(g_schedule_add_calls == 0);
    CHECK(g_fopen_calls == 1);

    f_fan_info_t fi;
    f_source_info_t si;
    f_curve_info_t ci;
    f_schedule_info_t schi;
    CHECK(f_fan_get_info(H_FAN, 0, &fi) == ESP_ERR_NOT_FOUND);
    CHECK(f_source_get_info(H_SRC, 0, &si) == ESP_ERR_NOT_FOUND);
    CHECK(f_curve_get_info(H_CUR, 0, &ci) == ESP_ERR_NOT_FOUND);
    CHECK(f_schedule_get_info(H_SCH, 0, &schi) == ESP_ERR_NOT_FOUND);

    ConfigFile rcfg;
    CHECK(read_config_file(p, &rcfg));
    CHECK(rcfg.has_fans && rcfg.fans.fans_count == 0);
    CHECK(rcfg.has_sources && rcfg.sources.sources_count == 0);
    CHECK(rcfg.has_curves && rcfg.curves.curves_count == 0);
    CHECK(!rcfg.has_schedules || rcfg.schedules.schedules_count == 0);
    remove(p);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* H-P3 — happy path with err_msg == NULL */
static void test_f_config_import_all_happy_path_null_err_msg(void)
{
    reset_test_state();
    static char p[] = "/tmp/fconfig_import_nullerr.pb";
    remove(p);
    g_config_path     = p;
    struct f_config h = {"", true, 0};
    ConfigFile cfg;
    build_happy_cfg(&cfg);

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, NULL);
    CHECK(err == ESP_OK);
    CHECK(g_fopen_calls == 1);
    CHECK(g_fan_add_calls == 1);
    CHECK(g_net_allocs == 0);

    ConfigFile rcfg;
    CHECK(read_config_file(p, &rcfg));
    CHECK(rcfg.has_fans && rcfg.fans.fans_count == 1);
    CHECK(rcfg.has_sources && rcfg.sources.sources_count == 3);
    CHECK(rcfg.has_curves && rcfg.curves.curves_count == 1);
    CHECK(rcfg.has_schedules && rcfg.schedules.schedules_count == 1);
    remove(p);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* A-P2 — fan binding setters skipped when ids are 0xFF */
static void test_f_config_import_all_apply_skips_fan_bindings_when_ff(void)
{
    reset_test_state();
    static char p[] = "/tmp/fconfig_import_ff.pb";
    remove(p);
    g_config_path     = p;
    struct f_config h = {"", true, 0};
    const char *em    = (const char *)0x1;
    ConfigFile cfg;
    cfg_single_fan(&cfg, 255, 255, 255);

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_OK);
    CHECK(em == NULL);
    CHECK(g_fan_add_calls == 1);
    CHECK(g_fan_set_mode_calls == 1);
    CHECK(g_fan_set_duty_calls == 1);
    CHECK(g_fan_set_group_calls == 1);
    CHECK(g_fan_set_inverted_calls == 1);
    CHECK(g_fan_set_enabled_calls == 1);
    CHECK(g_fan_set_source_calls == 0);
    CHECK(g_fan_set_curve_calls == 0);
    CHECK(g_fan_set_schedule_calls == 0);
    CHECK(g_fopen_calls == 1);
    remove(p);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* A-P3 — MANUAL source with temp_c == 0 skips update_manual */
static void test_f_config_import_all_apply_skips_manual_update_when_temp_zero(void)
{
    reset_test_state();
    static char p[] = "/tmp/fconfig_import_man0.pb";
    remove(p);
    g_config_path             = p;
    struct f_config h         = {"", true, 0};
    const char *em            = (const char *)0x1;
    ConfigFile cfg            = ConfigFile_init_default;
    cfg.has_sources           = true;
    cfg.sources.sources_count = 1;
    SourceInfo *s0            = &cfg.sources.sources[0];
    *s0                       = (SourceInfo)SourceInfo_init_default;
    strncpy(s0->name, "man0", sizeof(s0->name) - 1);
    s0->name[sizeof(s0->name) - 1] = '\0';
    s0->type                       = SourceType_SOURCE_TYPE_MANUAL;
    s0->gpio                       = 255;
    s0->temp_c                     = 0.0f;

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_OK);
    CHECK(em == NULL);
    CHECK(g_source_add_calls == 1);
    CHECK(g_source_update_manual_calls == 0);
    CHECK(g_fopen_calls == 1);
    remove(p);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* A-P4 — unknown source type value treated as MANUAL */
static void test_f_config_import_all_apply_unknown_source_type_as_manual(void)
{
    reset_test_state();
    static char p[] = "/tmp/fconfig_import_unk.pb";
    remove(p);
    g_config_path             = p;
    struct f_config h         = {"", true, 0};
    const char *em            = (const char *)0x1;
    ConfigFile cfg            = ConfigFile_init_default;
    cfg.has_sources           = true;
    cfg.sources.sources_count = 1;
    SourceInfo *s0            = &cfg.sources.sources[0];
    *s0                       = (SourceInfo)SourceInfo_init_default;
    strncpy(s0->name, "unk0", sizeof(s0->name) - 1);
    s0->name[sizeof(s0->name) - 1] = '\0';
    s0->type                       = (SourceType)5;
    s0->gpio                       = 255;
    s0->temp_c                     = 0.0f;

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_OK);
    CHECK(em == NULL);
    CHECK(g_source_add_ds18b20_calls == 0);
    CHECK(g_source_add_calls == 1);
    CHECK(g_source_update_manual_calls == 0);
    CHECK(g_fopen_calls == 1);
    remove(p);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* A-ERR-FAN-ADD — apply fails at f_fan_add, clear already ran */
static void test_f_config_import_all_apply_fan_add_fails(void)
{
    reset_test_state();
    struct f_config h = {"", true, 0};
    const char *em    = (const char *)0x1;
    g_fail_fan_add    = 1;
    ConfigFile cfg;
    cfg_single_fan(&cfg, 255, 255, 255);

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_FAIL); /* apply failures normalize to ESP_FAIL (not INVALID_ARG) */
    CHECK(em != NULL && strcmp(em, "config apply failed") == 0);
    CHECK(strstr(g_last_log, "import apply failed") != NULL);
    /* Clear ran twice: once before apply, once after the failed apply (S1
     * re-clear safety net so the registries are empty-but-consistent). */
    CHECK(g_fan_remove_calls == 2 * F_FAN_MAX_COUNT);
    CHECK(g_fan_add_calls == 1);
    CHECK(g_fopen_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* A-ERR-FAN-SET-MODE */
static void test_f_config_import_all_apply_fan_set_mode_fails(void)
{
    reset_test_state();
    struct f_config h   = {"", true, 0};
    const char *em      = (const char *)0x1;
    g_fail_fan_set_mode = 1;
    ConfigFile cfg;
    cfg_single_fan(&cfg, 255, 255, 255);

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_FAIL); /* apply failures normalize to ESP_FAIL (not INVALID_ARG) */
    CHECK(em != NULL && strcmp(em, "config apply failed") == 0);
    CHECK(strstr(g_last_log, "import apply failed") != NULL);
    CHECK(g_fan_add_calls == 1);
    CHECK(g_fan_set_mode_calls == 1);
    CHECK(g_fopen_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* A-ERR-FAN-SET-DUTY */
static void test_f_config_import_all_apply_fan_set_duty_fails(void)
{
    reset_test_state();
    struct f_config h   = {"", true, 0};
    const char *em      = (const char *)0x1;
    g_fail_fan_set_duty = 1;
    ConfigFile cfg;
    cfg_single_fan(&cfg, 255, 255, 255);

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_FAIL); /* apply failures normalize to ESP_FAIL (not INVALID_ARG) */
    CHECK(em != NULL && strcmp(em, "config apply failed") == 0);
    CHECK(strstr(g_last_log, "import apply failed") != NULL);
    CHECK(g_fan_add_calls == 1);
    CHECK(g_fan_set_duty_calls == 1);
    CHECK(g_fopen_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* A-ERR-FAN-SET-GROUP */
static void test_f_config_import_all_apply_fan_set_group_fails(void)
{
    reset_test_state();
    struct f_config h    = {"", true, 0};
    const char *em       = (const char *)0x1;
    g_fail_fan_set_group = 1;
    ConfigFile cfg;
    cfg_single_fan(&cfg, 255, 255, 255);

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_FAIL); /* apply failures normalize to ESP_FAIL (not INVALID_ARG) */
    CHECK(em != NULL && strcmp(em, "config apply failed") == 0);
    CHECK(strstr(g_last_log, "import apply failed") != NULL);
    CHECK(g_fan_add_calls == 1);
    CHECK(g_fan_set_group_calls == 1);
    CHECK(g_fopen_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* A-ERR-FAN-SET-INVERTED */
static void test_f_config_import_all_apply_fan_set_inverted_fails(void)
{
    reset_test_state();
    struct f_config h       = {"", true, 0};
    const char *em          = (const char *)0x1;
    g_fail_fan_set_inverted = 1;
    ConfigFile cfg;
    cfg_single_fan(&cfg, 255, 255, 255);

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_FAIL); /* apply failures normalize to ESP_FAIL (not INVALID_ARG) */
    CHECK(em != NULL && strcmp(em, "config apply failed") == 0);
    CHECK(strstr(g_last_log, "import apply failed") != NULL);
    CHECK(g_fan_add_calls == 1);
    CHECK(g_fan_set_inverted_calls == 1);
    CHECK(g_fopen_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* A-ERR-FAN-SET-SOURCE */
static void test_f_config_import_all_apply_fan_set_source_fails(void)
{
    reset_test_state();
    struct f_config h     = {"", true, 0};
    const char *em        = (const char *)0x1;
    g_fail_fan_set_source = 1;
    ConfigFile cfg;
    cfg_single_fan(&cfg, 0, 255, 255);
    /* fan source_id=0 must resolve (M3 cross-ref validation) before apply is reached */
    cfg.has_sources               = true;
    cfg.sources.sources_count     = 1;
    cfg.sources.sources[0].type   = SourceType_SOURCE_TYPE_MANUAL;
    cfg.sources.sources[0].gpio   = 255;
    cfg.sources.sources[0].temp_c = 25.0f;

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_FAIL); /* apply failures normalize to ESP_FAIL (not INVALID_ARG) */
    CHECK(em != NULL && strcmp(em, "config apply failed") == 0);
    CHECK(strstr(g_last_log, "import apply failed") != NULL);
    CHECK(g_fan_set_source_calls == 1);
    CHECK(g_fopen_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* A-ERR-FAN-SET-CURVE */
static void test_f_config_import_all_apply_fan_set_curve_fails(void)
{
    reset_test_state();
    struct f_config h    = {"", true, 0};
    const char *em       = (const char *)0x1;
    g_fail_fan_set_curve = 1;
    ConfigFile cfg;
    cfg_single_fan(&cfg, 255, 0, 255);
    /* fan curve_id=0 must resolve (M3 cross-ref validation) before apply is reached */
    cfg.has_curves                        = true;
    cfg.curves.curves_count               = 1;
    cfg.curves.curves[0].points_count     = 2;
    cfg.curves.curves[0].points[0].temp_c = 30.0f;
    cfg.curves.curves[0].points[0].duty   = 20;
    cfg.curves.curves[0].points[1].temp_c = 50.0f;
    cfg.curves.curves[0].points[1].duty   = 60;

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_FAIL); /* apply failures normalize to ESP_FAIL (not INVALID_ARG) */
    CHECK(em != NULL && strcmp(em, "config apply failed") == 0);
    CHECK(strstr(g_last_log, "import apply failed") != NULL);
    CHECK(g_fan_set_curve_calls == 1);
    CHECK(g_fopen_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* A-ERR-FAN-SET-SCHEDULE */
static void test_f_config_import_all_apply_fan_set_schedule_fails(void)
{
    reset_test_state();
    struct f_config h       = {"", true, 0};
    const char *em          = (const char *)0x1;
    g_fail_fan_set_schedule = 1;
    ConfigFile cfg;
    cfg_single_fan(&cfg, 255, 255, 0);
    /* fan schedule_id=0 must resolve (M3 cross-ref validation) before apply is reached */
    cfg.has_schedules                    = true;
    cfg.schedules.schedules_count        = 1;
    cfg.schedules.schedules[0].fan_id    = 0;
    cfg.schedules.schedules[0].duty      = 50;
    cfg.schedules.schedules[0].start_min = 480;
    cfg.schedules.schedules[0].end_min   = 1080;
    cfg.schedules.schedules[0].enabled   = true;

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_FAIL); /* apply failures normalize to ESP_FAIL (not INVALID_ARG) */
    CHECK(em != NULL && strcmp(em, "config apply failed") == 0);
    CHECK(strstr(g_last_log, "import apply failed") != NULL);
    CHECK(g_fan_set_schedule_calls == 1);
    CHECK(g_fopen_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* A-ERR-SRC-ADD-DS18B20 */
static void test_f_config_import_all_apply_source_add_ds18b20_fails(void)
{
    reset_test_state();
    struct f_config h                       = {"", true, 0};
    const char *em                          = (const char *)0x1;
    g_fail_source_add_ds18b20               = 1;
    ConfigFile cfg                          = ConfigFile_init_default;
    cfg.has_sources                         = true;
    cfg.sources.sources_count               = 1;
    cfg.sources.sources[0].type             = SourceType_SOURCE_TYPE_DS18B20;
    cfg.sources.sources[0].ds18b20_rom_code = 0x28ABCDULL;

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_FAIL); /* apply failures normalize to ESP_FAIL (not INVALID_ARG) */
    CHECK(em != NULL && strcmp(em, "config apply failed") == 0);
    CHECK(strstr(g_last_log, "import apply failed") != NULL);
    CHECK(g_source_add_ds18b20_calls == 1);
    CHECK(g_fopen_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* A-ERR-SRC-ADD */
static void test_f_config_import_all_apply_source_add_fails(void)
{
    reset_test_state();
    struct f_config h           = {"", true, 0};
    const char *em              = (const char *)0x1;
    g_fail_source_add           = 1;
    ConfigFile cfg              = ConfigFile_init_default;
    cfg.has_sources             = true;
    cfg.sources.sources_count   = 1;
    cfg.sources.sources[0].type = SourceType_SOURCE_TYPE_NTC;
    cfg.sources.sources[0].gpio = 34;

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_FAIL); /* apply failures normalize to ESP_FAIL (not INVALID_ARG) */
    CHECK(em != NULL && strcmp(em, "config apply failed") == 0);
    CHECK(strstr(g_last_log, "import apply failed") != NULL);
    CHECK(g_source_add_calls == 1);
    CHECK(g_fopen_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* A-ERR-SRC-UPDATE-MANUAL */
static void test_f_config_import_all_apply_source_update_manual_fails(void)
{
    reset_test_state();
    struct f_config h             = {"", true, 0};
    const char *em                = (const char *)0x1;
    g_fail_source_update_manual   = 1;
    ConfigFile cfg                = ConfigFile_init_default;
    cfg.has_sources               = true;
    cfg.sources.sources_count     = 1;
    cfg.sources.sources[0].type   = SourceType_SOURCE_TYPE_MANUAL;
    cfg.sources.sources[0].gpio   = 255;
    cfg.sources.sources[0].temp_c = 25.5f;

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_FAIL); /* apply failures normalize to ESP_FAIL (not INVALID_ARG) */
    CHECK(em != NULL && strcmp(em, "config apply failed") == 0);
    CHECK(strstr(g_last_log, "import apply failed") != NULL);
    CHECK(g_source_add_calls == 1);
    CHECK(g_source_update_manual_calls == 1);
    CHECK(g_fopen_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* A-ERR-CURVE-UPSERT */
static void test_f_config_import_all_apply_curve_upsert_fails(void)
{
    reset_test_state();
    struct f_config h                     = {"", true, 0};
    const char *em                        = (const char *)0x1;
    g_fail_curve_upsert                   = 1;
    ConfigFile cfg                        = ConfigFile_init_default;
    cfg.has_curves                        = true;
    cfg.curves.curves_count               = 1;
    cfg.curves.curves[0].points_count     = 2;
    cfg.curves.curves[0].points[0].temp_c = 30.0f;
    cfg.curves.curves[0].points[0].duty   = 20;
    cfg.curves.curves[0].points[1].temp_c = 50.0f;
    cfg.curves.curves[0].points[1].duty   = 60;

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_FAIL); /* apply failures normalize to ESP_FAIL (not INVALID_ARG) */
    CHECK(em != NULL && strcmp(em, "config apply failed") == 0);
    CHECK(strstr(g_last_log, "import apply failed") != NULL);
    CHECK(g_curve_upsert_calls == 1);
    CHECK(g_fopen_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* A-ERR-SCHED-ADD */
static void test_f_config_import_all_apply_schedule_add_fails(void)
{
    reset_test_state();
    struct f_config h   = {"", true, 0};
    const char *em      = (const char *)0x1;
    g_fail_schedule_add = 1;
    ConfigFile cfg;
    cfg_single_fan(&cfg, 255, 255, 255);
    cfg.has_schedules                    = true;
    cfg.schedules.schedules_count        = 1;
    cfg.schedules.schedules[0].fan_id    = 0;
    cfg.schedules.schedules[0].duty      = 50;
    cfg.schedules.schedules[0].start_min = 480;
    cfg.schedules.schedules[0].end_min   = 1080;
    cfg.schedules.schedules[0].enabled   = true;

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_FAIL); /* apply failures normalize to ESP_FAIL (not INVALID_ARG) */
    CHECK(em != NULL && strcmp(em, "config apply failed") == 0);
    CHECK(strstr(g_last_log, "import apply failed") != NULL);
    CHECK(g_schedule_add_calls == 1);
    CHECK(g_fopen_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P-ERR-1 — persist fopen fails after a successful apply */
static void test_f_config_import_all_persist_fopen_fails(void)
{
    reset_test_state();
    struct f_config h = {"", true, 0};
    const char *em    = (const char *)0x1;
    g_fopen_fail      = 1;
    ConfigFile cfg;
    build_happy_cfg(&cfg);

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_FAIL);
    CHECK(em == NULL);
    CHECK(strstr(g_last_log, "import apply failed") == NULL);
    CHECK(g_fopen_calls == 1);
    CHECK(g_fan_add_calls == 1);
    CHECK(g_fan_remove_calls == F_FAN_MAX_COUNT);
    CHECK(fan_used[0] == true);
    CHECK(g_net_allocs == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P-ERR-2 — persist export cfg-calloc fails after a successful apply */
static void test_f_config_import_all_persist_export_calloc_fails(void)
{
    reset_test_state();
    struct f_config h = {"", true, 0};
    const char *em    = (const char *)0x1;
    g_fail_calloc_on  = 1;
    ConfigFile cfg;
    build_happy_cfg(&cfg);

    esp_err_t err =
        f_config_import_all((f_config_handle_t)&h, H_FAN, H_SRC, H_CUR, H_SCH, NULL, &cfg, &em);
    CHECK(err == ESP_ERR_NO_MEM);
    CHECK(em == NULL);
    CHECK(strstr(g_last_log, "import apply failed") == NULL);
    CHECK(g_fopen_calls == 0);
    CHECK(g_calloc_calls == 1);
    CHECK(fan_used[0] == true);
    CHECK(g_net_allocs == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* ================================================================
 * main
 * ================================================================ */

int main(void)
{
    test_f_config_export_all_rejects_null_buf_out();
    test_f_config_export_all_rejects_null_len_out();
    test_f_config_export_all_cfg_calloc_failure();
    test_f_config_export_all_success_decodes_version_and_counts();
    test_f_config_export_all_null_fan_leaves_has_fans_true_empty();
    test_f_config_export_all_null_source_leaves_has_sources_true_empty();
    test_f_config_export_all_null_curve_leaves_has_curves_true_empty();
    test_f_config_export_all_null_schedule_sets_has_schedules_false();
    test_f_config_export_all_all_null_registries_empty_lists();
    test_f_config_export_all_enc_buf_calloc_failure();
    test_f_config_export_all_encode_failure_frees_buffers();
    test_f_config_export_all_partial_registries_counts_match();

    test_f_config_save_all_rejects_null_handle();
    test_f_config_save_all_rejects_unmounted();
    test_f_config_save_all_debounced_returns_ok_without_write();
    test_f_config_save_all_propagates_export_error();
    test_f_config_save_all_fopen_failure_frees_buffer_returns_fail();
    test_f_config_save_all_short_write_returns_fail();
    test_f_config_save_all_success_writes_file_returns_ok();
    test_f_config_save_all_cfg_calloc_failure_propagates();
    test_f_config_save_all_encode_failure_propagates();

    test_f_config_save_all_forced_rejects_null_handle();
    test_f_config_save_all_forced_rejects_unmounted();
    test_f_config_save_all_forced_bypasses_debounce_writes();
    test_f_config_save_all_forced_propagates_export_cfg_calloc_failure();
    test_f_config_save_all_forced_propagates_export_enc_buf_calloc_failure();
    test_f_config_save_all_forced_propagates_export_encode_failure();
    test_f_config_save_all_forced_fopen_failure_frees_buffer();
    test_f_config_save_all_forced_short_write_returns_fail();
    test_f_config_save_all_forced_success_writes_decodable_file();

    test_f_config_import_all_rejects_null_handle();
    test_f_config_import_all_rejects_unmounted();
    test_f_config_import_all_rejects_null_cfg();
    test_f_config_import_all_rejects_fans_overflow();
    test_f_config_import_all_rejects_sources_overflow();
    test_f_config_import_all_rejects_curves_overflow();
    test_f_config_import_all_rejects_schedules_overflow();
    test_f_config_import_all_accepts_max_capacity();
    test_f_config_import_all_rejects_fan_pwm_gpio();
    test_f_config_import_all_rejects_fan_tach_gpio();
    test_f_config_import_all_rejects_fan_mode();
    test_f_config_import_all_rejects_fan_duty();
    test_f_config_import_all_rejects_ntc_source_gpio();
    test_f_config_import_all_rejects_manual_source_temp_below_min();    /* VI-1 */
    test_f_config_import_all_rejects_manual_source_temp_above_max();    /* VI-2 */
    test_f_config_import_all_accepts_manual_source_temp_min_boundary(); /* VI-3 */
    test_f_config_import_all_accepts_manual_source_temp_max_boundary(); /* VI-4 */
    test_f_config_import_all_ntc_source_out_of_range_temp_passes();     /* VI-5 */
    test_f_config_import_all_ds18b20_source_out_of_range_temp_passes(); /* VI-6 */
    test_f_config_import_all_rejects_unknown_type_out_of_range_temp();  /* VI-7 */
    test_f_config_import_all_rejects_curve_too_few_points();
    test_f_config_import_all_rejects_curve_too_many_points();
    test_f_config_import_all_rejects_curve_unsorted();
    test_f_config_import_all_rejects_curve_point_duty();
    test_f_config_import_all_rejects_schedule_time();
    test_f_config_import_all_rejects_schedule_duty();
    test_f_config_import_all_rejects_schedule_missing_fan();
    test_f_config_import_all_rejects_fan_missing_source();
    test_f_config_import_all_rejects_fan_missing_curve();
    test_f_config_import_all_rejects_fan_missing_schedule();
    test_f_config_import_all_rejects_pwm_tach_same_gpio();
    test_f_config_import_all_rejects_duplicate_gpio_across_devices();
    test_f_config_import_all_rejects_live_reserved_gpio();
    test_f_config_import_all_rejects_live_onewire_gpio();
    test_f_config_import_all_accepts_live_pwm_claimed_gpio();
    test_f_config_import_all_clear_tolerates_empty_and_null_handles();
    test_f_config_import_all_happy_path_all_registries();
    test_f_config_import_all_empty_config_clears_all_registries();
    test_f_config_import_all_happy_path_null_err_msg();
    test_f_config_import_all_apply_skips_fan_bindings_when_ff();
    test_f_config_import_all_apply_skips_manual_update_when_temp_zero();
    test_f_config_import_all_apply_unknown_source_type_as_manual();
    test_f_config_import_all_apply_fan_add_fails();
    test_f_config_import_all_apply_fan_set_mode_fails();
    test_f_config_import_all_apply_fan_set_duty_fails();
    test_f_config_import_all_apply_fan_set_group_fails();
    test_f_config_import_all_apply_fan_set_inverted_fails();
    test_f_config_import_all_apply_fan_set_source_fails();
    test_f_config_import_all_apply_fan_set_curve_fails();
    test_f_config_import_all_apply_fan_set_schedule_fails();
    test_f_config_import_all_apply_source_add_ds18b20_fails();
    test_f_config_import_all_apply_source_add_fails();
    test_f_config_import_all_apply_source_update_manual_fails();
    test_f_config_import_all_apply_curve_upsert_fails();
    test_f_config_import_all_apply_schedule_add_fails();
    test_f_config_import_all_persist_fopen_fails();
    test_f_config_import_all_persist_export_calloc_fails();

    printf("\nRESULT: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
