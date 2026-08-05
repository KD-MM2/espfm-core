/*
 * Host-based C unit tests for f_control_get_tunables and the _ctrl_callback
 * alarm-write integration (components/f_control/f_control.c, ctrl phases 2-5).
 *
 * The REAL f_control.c is compiled with `static` demoted so _ctrl_callback and
 * the `struct f_control` definition are externally reachable, against stubbed
 * ESP-IDF / FreeRTOS layers (see stubs/) plus the real f_control / f_core /
 * f_fan / f_source / f_curve / f_gpio / f_ds18b20 headers.  GNU ld --wrap
 * hooks intercept calloc/free and every external dependency the control-loop
 * callback touches: f_fan_update_rpm, f_fan_get_info, f_fan_set_alarm,
 * f_fan_set_duty, f_source_get_reading, f_curve_lookup and esp_event_post so
 * each EPA execution path can be driven deterministically.
 * -ffunction-sections/--gc-sections drop _ctrl_task / f_control_start (the
 * FreeRTOS task lifecycle), so no real scheduler or task bodies are needed.
 *
 * Built and executed by build_and_run.sh under WSL.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_event.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "f_core.h"
#include "f_control.h"

/* The (gc'd) task lifecycle uses ESP_ERROR_CHECK nowhere, but keep a no-op so
 * the whole TU compiles on host regardless of transitive header usage. */
#ifndef ESP_ERROR_CHECK
#define ESP_ERROR_CHECK(x) \
    do {                   \
        (void)(x);         \
    } while (0)
#endif

/* ================================================================
 * Real source under test, with `static` demoted.  All the headers the source
 * includes are #pragma-once guarded and already included above, so this only
 * demotes the source's own static functions/variables and exposes the struct.
 * ================================================================ */
#define static
#include "../../components/f_control/f_control.c"
#undef static

/* ================================================================
 * --wrap target declarations (resolved by GNU ld --wrap)
 * ================================================================ */

extern void *__real_calloc(size_t n, size_t s);
extern void __real_free(void *p);

/* f_core.h declares `extern esp_event_base_t ESPFM_EVENT` (ESP_EVENT_DECLARE_BASE);
 * the callback passes it to the wrapped esp_event_post, so define it here. */
esp_event_base_t ESPFM_EVENT = (esp_event_base_t)0xE;

/* ================================================================
 * Test-hook globals (controlled per test)
 * ================================================================ */

static int g_update_rpm_calls;  /* f_fan_update_rpm wrap */
static int g_get_info_calls;    /* f_fan_get_info wrap */
static f_fan_info_t g_fan_info; /* info returned by f_fan_get_info */

static int g_set_alarm_calls; /* f_fan_set_alarm wrap */
static uint8_t g_alarm_id;
static fan_alarm_t g_alarm_val;

static int g_set_duty_calls; /* f_fan_set_duty wrap */
static uint8_t g_set_duty_val;

static esp_err_t g_src_err; /* f_source_get_reading wrap */
static source_status_t g_src_status;
static float g_src_temp;

static esp_err_t g_curve_err; /* f_curve_lookup wrap */
static uint8_t g_curve_duty;

static uint32_t g_event_posted; /* esp_event_post wrap */
static int g_event_post_calls;

static int g_calloc_calls; /* wrapped allocation tracking */
static int g_net_allocs;

static int g_log_calls;     /* __test_log invocations */
static int g_log_err_calls; /* __test_log invocations at level 'E' */

/* ================================================================
 * Log capture (stub esp_log.h -> __test_log)
 * ================================================================ */

void __test_log(char level, const char *tag, const char *fmt, ...)
{
    va_list ap;
    (void)tag;
    g_log_calls++;
    if (level == 'E') g_log_err_calls++;
    va_start(ap, fmt);
    va_end(ap);
}

/* ================================================================
 * FreeRTOS task no-op definitions (declared in stubs/freertos/task.h).
 * Only reachable from _ctrl_task / f_control_start, which --gc-sections
 * drops because no test references them; definitions keep the TU self-
 * contained if the sections are ever retained.
 * ================================================================ */

BaseType_t xTaskCreate(void (*task_func)(void *), const char *name, uint32_t stack_size,
                       void *param, uint32_t priority, TaskHandle_t *task_handle)
{
    (void)task_func;
    (void)name;
    (void)stack_size;
    (void)param;
    (void)priority;
    if (task_handle) *task_handle = NULL;
    return pdPASS;
}

void vTaskDelete(void *task)
{
    (void)task;
}

void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
}

/* ================================================================
 * --wrap implementations
 * ================================================================ */

void *__wrap_calloc(size_t n, size_t s)
{
    g_calloc_calls++;
    void *p = __real_calloc(n, s);
    if (p) g_net_allocs++;
    return p;
}

void __wrap_free(void *p)
{
    if (p) g_net_allocs--;
    __real_free(p);
}

esp_err_t __wrap_f_fan_update_rpm(f_fan_handle_t h, uint8_t id)
{
    (void)h;
    (void)id;
    g_update_rpm_calls++;
    return ESP_OK;
}

esp_err_t __wrap_f_fan_get_info(f_fan_handle_t h, uint8_t id, f_fan_info_t *o)
{
    (void)h;
    (void)id;
    g_get_info_calls++;
    if (o) *o = g_fan_info;
    return ESP_OK;
}

esp_err_t __wrap_f_fan_set_alarm(f_fan_handle_t h, uint8_t id, fan_alarm_t a)
{
    (void)h;
    g_set_alarm_calls++;
    g_alarm_id  = id;
    g_alarm_val = a;
    return ESP_OK;
}

esp_err_t __wrap_f_fan_set_duty(f_fan_handle_t h, uint8_t id, uint8_t d)
{
    (void)h;
    (void)id;
    g_set_duty_calls++;
    g_set_duty_val = d;
    return ESP_OK;
}

esp_err_t __wrap_f_source_get_reading(f_source_handle_t h, uint8_t id, float *t, source_status_t *s)
{
    (void)h;
    (void)id;
    if (t) *t = g_src_temp;
    if (s) *s = g_src_status;
    return g_src_err;
}

esp_err_t __wrap_f_curve_lookup(f_curve_handle_t h, uint8_t id, float t, uint8_t *d)
{
    (void)h;
    (void)id;
    (void)t;
    if (d) *d = g_curve_duty;
    return g_curve_err;
}

esp_err_t __wrap_esp_event_post(esp_event_base_t b, int32_t e, void *d, size_t sz, uint32_t t)
{
    (void)b;
    (void)d;
    (void)sz;
    (void)t;
    g_event_post_calls++;
    g_event_posted = (uint32_t)e;
    return ESP_OK;
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

/* Zero every test-hook global so each test starts from a clean slate. */
static void reset_test_state(void)
{
    g_update_rpm_calls = 0;
    g_get_info_calls   = 0;
    memset(&g_fan_info, 0, sizeof(g_fan_info));

    g_set_alarm_calls  = 0;
    g_alarm_id         = 0;
    g_alarm_val        = FAN_ALARM_NONE;

    g_set_duty_calls   = 0;
    g_set_duty_val     = 0;

    g_src_err          = ESP_OK;
    g_src_status       = SOURCE_STATUS_VALID;
    g_src_temp         = 0.0f;

    g_curve_err        = ESP_OK;
    g_curve_duty       = 0;

    g_event_posted     = 0;
    g_event_post_calls = 0;

    g_calloc_calls     = 0;
    g_net_allocs       = 0;

    g_log_calls        = 0;
    g_log_err_calls    = 0;
}

/* Create a fresh control handle with known tunables: hysteresis=5,
 * ramp_up=15, ramp_down=20, policy=FAILSAFE_HOLD, safe_duty=70. */
static void setup_ctrl(f_control_handle_t *out)
{
    f_control_handle_t c = NULL;
    CHECK(f_control_init(&c, (f_fan_handle_t)0x1, (f_source_handle_t)0x2, (f_curve_handle_t)0x3) ==
          ESP_OK);
    CHECK(f_control_set_hysteresis(c, 5) == ESP_OK);
    CHECK(f_control_set_ramp_rates(c, 15, 20) == ESP_OK);
    CHECK(f_control_set_failsafe(c, FAILSAFE_HOLD, 70) == ESP_OK);
    *out = c;
}

/* Create a control handle plus a zeroed fan info for a direct _ctrl_callback
 * invocation (id 0, enabled).  Per-test code sets mode and the wrap hooks. */
static void setup_callback(f_control_handle_t *out, f_fan_info_t *info)
{
    setup_ctrl(out);
    memset(info, 0, sizeof(*info));
    info->id      = 0;
    info->enabled = true;
}

/* ================================================================
 * f_control_get_tunables tests (P1-P7)
 * ================================================================ */

/* P1 — NULL handle rejected; none of the five out-pointers written. */
static void test_get_tunables_null_handle_returns_invalid_arg(void)
{
    reset_test_state();
    f_control_handle_t c;
    setup_ctrl(&c);
    uint8_t h = 0xAA, u = 0xAA, d = 0xAA, s = 0xAA;
    failsafe_policy_t p = (failsafe_policy_t)0xAA;
    CHECK(f_control_get_tunables(NULL, &h, &u, &d, &p, &s) == ESP_ERR_INVALID_ARG);
    CHECK(h == 0xAA && u == 0xAA && d == 0xAA && p == (failsafe_policy_t)0xAA && s == 0xAA);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P2 — NULL hysteresis_pct rejected; the other out-pointers NOT written. */
static void test_get_tunables_null_hysteresis_out_returns_invalid_arg(void)
{
    reset_test_state();
    f_control_handle_t c;
    setup_ctrl(&c);
    uint8_t u = 0xAA, d = 0xAA, s = 0xAA;
    failsafe_policy_t p = (failsafe_policy_t)0xAA;
    CHECK(f_control_get_tunables(c, NULL, &u, &d, &p, &s) == ESP_ERR_INVALID_ARG);
    CHECK(u == 0xAA && d == 0xAA && p == (failsafe_policy_t)0xAA && s == 0xAA);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P3 — NULL ramp_up_pct rejected. */
static void test_get_tunables_null_ramp_up_out_returns_invalid_arg(void)
{
    reset_test_state();
    f_control_handle_t c;
    setup_ctrl(&c);
    uint8_t h, d, s;
    failsafe_policy_t p;
    CHECK(f_control_get_tunables(c, &h, NULL, &d, &p, &s) == ESP_ERR_INVALID_ARG);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P4 — NULL ramp_down_pct rejected. */
static void test_get_tunables_null_ramp_down_out_returns_invalid_arg(void)
{
    reset_test_state();
    f_control_handle_t c;
    setup_ctrl(&c);
    uint8_t h, u, s;
    failsafe_policy_t p;
    CHECK(f_control_get_tunables(c, &h, &u, NULL, &p, &s) == ESP_ERR_INVALID_ARG);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P5 — NULL failsafe_policy rejected. */
static void test_get_tunables_null_policy_out_returns_invalid_arg(void)
{
    reset_test_state();
    f_control_handle_t c;
    setup_ctrl(&c);
    uint8_t h, u, d, s;
    CHECK(f_control_get_tunables(c, &h, &u, &d, NULL, &s) == ESP_ERR_INVALID_ARG);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P6 — NULL failsafe_safe_duty rejected. */
static void test_get_tunables_null_safe_duty_out_returns_invalid_arg(void)
{
    reset_test_state();
    f_control_handle_t c;
    setup_ctrl(&c);
    uint8_t h, u, d;
    failsafe_policy_t p;
    CHECK(f_control_get_tunables(c, &h, &u, &d, &p, NULL) == ESP_ERR_INVALID_ARG);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P7 — happy path: all five fields copied from the struct. */
static void test_get_tunables_success_copies_all_five_fields(void)
{
    reset_test_state();
    f_control_handle_t c;
    setup_ctrl(&c);
    uint8_t h, u, d, s;
    failsafe_policy_t p;
    CHECK(f_control_get_tunables(c, &h, &u, &d, &p, &s) == ESP_OK);
    CHECK(h == 5);
    CHECK(u == 15);
    CHECK(d == 20);
    CHECK(p == FAILSAFE_HOLD);
    CHECK(s == 70);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* ================================================================
 * _ctrl_callback alarm-write tests (CT1-CT6)
 * ================================================================ */

/* CT1 — MANUAL + stall (duty>0, rpm==0, counter reaches 3) writes STALL to the
 * registry and posts FAN_ALARM on transition from NONE. */
static void test_ctrl_callback_manual_stall_writes_stall_alarm(void)
{
    reset_test_state();
    f_control_handle_t ctrl;
    f_fan_info_t info;
    setup_callback(&ctrl, &info);
    info.mode              = FAN_MODE_MANUAL;
    g_fan_info.duty        = 100;
    g_fan_info.rpm         = 0;
    ctrl->stall_counter[0] = 2;
    ctrl->prev_alarm[0]    = FAN_ALARM_NONE;

    _ctrl_callback(&info, ctrl);

    CHECK(g_set_alarm_calls == 1);
    CHECK(g_alarm_id == 0);
    CHECK(g_alarm_val == FAN_ALARM_STALL);
    CHECK(ctrl->prev_alarm[0] == FAN_ALARM_STALL);
    CHECK(g_event_posted == (uint32_t)ESPFM_EVENT_FAN_ALARM);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* CT2 — MANUAL + no stall writes NONE, clearing a previous STALL alarm and
 * posting FAN_ALARM_CLEAR. */
static void test_ctrl_callback_manual_clear_writes_none(void)
{
    reset_test_state();
    f_control_handle_t ctrl;
    f_fan_info_t info;
    setup_callback(&ctrl, &info);
    info.mode              = FAN_MODE_MANUAL;
    g_fan_info.duty        = 0;
    g_fan_info.rpm         = 0;
    ctrl->stall_counter[0] = 0;
    ctrl->prev_alarm[0]    = FAN_ALARM_STALL;

    _ctrl_callback(&info, ctrl);

    CHECK(g_set_alarm_calls == 1);
    CHECK(g_alarm_val == FAN_ALARM_NONE);
    CHECK(ctrl->prev_alarm[0] == FAN_ALARM_NONE);
    CHECK(g_event_posted == (uint32_t)ESPFM_EVENT_FAN_ALARM_CLEAR);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* CT3 — AUTO + source read failure triggers fail-safe FULL_SPEED duty, posts
 * SOURCE_INVALID, and writes the alarm (NONE) at the check_alarm label. */
static void test_ctrl_callback_auto_source_invalid_writes_at_check_alarm(void)
{
    reset_test_state();
    f_control_handle_t ctrl;
    f_fan_info_t info;
    setup_callback(&ctrl, &info);
    info.mode             = FAN_MODE_AUTO;
    ctrl->failsafe_policy = FAILSAFE_FULL_SPEED;
    g_src_err             = ESP_FAIL;
    g_fan_info.duty       = 0;
    ctrl->prev_alarm[0]   = FAN_ALARM_NONE;

    _ctrl_callback(&info, ctrl);

    CHECK(g_set_duty_calls == 1);
    CHECK(g_set_duty_val == 100);
    CHECK(g_event_posted == (uint32_t)ESPFM_EVENT_SOURCE_INVALID);
    CHECK(g_set_alarm_calls == 1);
    CHECK(g_alarm_val == FAN_ALARM_NONE);
    CHECK(ctrl->prev_alarm[0] == FAN_ALARM_NONE);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* CT4 — AUTO + curve lookup failure jumps to check_alarm (alarm NONE) with no
 * duty write. */
static void test_ctrl_callback_auto_curve_fail_writes_at_check_alarm(void)
{
    reset_test_state();
    f_control_handle_t ctrl;
    f_fan_info_t info;
    setup_callback(&ctrl, &info);
    info.mode           = FAN_MODE_AUTO;
    g_src_err           = ESP_OK;
    g_src_status        = SOURCE_STATUS_VALID;
    g_src_temp          = 30.0f;
    g_curve_err         = ESP_ERR_NOT_FOUND;
    ctrl->prev_alarm[0] = FAN_ALARM_NONE;

    _ctrl_callback(&info, ctrl);

    CHECK(g_set_alarm_calls == 1);
    CHECK(g_alarm_val == FAN_ALARM_NONE);
    CHECK(g_set_duty_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* CT5 — AUTO + source valid + over-temp writes OVERTEMP and posts FAN_ALARM on
 * transition from NONE. */
static void test_ctrl_callback_auto_overtemp_writes_overtemp(void)
{
    reset_test_state();
    f_control_handle_t ctrl;
    f_fan_info_t info;
    setup_callback(&ctrl, &info);
    info.mode           = FAN_MODE_AUTO;
    g_src_err           = ESP_OK;
    g_src_status        = SOURCE_STATUS_VALID;
    g_src_temp          = 90.0f; /* > CONFIG_ESPFM_OVERTEMP_THRESHOLD_C=60 */
    g_curve_err         = ESP_OK;
    g_curve_duty        = 80;
    ctrl->prev_duty[0]  = 0;
    ctrl->prev_alarm[0] = FAN_ALARM_NONE;

    _ctrl_callback(&info, ctrl);

    CHECK(g_set_alarm_calls == 1);
    CHECK(g_alarm_val == FAN_ALARM_OVERTEMP);
    CHECK(ctrl->prev_alarm[0] == FAN_ALARM_OVERTEMP);
    CHECK(g_event_posted == (uint32_t)ESPFM_EVENT_FAN_ALARM);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* CT6 — AUTO + normal temp writes NONE, clearing a previous OVERTEMP alarm and
 * posting FAN_ALARM_CLEAR. */
static void test_ctrl_callback_auto_normal_writes_none(void)
{
    reset_test_state();
    f_control_handle_t ctrl;
    f_fan_info_t info;
    setup_callback(&ctrl, &info);
    info.mode           = FAN_MODE_AUTO;
    g_src_err           = ESP_OK;
    g_src_status        = SOURCE_STATUS_VALID;
    g_src_temp          = 30.0f; /* <= threshold */
    g_curve_err         = ESP_OK;
    g_curve_duty        = 50;
    ctrl->prev_duty[0]  = 0;
    ctrl->prev_alarm[0] = FAN_ALARM_OVERTEMP;

    _ctrl_callback(&info, ctrl);

    CHECK(g_set_alarm_calls == 1);
    CHECK(g_alarm_val == FAN_ALARM_NONE);
    CHECK(ctrl->prev_alarm[0] == FAN_ALARM_NONE);
    CHECK(g_event_posted == (uint32_t)ESPFM_EVENT_FAN_ALARM_CLEAR);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* ================================================================
 * main
 * ================================================================ */

int main(void)
{
    /* f_control_get_tunables */
    test_get_tunables_null_handle_returns_invalid_arg();         /* P1 */
    test_get_tunables_null_hysteresis_out_returns_invalid_arg(); /* P2 */
    test_get_tunables_null_ramp_up_out_returns_invalid_arg();    /* P3 */
    test_get_tunables_null_ramp_down_out_returns_invalid_arg();  /* P4 */
    test_get_tunables_null_policy_out_returns_invalid_arg();     /* P5 */
    test_get_tunables_null_safe_duty_out_returns_invalid_arg();  /* P6 */
    test_get_tunables_success_copies_all_five_fields();          /* P7 */

    /* _ctrl_callback alarm-write */
    test_ctrl_callback_manual_stall_writes_stall_alarm();           /* CT1 */
    test_ctrl_callback_manual_clear_writes_none();                  /* CT2 */
    test_ctrl_callback_auto_source_invalid_writes_at_check_alarm(); /* CT3 */
    test_ctrl_callback_auto_curve_fail_writes_at_check_alarm();     /* CT4 */
    test_ctrl_callback_auto_overtemp_writes_overtemp();             /* CT5 */
    test_ctrl_callback_auto_normal_writes_none();                   /* CT6 */

    printf("\nRESULT: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
