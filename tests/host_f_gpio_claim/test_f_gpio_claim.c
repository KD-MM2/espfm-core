/*
 * Host-based C unit tests for the f_gpio claim-wiring feature (gpio phases 2-5):
 * cross-peripheral pin-conflict rejection in f_fan / f_source and the ONEWIRE
 * bus-level claim in f_ds18b20_init.
 *
 * The REAL f_fan.c, f_source.c, f_gpio.c, f_ds18b20.c and f_constraints.c are
 * compiled against stubbed ESP-IDF layers (see stubs/) with -DCONFIG_IDF_TARGET_ESP32
 * (so the ESP32 reserved-pin table {1,3,6,7,8,9,10,11,16,17} and F_GPIO_MAX_PINS 40
 * are active).  GNU ld --wrap hooks intercept calloc/free, f_ledc_add_channel,
 * f_ledc_remove_channel, f_pcnt_add_input, f_pcnt_remove_input, esp_timer_get_time,
 * onewire_new_bus_rmt, onewire_bus_del, esp_err_to_name and f_gpio_claim (a
 * pass-through with fail-on-Nth-call injection so the provably-unreachable
 * post-alloc claim failures FS-P19 / FS-P20 can be driven).  FreeRTOS mutex and
 * the onewire/ds18b20 scan APIs are provided as no-op stubs; -ffunction-sections
 * / --gc-sections drop the unreferenced driver functions at link time.
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

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "onewire_bus.h"

#include "f_core.h"
#include "f_gpio.h"
#include "f_ledc.h"
#include "f_pcnt.h"
#include "f_adc.h"
#include "f_fan.h"
#include "f_source.h"
#include "f_ds18b20.h"
#include "f_constraints.h"

/* ================================================================
 * --wrap target declarations (resolved by GNU ld --wrap)
 * ================================================================ */

extern void *__real_calloc(size_t n, size_t s);
extern void __real_free(void *p);
extern esp_err_t __real_f_gpio_claim(f_gpio_handle_t handle, uint8_t pin, uint32_t cap_mask);

/* ================================================================
 * Test-hook globals (controlled per test)
 * ================================================================ */

static f_gpio_handle_t g_gpio;      /* real registry, fresh per reset_test_state */
static f_fan_handle_t g_fan;        /* real fan registry, fresh per reset_test_state */
static f_source_handle_t g_src;     /* real source registry, fresh per reset_test_state */
static f_ds18b20_handle_t g_ds_ref; /* pointer storage passed to f_source_init */

static f_ledc_handle_t g_ledc; /* opaque fake LEDC handle (never dereferenced) */
static f_pcnt_handle_t g_pcnt; /* opaque fake PCNT handle (never dereferenced) */

static int g_fail_calloc_on; /* 0 = never fail; N = fail the Nth calloc call */
static int g_calloc_calls;
static int g_net_allocs; /* live wrapped allocations (calloc - free) */

static int g_fail_gpio_claim_on; /* 0 = never fail; N = fail the Nth f_gpio_claim call */
static int g_gpio_claim_calls;

static int g_ledc_add_calls;
static int g_ledc_remove_calls;
static int g_pcnt_add_calls;
static int g_pcnt_remove_calls;
static int g_bus_del_calls; /* onewire_bus_del invocations */
static uint64_t g_now_us;   /* clock returned by esp_timer_get_time */

static int g_log_calls;
static int g_log_err_calls; /* __test_log invocations at level 'E' */
static char g_last_log[256];

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
    vsnprintf(g_last_log, sizeof(g_last_log), fmt, ap);
    va_end(ap);
}

/* ================================================================
 * FreeRTOS mutex stubs (real sources link against these)
 * ================================================================ */

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void)
{
    return (SemaphoreHandle_t)0x1234; /* non-NULL so init succeeds */
}

BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t xMutex, TickType_t xBlockTime)
{
    (void)xMutex;
    (void)xBlockTime;
    return 1;
}

BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t xMutex)
{
    (void)xMutex;
    return 1;
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

esp_err_t __wrap_f_gpio_claim(f_gpio_handle_t handle, uint8_t pin, uint32_t cap_mask)
{
    g_gpio_claim_calls++;
    if (g_fail_gpio_claim_on > 0 && g_gpio_claim_calls == g_fail_gpio_claim_on) {
        /* Simulate a pin claimed by another task between pre-validation and the
         * actual claim — the only way the post-alloc claim paths (FS-P19/P20)
         * can fail on the real f_gpio. */
        return ESP_ERR_INVALID_STATE;
    }
    return __real_f_gpio_claim(handle, pin, cap_mask);
}

esp_err_t __wrap_f_ledc_add_channel(f_ledc_handle_t handle, uint8_t gpio, uint8_t *channel_id_out)
{
    (void)handle;
    (void)gpio;
    g_ledc_add_calls++;
    if (channel_id_out) *channel_id_out = 0;
    return ESP_OK;
}

esp_err_t __wrap_f_ledc_remove_channel(f_ledc_handle_t handle, uint8_t channel_id)
{
    (void)handle;
    (void)channel_id;
    g_ledc_remove_calls++;
    return ESP_OK;
}

esp_err_t __wrap_f_pcnt_add_input(f_pcnt_handle_t handle, uint8_t gpio, uint8_t *unit_id_out)
{
    (void)handle;
    (void)gpio;
    g_pcnt_add_calls++;
    if (unit_id_out) *unit_id_out = 0;
    return ESP_OK;
}

esp_err_t __wrap_f_pcnt_remove_input(f_pcnt_handle_t handle, uint8_t unit_id)
{
    (void)handle;
    (void)unit_id;
    g_pcnt_remove_calls++;
    return ESP_OK;
}

uint64_t __wrap_esp_timer_get_time(void)
{
    return g_now_us;
}

esp_err_t __wrap_onewire_new_bus_rmt(const onewire_bus_config_t *bus_config,
                                     const onewire_bus_rmt_config_t *rmt_config,
                                     onewire_bus_handle_t *ret_bus)
{
    (void)bus_config;
    (void)rmt_config;
    if (ret_bus) *ret_bus = (onewire_bus_handle_t)0xBEEF;
    return ESP_OK;
}

esp_err_t __wrap_onewire_bus_del(onewire_bus_handle_t bus)
{
    (void)bus;
    g_bus_del_calls++;
    return ESP_OK;
}

const char *__wrap_esp_err_to_name(esp_err_t code)
{
    (void)code;
    return "ESP_FAIL";
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

/* Re-create fresh real registries so every test starts from empty state.
 * Counters are zeroed AFTER the fixture allocations so g_net_allocs==0 is the
 * per-test baseline (an add that allocates exactly what it frees nets to 0). */
static void reset_test_state(void)
{
    g_fail_calloc_on     = 0;
    g_fail_gpio_claim_on = 0;

    g_gpio               = NULL;
    g_fan                = NULL;
    g_src                = NULL;
    g_ds_ref             = NULL;
    f_gpio_init(&g_gpio);
    g_ledc = (f_ledc_handle_t)0x1;
    g_pcnt = (f_pcnt_handle_t)0x1;
    f_fan_init(&g_fan, g_ledc, g_pcnt, g_gpio);
    f_source_init(&g_src, (f_adc_handle_t)0x1, &g_ds_ref, g_gpio);

    g_net_allocs        = 0; /* baseline after fixture alloc */
    g_calloc_calls      = 0;
    g_gpio_claim_calls  = 0;
    g_ledc_add_calls    = 0;
    g_ledc_remove_calls = 0;
    g_pcnt_add_calls    = 0;
    g_pcnt_remove_calls = 0;
    g_bus_del_calls     = 0;
    g_now_us            = 0;
    g_log_calls         = 0;
    g_log_err_calls     = 0;
    g_last_log[0]       = '\0';
}

/* Add a fan with PWM 15 / TACH 4 (both non-reserved on ESP32) into slot 0. */
static esp_err_t setup_fan_15_4(uint8_t *id_out)
{
    return f_fan_add(g_fan, 15, 4, "f0", id_out, NULL);
}

/* Zero the HW + claim call counters so assertions reflect only the f_fan_set_gpio
 * call under test (the setup f_fan_add already bumped ledc/pcnt/claim counters). */
static void reset_hw_counters(void)
{
    g_ledc_add_calls    = 0;
    g_ledc_remove_calls = 0;
    g_pcnt_add_calls    = 0;
    g_pcnt_remove_calls = 0;
    g_gpio_claim_calls  = 0;
}

/* ================================================================
 * f_fan_add tests (FA-P1..P13)
 * ================================================================ */

/* FA-P1 — null-args guard: return INVALID_ARG, *err_msg left NULL. */
static void test_fan_add_null_args_returns_invalid_arg(void)
{
    reset_test_state();
    uint8_t id;
    const char *err = (const char *)0x1;
    CHECK(f_fan_add(NULL, 15, F_FAN_TACH_NONE, "f", &id, &err) == ESP_ERR_INVALID_ARG);
    CHECK(err == NULL);
    err = (const char *)0x1;
    CHECK(f_fan_add(g_fan, 15, F_FAN_TACH_NONE, NULL, &id, &err) == ESP_ERR_INVALID_ARG);
    CHECK(err == NULL);
    err = (const char *)0x1;
    CHECK(f_fan_add(g_fan, 15, F_FAN_TACH_NONE, "f", NULL, &err) == ESP_ERR_INVALID_ARG);
    CHECK(err == NULL);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FA-P2 — PWM pin out of constraints range (0-48): INVALID_ARG, constraints msg. */
static void test_fan_add_pwm_out_of_constraints_returns_invalid_arg(void)
{
    reset_test_state();
    uint8_t id;
    const char *err = NULL;
    CHECK(f_fan_add(g_fan, 50, F_FAN_TACH_NONE, "f", &id, &err) == ESP_ERR_INVALID_ARG);
    CHECK(err != NULL && strcmp(err, "GPIO must be 0-48") == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FA-P3 — TACH pin out of constraints range: INVALID_ARG, constraints msg. */
static void test_fan_add_tach_out_of_constraints_returns_invalid_arg(void)
{
    reset_test_state();
    uint8_t id;
    const char *err = NULL;
    CHECK(f_fan_add(g_fan, 15, 200, "f", &id, &err) == ESP_ERR_INVALID_ARG);
    CHECK(err != NULL && strcmp(err, "GPIO must be 0-48") == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FA-P4 — registry full (8 used): ESP_ERR_NO_MEM, no claim attempted. */
static void test_fan_add_registry_full_returns_no_mem(void)
{
    reset_test_state();
    uint8_t id;
    for (int i = 0; i < F_FAN_MAX_COUNT; i++) {
        CHECK(f_fan_add(g_fan, (uint8_t)(20 + i), F_FAN_TACH_NONE, "f", &id, NULL) == ESP_OK);
    }
    CHECK(f_fan_get_count(g_fan) == 8);
    CHECK(f_fan_add(g_fan, 15, F_FAN_TACH_NONE, "x", &id, NULL) == ESP_ERR_NO_MEM);
    CHECK(f_gpio_is_available(g_gpio, 15) == true); /* no claim on the 9th pin */
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FA-P5 — PWM claim fails (reserved pin 1): INVALID_ARG, "GPIO 1 is reserved", slot free. */
static void test_fan_add_claim_pwm_reserved_returns_invalid_arg(void)
{
    reset_test_state();
    uint8_t id;
    const char *err = NULL;
    CHECK(f_fan_add(g_fan, 1, F_FAN_TACH_NONE, "f", &id, &err) == ESP_ERR_INVALID_ARG);
    CHECK(err != NULL && strcmp(err, "GPIO 1 is reserved") == 0);
    CHECK(f_fan_get_count(g_fan) == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FA-P6 — PWM claim fails (pin 40: passes 0-48 constraints, exceeds 40-pin table):
 * INVALID_ARG, "GPIO 40 is out of range", slot free. */
static void test_fan_add_claim_pwm_out_of_range_returns_invalid_arg(void)
{
    reset_test_state();
    uint8_t id;
    const char *err = NULL;
    CHECK(f_fan_add(g_fan, 40, F_FAN_TACH_NONE, "f", &id, &err) == ESP_ERR_INVALID_ARG);
    CHECK(err != NULL && strcmp(err, "GPIO 40 is out of range") == 0);
    CHECK(f_fan_get_count(g_fan) == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FA-P7 — PWM claim fails (pin 15 already claimed as ADC): INVALID_STATE,
 * "GPIO 15 already in use by ADC", slot free. */
static void test_fan_add_claim_pwm_foreign_claim_returns_invalid_state(void)
{
    reset_test_state();
    CHECK(f_gpio_claim(g_gpio, 15, F_GPIO_CAP_ADC) == ESP_OK);
    uint8_t id;
    const char *err = NULL;
    CHECK(f_fan_add(g_fan, 15, F_FAN_TACH_NONE, "f", &id, &err) == ESP_ERR_INVALID_STATE);
    CHECK(err != NULL && strcmp(err, "GPIO 15 already in use by ADC") == 0);
    CHECK(f_fan_get_count(g_fan) == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FA-P8 — happy path, no tach: PWM claimed, LEDC once, no PCNT, id==0, count 1. */
static void test_fan_add_success_no_tach_claims_pwm_only(void)
{
    reset_test_state();
    uint8_t id;
    const char *err = NULL;
    CHECK(f_fan_add(g_fan, 15, F_FAN_TACH_NONE, "f0", &id, &err) == ESP_OK);
    CHECK(id == 0);
    CHECK(f_fan_get_count(g_fan) == 1);
    f_fan_info_t fi;
    memset(&fi, 0, sizeof(fi));
    CHECK(f_fan_get_info(g_fan, 0, &fi) == ESP_OK);
    CHECK(fi.pwm_gpio == 15);
    CHECK(fi.tach_gpio == 0xFF);
    CHECK(f_gpio_is_claimed_for(g_gpio, 15, F_GPIO_CAP_PWM) == true);
    CHECK(g_ledc_add_calls == 1);
    CHECK(g_pcnt_add_calls == 0);
    CHECK(g_net_allocs == 0); /* no leak from the add */
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FA-P9 — happy path, tach present: both pins claimed, PCNT once. */
static void test_fan_add_success_with_tach_claims_both(void)
{
    reset_test_state();
    uint8_t id;
    const char *err = NULL;
    CHECK(f_fan_add(g_fan, 15, 4, "f0", &id, &err) == ESP_OK);
    CHECK(id == 0);
    f_fan_info_t fi;
    memset(&fi, 0, sizeof(fi));
    CHECK(f_fan_get_info(g_fan, 0, &fi) == ESP_OK);
    CHECK(fi.tach_gpio == 4);
    CHECK(f_gpio_is_claimed_for(g_gpio, 15, F_GPIO_CAP_PWM) == true);
    CHECK(f_gpio_is_claimed_for(g_gpio, 4, F_GPIO_CAP_TACH) == true);
    CHECK(g_pcnt_add_calls == 1);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FA-P10 — tach claim fails (reserved pin 1): pwm released, count 0. */
static void test_fan_add_tach_reserved_releases_pwm_returns_invalid_arg(void)
{
    reset_test_state();
    uint8_t id;
    const char *err = NULL;
    CHECK(f_fan_add(g_fan, 15, 1, "f", &id, &err) == ESP_ERR_INVALID_ARG);
    CHECK(err != NULL && strcmp(err, "GPIO 1 is reserved") == 0);
    CHECK(f_gpio_is_available(g_gpio, 15) == true); /* pwm released */
    CHECK(f_fan_get_count(g_fan) == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FA-P11 — tach claim fails (pin 40 out of range): pwm released, count 0. */
static void test_fan_add_tach_out_of_range_releases_pwm(void)
{
    reset_test_state();
    uint8_t id;
    const char *err = NULL;
    CHECK(f_fan_add(g_fan, 15, 40, "f", &id, &err) == ESP_ERR_INVALID_ARG);
    CHECK(err != NULL && strcmp(err, "GPIO 40 is out of range") == 0);
    CHECK(f_gpio_is_available(g_gpio, 15) == true);
    CHECK(f_fan_get_count(g_fan) == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FA-P12 — tach claim fails (pin 4 already claimed as TACH): pwm released, count 0. */
static void test_fan_add_tach_foreign_claim_releases_pwm(void)
{
    reset_test_state();
    CHECK(f_gpio_claim(g_gpio, 4, F_GPIO_CAP_TACH) == ESP_OK);
    uint8_t id;
    const char *err = NULL;
    CHECK(f_fan_add(g_fan, 15, 4, "f", &id, &err) == ESP_ERR_INVALID_STATE);
    CHECK(err != NULL && strcmp(err, "GPIO 4 already in use by TACH") == 0);
    CHECK(f_gpio_is_available(g_gpio, 15) == true);
    CHECK(f_fan_get_count(g_fan) == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FA-P13 — pwm==tach: the second claim on the just-claimed PWM pin returns
 * INVALID_STATE; pwm released; no fan created. */
static void test_fan_add_pwm_equals_tach_rejected_and_pwm_released(void)
{
    reset_test_state();
    uint8_t id;
    const char *err = NULL;
    CHECK(f_fan_add(g_fan, 15, 15, "f", &id, &err) == ESP_ERR_INVALID_STATE);
    CHECK(err != NULL && strcmp(err, "GPIO 15 already in use by PWM") == 0);
    CHECK(f_gpio_is_available(g_gpio, 15) == true);
    CHECK(f_fan_get_count(g_fan) == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* ================================================================
 * f_fan_set_gpio tests (FS-P1..P20)
 * ================================================================ */

/* FS-P1 — null/invalid-id guard: INVALID_ARG, *err_msg NULL. */
static void test_fan_set_gpio_null_args_returns_invalid_arg(void)
{
    reset_test_state();
    const char *err = (const char *)0x1;
    CHECK(f_fan_set_gpio(NULL, 0, 15, 4, &err) == ESP_ERR_INVALID_ARG);
    CHECK(err == NULL);
    err = (const char *)0x1;
    CHECK(f_fan_set_gpio(g_fan, 99, 15, 4, &err) == ESP_ERR_INVALID_ARG);
    CHECK(err == NULL);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FS-P2 — new PWM out of constraints range: INVALID_ARG, constraints msg. */
static void test_fan_set_gpio_new_pwm_out_of_constraints_returns_invalid_arg(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(setup_fan_15_4(&id) == ESP_OK);
    const char *err = NULL;
    CHECK(f_fan_set_gpio(g_fan, 0, 50, 4, &err) == ESP_ERR_INVALID_ARG);
    CHECK(err != NULL && strcmp(err, "GPIO must be 0-48") == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FS-P3 — new TACH out of constraints range: INVALID_ARG. */
static void test_fan_set_gpio_new_tach_out_of_constraints_returns_invalid_arg(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(setup_fan_15_4(&id) == ESP_OK);
    const char *err = NULL;
    CHECK(f_fan_set_gpio(g_fan, 0, 15, 200, &err) == ESP_ERR_INVALID_ARG);
    CHECK(err != NULL && strcmp(err, "GPIO must be 0-48") == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FS-P4 — slot not used: ESP_ERR_NOT_FOUND. */
static void test_fan_set_gpio_slot_not_used_returns_not_found(void)
{
    reset_test_state();
    const char *err = NULL;
    CHECK(f_fan_set_gpio(g_fan, 1, 15, 4, &err) == ESP_ERR_NOT_FOUND);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FS-P5 — new_pwm == new_tach: INVALID_ARG, fan + claims intact, no teardown. */
static void test_fan_set_gpio_pwm_equals_tach_returns_invalid_arg_fan_intact(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(setup_fan_15_4(&id) == ESP_OK);
    const char *err = NULL;
    CHECK(f_fan_set_gpio(g_fan, 0, 20, 20, &err) == ESP_ERR_INVALID_ARG);
    CHECK(err != NULL && strcmp(err, "pwm and tach cannot use the same GPIO") == 0);
    f_fan_info_t fi;
    memset(&fi, 0, sizeof(fi));
    CHECK(f_fan_get_info(g_fan, 0, &fi) == ESP_OK);
    CHECK(fi.pwm_gpio == 15 && fi.tach_gpio == 4);
    CHECK(f_gpio_is_claimed_for(g_gpio, 15, F_GPIO_CAP_PWM) == true);
    CHECK(g_ledc_remove_calls == 0); /* no teardown ran */
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FS-P6 — new PWM out of range (40): INVALID_ARG, fan intact. */
static void test_fan_set_gpio_new_pwm_out_of_range_fan_intact(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(setup_fan_15_4(&id) == ESP_OK);
    const char *err = NULL;
    CHECK(f_fan_set_gpio(g_fan, 0, 40, 4, &err) == ESP_ERR_INVALID_ARG);
    CHECK(err != NULL && strcmp(err, "GPIO 40 is out of range") == 0);
    f_fan_info_t fi;
    memset(&fi, 0, sizeof(fi));
    CHECK(f_fan_get_info(g_fan, 0, &fi) == ESP_OK);
    CHECK(fi.pwm_gpio == 15 && fi.tach_gpio == 4);
    CHECK(f_gpio_is_claimed_for(g_gpio, 15, F_GPIO_CAP_PWM) == true);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FS-P7 — new PWM reserved (1): INVALID_ARG, fan intact. */
static void test_fan_set_gpio_new_pwm_reserved_fan_intact(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(setup_fan_15_4(&id) == ESP_OK);
    const char *err = NULL;
    CHECK(f_fan_set_gpio(g_fan, 0, 1, 4, &err) == ESP_ERR_INVALID_ARG);
    CHECK(err != NULL && strcmp(err, "GPIO 1 is reserved") == 0);
    f_fan_info_t fi;
    memset(&fi, 0, sizeof(fi));
    CHECK(f_fan_get_info(g_fan, 0, &fi) == ESP_OK);
    CHECK(fi.pwm_gpio == 15 && fi.tach_gpio == 4);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FS-P8 — new PWM foreign-claimed (20 as ADC): INVALID_STATE, fan intact. */
static void test_fan_set_gpio_new_pwm_foreign_fan_intact(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(setup_fan_15_4(&id) == ESP_OK);
    CHECK(f_gpio_claim(g_gpio, 20, F_GPIO_CAP_ADC) == ESP_OK);
    const char *err = NULL;
    CHECK(f_fan_set_gpio(g_fan, 0, 20, 4, &err) == ESP_ERR_INVALID_STATE);
    CHECK(err != NULL && strcmp(err, "GPIO 20 already in use by ADC") == 0);
    f_fan_info_t fi;
    memset(&fi, 0, sizeof(fi));
    CHECK(f_fan_get_info(g_fan, 0, &fi) == ESP_OK);
    CHECK(fi.pwm_gpio == 15 && fi.tach_gpio == 4);
    CHECK(f_gpio_is_claimed_for(g_gpio, 15, F_GPIO_CAP_PWM) == true);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FS-P9 — new TACH out of range (40): INVALID_ARG, fan intact. */
static void test_fan_set_gpio_new_tach_out_of_range_fan_intact(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(setup_fan_15_4(&id) == ESP_OK);
    const char *err = NULL;
    CHECK(f_fan_set_gpio(g_fan, 0, 15, 40, &err) == ESP_ERR_INVALID_ARG);
    CHECK(err != NULL && strcmp(err, "GPIO 40 is out of range") == 0);
    f_fan_info_t fi;
    memset(&fi, 0, sizeof(fi));
    CHECK(f_fan_get_info(g_fan, 0, &fi) == ESP_OK);
    CHECK(fi.pwm_gpio == 15 && fi.tach_gpio == 4);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FS-P10 — new TACH reserved (1): INVALID_ARG, fan intact. */
static void test_fan_set_gpio_new_tach_reserved_fan_intact(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(setup_fan_15_4(&id) == ESP_OK);
    const char *err = NULL;
    CHECK(f_fan_set_gpio(g_fan, 0, 15, 1, &err) == ESP_ERR_INVALID_ARG);
    CHECK(err != NULL && strcmp(err, "GPIO 1 is reserved") == 0);
    f_fan_info_t fi;
    memset(&fi, 0, sizeof(fi));
    CHECK(f_fan_get_info(g_fan, 0, &fi) == ESP_OK);
    CHECK(fi.pwm_gpio == 15 && fi.tach_gpio == 4);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FS-P11 — new TACH foreign-claimed (20 as TACH): INVALID_STATE, fan intact. */
static void test_fan_set_gpio_new_tach_foreign_fan_intact(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(setup_fan_15_4(&id) == ESP_OK);
    CHECK(f_gpio_claim(g_gpio, 20, F_GPIO_CAP_TACH) == ESP_OK);
    const char *err = NULL;
    CHECK(f_fan_set_gpio(g_fan, 0, 15, 20, &err) == ESP_ERR_INVALID_STATE);
    CHECK(err != NULL && strcmp(err, "GPIO 20 already in use by TACH") == 0);
    f_fan_info_t fi;
    memset(&fi, 0, sizeof(fi));
    CHECK(f_fan_get_info(g_fan, 0, &fi) == ESP_OK);
    CHECK(fi.pwm_gpio == 15 && fi.tach_gpio == 4);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FS-P12 — identity (new==old): OK, both pins re-claimed, LEDC teardown+realloc. */
static void test_fan_set_gpio_identity_succeeds_reclaims_same_pins(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(setup_fan_15_4(&id) == ESP_OK);
    reset_hw_counters();
    const char *err = NULL;
    CHECK(f_fan_set_gpio(g_fan, 0, 15, 4, &err) == ESP_OK);
    f_fan_info_t fi;
    memset(&fi, 0, sizeof(fi));
    CHECK(f_fan_get_info(g_fan, 0, &fi) == ESP_OK);
    CHECK(fi.pwm_gpio == 15 && fi.tach_gpio == 4);
    CHECK(f_gpio_is_claimed_for(g_gpio, 15, F_GPIO_CAP_PWM) == true);
    CHECK(f_gpio_is_claimed_for(g_gpio, 4, F_GPIO_CAP_TACH) == true);
    CHECK(g_ledc_remove_calls == 1 && g_ledc_add_calls == 1);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FS-P13 — PWM-only change: old pwm released, new pwm claimed, tach untouched. */
static void test_fan_set_gpio_pwm_only_change_reclaims(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(setup_fan_15_4(&id) == ESP_OK);
    const char *err = NULL;
    CHECK(f_fan_set_gpio(g_fan, 0, 20, 4, &err) == ESP_OK);
    f_fan_info_t fi;
    memset(&fi, 0, sizeof(fi));
    CHECK(f_fan_get_info(g_fan, 0, &fi) == ESP_OK);
    CHECK(fi.pwm_gpio == 20 && fi.tach_gpio == 4);
    CHECK(f_gpio_is_available(g_gpio, 15) == true);
    CHECK(f_gpio_is_claimed_for(g_gpio, 20, F_GPIO_CAP_PWM) == true);
    CHECK(f_gpio_is_claimed_for(g_gpio, 4, F_GPIO_CAP_TACH) == true);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FS-P14 — TACH-only change: old tach released, new tach claimed, pwm untouched. */
static void test_fan_set_gpio_tach_only_change_reclaims(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(setup_fan_15_4(&id) == ESP_OK);
    const char *err = NULL;
    CHECK(f_fan_set_gpio(g_fan, 0, 15, 21, &err) == ESP_OK);
    f_fan_info_t fi;
    memset(&fi, 0, sizeof(fi));
    CHECK(f_fan_get_info(g_fan, 0, &fi) == ESP_OK);
    CHECK(fi.pwm_gpio == 15 && fi.tach_gpio == 21);
    CHECK(f_gpio_is_available(g_gpio, 4) == true);
    CHECK(f_gpio_is_claimed_for(g_gpio, 21, F_GPIO_CAP_TACH) == true);
    CHECK(f_gpio_is_claimed_for(g_gpio, 15, F_GPIO_CAP_PWM) == true);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FS-P15 — full swap (pwm<->tach): both re-claimed, no INVALID_STATE. */
static void test_fan_set_gpio_full_swap_succeeds(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(setup_fan_15_4(&id) == ESP_OK);
    const char *err = NULL;
    CHECK(f_fan_set_gpio(g_fan, 0, 4, 15, &err) == ESP_OK);
    f_fan_info_t fi;
    memset(&fi, 0, sizeof(fi));
    CHECK(f_fan_get_info(g_fan, 0, &fi) == ESP_OK);
    CHECK(fi.pwm_gpio == 4 && fi.tach_gpio == 15);
    CHECK(f_gpio_is_claimed_for(g_gpio, 4, F_GPIO_CAP_PWM) == true);
    CHECK(f_gpio_is_claimed_for(g_gpio, 15, F_GPIO_CAP_TACH) == true);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FS-P16 — both to new pins: old released, new claimed. */
static void test_fan_set_gpio_both_new_pins_succeeds(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(setup_fan_15_4(&id) == ESP_OK);
    const char *err = NULL;
    CHECK(f_fan_set_gpio(g_fan, 0, 20, 21, &err) == ESP_OK);
    f_fan_info_t fi;
    memset(&fi, 0, sizeof(fi));
    CHECK(f_fan_get_info(g_fan, 0, &fi) == ESP_OK);
    CHECK(fi.pwm_gpio == 20 && fi.tach_gpio == 21);
    CHECK(f_gpio_is_available(g_gpio, 15) == true);
    CHECK(f_gpio_is_available(g_gpio, 4) == true);
    CHECK(f_gpio_is_claimed_for(g_gpio, 20, F_GPIO_CAP_PWM) == true);
    CHECK(f_gpio_is_claimed_for(g_gpio, 21, F_GPIO_CAP_TACH) == true);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FS-P17 — tach removal (new_tach == NONE): old tach released, PCNT removed. */
static void test_fan_set_gpio_tach_removed_releases_tach(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(setup_fan_15_4(&id) == ESP_OK);
    const char *err = NULL;
    CHECK(f_fan_set_gpio(g_fan, 0, 15, F_FAN_TACH_NONE, &err) == ESP_OK);
    f_fan_info_t fi;
    memset(&fi, 0, sizeof(fi));
    CHECK(f_fan_get_info(g_fan, 0, &fi) == ESP_OK);
    CHECK(fi.tach_gpio == 0xFF);
    CHECK(f_gpio_is_available(g_gpio, 4) == true);
    CHECK(f_gpio_is_claimed_for(g_gpio, 15, F_GPIO_CAP_PWM) == true);
    CHECK(g_pcnt_remove_calls == 1);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FS-P18 — tach addition (old tach NONE): new tach claimed, PCNT added. */
static void test_fan_set_gpio_tach_added_claims_new_tach(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(f_fan_add(g_fan, 15, F_FAN_TACH_NONE, "f0", &id, NULL) == ESP_OK);
    const char *err = NULL;
    CHECK(f_fan_set_gpio(g_fan, 0, 15, 20, &err) == ESP_OK);
    f_fan_info_t fi;
    memset(&fi, 0, sizeof(fi));
    CHECK(f_fan_get_info(g_fan, 0, &fi) == ESP_OK);
    CHECK(fi.tach_gpio == 20);
    CHECK(f_gpio_is_claimed_for(g_gpio, 20, F_GPIO_CAP_TACH) == true);
    CHECK(f_gpio_is_claimed_for(g_gpio, 15, F_GPIO_CAP_PWM) == true);
    CHECK(g_pcnt_add_calls == 1);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FS-P19 — post-alloc PWM claim fails (injected): error returned, pin NOT
 * claimed, struct fields unchanged. */
static void test_fan_set_gpio_pwm_claim_injected_failure_returns_error(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(setup_fan_15_4(&id) == ESP_OK);
    reset_hw_counters();
    g_fail_gpio_claim_on = 1; /* first f_gpio_claim (new pwm 20) fails */
    const char *err      = NULL;
    CHECK(f_fan_set_gpio(g_fan, 0, 20, 4, &err) == ESP_ERR_INVALID_STATE);
    CHECK(err != NULL);
    CHECK(f_gpio_is_available(g_gpio, 20) == true); /* not claimed */
    f_fan_info_t fi;
    memset(&fi, 0, sizeof(fi));
    CHECK(f_fan_get_info(g_fan, 0, &fi) == ESP_OK);
    CHECK(fi.pwm_gpio == 15 && fi.tach_gpio == 4); /* fields unchanged */
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FS-P20 — post-alloc TACH claim fails (injected): new pwm released, tach NOT
 * claimed, struct fields unchanged. */
static void test_fan_set_gpio_tach_claim_injected_failure_releases_new_pwm(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(setup_fan_15_4(&id) == ESP_OK);
    reset_hw_counters();
    g_fail_gpio_claim_on = 2; /* 1st claim (new pwm 20) OK, 2nd claim (new tach 21) fails */
    const char *err      = NULL;
    CHECK(f_fan_set_gpio(g_fan, 0, 20, 21, &err) == ESP_ERR_INVALID_STATE);
    CHECK(err != NULL);
    CHECK(f_gpio_is_available(g_gpio, 20) == true); /* new pwm released */
    CHECK(f_gpio_is_available(g_gpio, 21) == true); /* not claimed */
    f_fan_info_t fi;
    memset(&fi, 0, sizeof(fi));
    CHECK(f_fan_get_info(g_fan, 0, &fi) == ESP_OK);
    CHECK(fi.pwm_gpio == 15 && fi.tach_gpio == 4);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* ================================================================
 * f_fan_remove tests (FR-P1..P4)
 * ================================================================ */

/* FR-P1 — null/out-of-range guard: INVALID_ARG. */
static void test_fan_remove_null_args_returns_invalid_arg(void)
{
    reset_test_state();
    CHECK(f_fan_remove(NULL, 0) == ESP_ERR_INVALID_ARG);
    CHECK(f_fan_remove(g_fan, 99) == ESP_ERR_INVALID_ARG);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FR-P2 — unused slot: NOT_FOUND, count unchanged. */
static void test_fan_remove_unused_slot_returns_not_found(void)
{
    reset_test_state();
    CHECK(f_fan_remove(g_fan, 0) == ESP_ERR_NOT_FOUND);
    CHECK(f_fan_get_count(g_fan) == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FR-P3 — remove fan with no tach: pwm released, slot cleared. */
static void test_fan_remove_no_tach_releases_pwm(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(f_fan_add(g_fan, 15, F_FAN_TACH_NONE, "f", &id, NULL) == ESP_OK);
    CHECK(f_fan_remove(g_fan, 0) == ESP_OK);
    CHECK(f_gpio_is_available(g_gpio, 15) == true);
    CHECK(f_fan_get_count(g_fan) == 0);
    CHECK(f_fan_remove(g_fan, 0) == ESP_ERR_NOT_FOUND); /* slot cleared */
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* FR-P4 — remove fan with tach: both pins released, PCNT removed. */
static void test_fan_remove_with_tach_releases_both(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(f_fan_add(g_fan, 15, 4, "f", &id, NULL) == ESP_OK);
    CHECK(f_fan_remove(g_fan, 0) == ESP_OK);
    CHECK(f_gpio_is_available(g_gpio, 15) == true);
    CHECK(f_gpio_is_available(g_gpio, 4) == true);
    CHECK(g_pcnt_remove_calls == 1);
    CHECK(f_fan_get_count(g_fan) == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* ================================================================
 * f_source_add tests (SA-P1..P8)
 * ================================================================ */

/* SA-P1 — null-args guard: INVALID_ARG, *err_msg NULL. */
static void test_source_add_null_args_returns_invalid_arg(void)
{
    reset_test_state();
    uint8_t id;
    const char *err = (const char *)0x1;
    CHECK(f_source_add(NULL, SOURCE_TYPE_NTC, 15, "s", &id, &err) == ESP_ERR_INVALID_ARG);
    CHECK(err == NULL);
    err = (const char *)0x1;
    CHECK(f_source_add(g_src, SOURCE_TYPE_NTC, 15, NULL, &id, &err) == ESP_ERR_INVALID_ARG);
    CHECK(err == NULL);
    err = (const char *)0x1;
    CHECK(f_source_add(g_src, SOURCE_TYPE_NTC, 15, "s", NULL, &err) == ESP_ERR_INVALID_ARG);
    CHECK(err == NULL);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* SA-P2 — registry full (8 used): NO_MEM, no claim. */
static void test_source_add_registry_full_returns_no_mem(void)
{
    reset_test_state();
    uint8_t id;
    for (int i = 0; i < F_SOURCE_MAX_COUNT; i++) {
        CHECK(f_source_add(g_src, SOURCE_TYPE_MANUAL, 15, "s", &id, NULL) == ESP_OK);
    }
    CHECK(f_source_get_count(g_src) == 8);
    CHECK(f_source_add(g_src, SOURCE_TYPE_NTC, 15, "x", &id, NULL) == ESP_ERR_NO_MEM);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* SA-P3 — NTC claim succeeds: pin claimed ADC, info reflects gpio. */
static void test_source_add_ntc_claims_adc_succeeds(void)
{
    reset_test_state();
    uint8_t id;
    const char *err = NULL;
    CHECK(f_source_add(g_src, SOURCE_TYPE_NTC, 15, "s", &id, &err) == ESP_OK);
    CHECK(id == 0);
    CHECK(f_source_get_count(g_src) == 1);
    CHECK(f_gpio_is_claimed_for(g_gpio, 15, F_GPIO_CAP_ADC) == true);
    f_source_info_t si;
    memset(&si, 0, sizeof(si));
    CHECK(f_source_get_info(g_src, 0, &si) == ESP_OK);
    CHECK(si.gpio == 15);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* SA-P4 — NTC with sentinel gpio (0xFF): claim skipped, source created. */
static void test_source_add_ntc_sentinel_gpio_skips_claim(void)
{
    reset_test_state();
    uint8_t id;
    const char *err = NULL;
    CHECK(f_source_add(g_src, SOURCE_TYPE_NTC, F_SOURCE_GPIO_NONE, "s", &id, &err) == ESP_OK);
    f_source_info_t si;
    memset(&si, 0, sizeof(si));
    CHECK(f_source_get_info(g_src, 0, &si) == ESP_OK);
    CHECK(si.gpio == 0xFF);
    CHECK(f_gpio_is_available(g_gpio, 0xFF) == false); /* never attempted */
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* SA-P5 — MANUAL / DS18B20 types skip the ADC claim entirely. */
static void test_source_add_manual_skips_claim(void)
{
    reset_test_state();
    uint8_t id;
    const char *err = NULL;
    CHECK(f_source_add(g_src, SOURCE_TYPE_MANUAL, 15, "s", &id, &err) == ESP_OK);
    CHECK(f_gpio_is_available(g_gpio, 15) == true); /* not claimed */
    reset_test_state();
    err = NULL;
    CHECK(f_source_add(g_src, SOURCE_TYPE_DS18B20, 15, "d", &id, &err) == ESP_OK);
    CHECK(f_gpio_is_available(g_gpio, 15) == true);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* SA-P6 — NTC claim fails (reserved 1): INVALID_ARG, slot free, ESP_LOGE. */
static void test_source_add_ntc_reserved_returns_invalid_arg(void)
{
    reset_test_state();
    uint8_t id;
    const char *err = NULL;
    CHECK(f_source_add(g_src, SOURCE_TYPE_NTC, 1, "s", &id, &err) == ESP_ERR_INVALID_ARG);
    CHECK(err != NULL && strcmp(err, "GPIO 1 is reserved") == 0);
    CHECK(f_source_get_count(g_src) == 0);
    CHECK(g_log_err_calls == 1); /* ESP_LOGE emitted on claim failure */
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* SA-P7 — NTC claim fails (pin 40 out of range): INVALID_ARG, slot free. */
static void test_source_add_ntc_out_of_range_returns_invalid_arg(void)
{
    reset_test_state();
    uint8_t id;
    const char *err = NULL;
    CHECK(f_source_add(g_src, SOURCE_TYPE_NTC, 40, "s", &id, &err) == ESP_ERR_INVALID_ARG);
    CHECK(err != NULL && strcmp(err, "GPIO 40 is out of range") == 0);
    CHECK(f_source_get_count(g_src) == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* SA-P8 — NTC claim fails (15 already claimed as PWM): INVALID_STATE, slot free. */
static void test_source_add_ntc_foreign_claim_returns_invalid_state(void)
{
    reset_test_state();
    CHECK(f_gpio_claim(g_gpio, 15, F_GPIO_CAP_PWM) == ESP_OK);
    uint8_t id;
    const char *err = NULL;
    CHECK(f_source_add(g_src, SOURCE_TYPE_NTC, 15, "s", &id, &err) == ESP_ERR_INVALID_STATE);
    CHECK(err != NULL && strcmp(err, "GPIO 15 already in use by PWM") == 0);
    CHECK(f_source_get_count(g_src) == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* ================================================================
 * f_source_remove tests (SR-P1..P5)
 * ================================================================ */

/* SR-P1 — null/out-of-range guard: INVALID_ARG. */
static void test_source_remove_null_args_returns_invalid_arg(void)
{
    reset_test_state();
    CHECK(f_source_remove(NULL, 0) == ESP_ERR_INVALID_ARG);
    CHECK(f_source_remove(g_src, 99) == ESP_ERR_INVALID_ARG);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* SR-P2 — unused slot: NOT_FOUND. */
static void test_source_remove_unused_slot_returns_not_found(void)
{
    reset_test_state();
    CHECK(f_source_remove(g_src, 0) == ESP_ERR_NOT_FOUND);
    CHECK(f_source_get_count(g_src) == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* SR-P3 — NTC source releases ADC claim. */
static void test_source_remove_ntc_releases_adc(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(f_source_add(g_src, SOURCE_TYPE_NTC, 15, "s", &id, NULL) == ESP_OK);
    CHECK(f_source_remove(g_src, 0) == ESP_OK);
    CHECK(f_gpio_is_available(g_gpio, 15) == true);
    CHECK(f_source_get_count(g_src) == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* SR-P4 — NTC with sentinel gpio: no release attempted. */
static void test_source_remove_ntc_sentinel_no_release(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(f_source_add(g_src, SOURCE_TYPE_NTC, F_SOURCE_GPIO_NONE, "s", &id, NULL) == ESP_OK);
    CHECK(f_source_remove(g_src, 0) == ESP_OK);
    CHECK(f_source_get_count(g_src) == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* SR-P5 — MANUAL source: no release. */
static void test_source_remove_manual_no_release(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(f_source_add(g_src, SOURCE_TYPE_MANUAL, 15, "s", &id, NULL) == ESP_OK);
    CHECK(f_source_remove(g_src, 0) == ESP_OK);
    CHECK(f_gpio_is_available(g_gpio, 15) == true);
    CHECK(f_source_get_count(g_src) == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* ================================================================
 * f_ds18b20_init tests (DI-P1..P6)
 * ================================================================ */

/* DI-P1 — null-handle guard: INVALID_ARG. */
static void test_ds18b20_init_null_handle_returns_invalid_arg(void)
{
    reset_test_state();
    CHECK(f_ds18b20_init(NULL, 15, g_gpio) == ESP_ERR_INVALID_ARG);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* DI-P2 — calloc failure: NO_MEM, *handle unchanged. */
static void test_ds18b20_init_calloc_failure_returns_no_mem_handle_unset(void)
{
    reset_test_state();
    f_ds18b20_handle_t h = (f_ds18b20_handle_t)0x1;
    g_fail_calloc_on     = 1;
    CHECK(f_ds18b20_init(&h, 15, g_gpio) == ESP_ERR_NO_MEM);
    CHECK(h == (f_ds18b20_handle_t)0x1); /* unchanged */
    CHECK(g_bus_del_calls == 0);         /* bus never created */
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* DI-P3 — claim succeeds: ONEWIRE claimed, no bus del. */
static void test_ds18b20_init_success_claims_onwire(void)
{
    reset_test_state();
    f_ds18b20_handle_t h = NULL;
    CHECK(f_ds18b20_init(&h, 15, g_gpio) == ESP_OK);
    CHECK(h != NULL);
    CHECK(f_gpio_is_claimed_for(g_gpio, 15, F_GPIO_CAP_ONEWIRE) == true);
    CHECK(g_bus_del_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* DI-P4 — reserved pin: bus del + free, *handle unset. */
static void test_ds18b20_init_reserved_cleans_up_handle_unset(void)
{
    reset_test_state();
    f_ds18b20_handle_t h = (f_ds18b20_handle_t)0x1;
    CHECK(f_ds18b20_init(&h, 1, g_gpio) == ESP_ERR_INVALID_ARG);
    CHECK(g_bus_del_calls == 1);
    CHECK(g_net_allocs == 0);            /* handle freed */
    CHECK(h == (f_ds18b20_handle_t)0x1); /* not set */
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* DI-P5 — out-of-range pin: bus del + free, *handle unset. */
static void test_ds18b20_init_out_of_range_cleans_up_handle_unset(void)
{
    reset_test_state();
    f_ds18b20_handle_t h = (f_ds18b20_handle_t)0x1;
    CHECK(f_ds18b20_init(&h, 40, g_gpio) == ESP_ERR_INVALID_ARG);
    CHECK(g_bus_del_calls == 1);
    CHECK(g_net_allocs == 0);
    CHECK(h == (f_ds18b20_handle_t)0x1);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* DI-P6 — foreign-claimed pin: bus del + free, *handle unset. */
static void test_ds18b20_init_foreign_claim_cleans_up_handle_unset(void)
{
    reset_test_state();
    CHECK(f_gpio_claim(g_gpio, 15, F_GPIO_CAP_ADC) == ESP_OK);
    f_ds18b20_handle_t h = (f_ds18b20_handle_t)0x1;
    CHECK(f_ds18b20_init(&h, 15, g_gpio) == ESP_ERR_INVALID_STATE);
    CHECK(g_bus_del_calls == 1);
    CHECK(g_net_allocs == 0);
    CHECK(h == (f_ds18b20_handle_t)0x1);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* ================================================================
 * f_source_update_manual tests (SU-P1..P9)
 * ================================================================ */

/* SU-P1 — NULL-handle guard: INVALID_ARG, registry untouched. */
static void test_source_update_manual_null_handle_returns_invalid_arg(void)
{
    reset_test_state();
    CHECK(f_source_update_manual(NULL, 0, 25.0f) == ESP_ERR_INVALID_ARG);
    CHECK(f_source_get_count(g_src) == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* SU-P2 — id out of range (>= F_SOURCE_MAX_COUNT): INVALID_ARG, slot untouched. */
static void test_source_update_manual_id_out_of_range_returns_invalid_arg(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(f_source_add(g_src, SOURCE_TYPE_MANUAL, F_SOURCE_GPIO_NONE, "man", &id, NULL) == ESP_OK);
    CHECK(f_source_update_manual(g_src, F_SOURCE_MAX_COUNT, 25.0f) == ESP_ERR_INVALID_ARG);
    f_source_info_t si;
    memset(&si, 0, sizeof(si));
    CHECK(f_source_get_info(g_src, 0, &si) == ESP_OK);
    CHECK(si.temp_c == 0.0f);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* SU-P3 — temp below physical min (-40.0): INVALID_ARG, no write, slot unchanged. */
static void test_source_update_manual_temp_below_min_rejected_before_write(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(f_source_add(g_src, SOURCE_TYPE_MANUAL, F_SOURCE_GPIO_NONE, "man", &id, NULL) == ESP_OK);
    CHECK(f_source_update_manual(g_src, id, -40.01f) == ESP_ERR_INVALID_ARG);
    f_source_info_t si;
    memset(&si, 0, sizeof(si));
    CHECK(f_source_get_info(g_src, id, &si) == ESP_OK);
    CHECK(si.temp_c == 0.0f);
    CHECK(si.status == SOURCE_STATUS_VALID);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* SU-P4 — temp above physical max (125.0): INVALID_ARG, no write, slot untouched. */
static void test_source_update_manual_temp_above_max_rejected_before_write(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(f_source_add(g_src, SOURCE_TYPE_MANUAL, F_SOURCE_GPIO_NONE, "man", &id, NULL) == ESP_OK);
    CHECK(f_source_update_manual(g_src, id, 125.01f) == ESP_ERR_INVALID_ARG);
    f_source_info_t si;
    memset(&si, 0, sizeof(si));
    CHECK(f_source_get_info(g_src, id, &si) == ESP_OK);
    CHECK(si.temp_c == 0.0f);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* SU-P5 — slot not used: NOT_FOUND, no write. */
static void test_source_update_manual_unused_slot_returns_not_found(void)
{
    reset_test_state();
    CHECK(f_source_update_manual(g_src, 0, 25.0f) == ESP_ERR_NOT_FOUND);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* SU-P6 — slot used but non-MANUAL type (DS18B20): INVALID_ARG, temp unchanged. */
static void test_source_update_manual_non_manual_type_rejected(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(f_source_add_ds18b20(g_src, 0x28ABCDULL, "ds", &id) == ESP_OK);
    CHECK(f_source_update_manual(g_src, id, 25.0f) == ESP_ERR_INVALID_ARG);
    f_source_info_t si;
    memset(&si, 0, sizeof(si));
    CHECK(f_source_get_info(g_src, id, &si) == ESP_OK);
    CHECK(si.temp_c == 0.0f);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* SU-P7 — happy path: writes temp, marks VALID, stamps clock. */
static void test_source_update_manual_happy_path_writes_and_marks_valid(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(f_source_add(g_src, SOURCE_TYPE_MANUAL, F_SOURCE_GPIO_NONE, "man", &id, NULL) == ESP_OK);
    g_now_us = 1000000;
    CHECK(f_source_update_manual(g_src, id, 25.0f) == ESP_OK);
    f_source_info_t si;
    memset(&si, 0, sizeof(si));
    CHECK(f_source_get_info(g_src, id, &si) == ESP_OK);
    CHECK(si.temp_c == 25.0f);
    CHECK(si.last_update_us == 1000000);
    CHECK(si.status == SOURCE_STATUS_VALID);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* SU-P8 — boundary accepted at min (inclusive): OK, temp stored. */
static void test_source_update_manual_accepts_min_boundary(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(f_source_add(g_src, SOURCE_TYPE_MANUAL, F_SOURCE_GPIO_NONE, "man", &id, NULL) == ESP_OK);
    CHECK(f_source_update_manual(g_src, id, -40.0f) == ESP_OK);
    f_source_info_t si;
    memset(&si, 0, sizeof(si));
    CHECK(f_source_get_info(g_src, id, &si) == ESP_OK);
    CHECK(si.temp_c == -40.0f);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* SU-P9 — boundary accepted at max (inclusive): OK, temp stored. */
static void test_source_update_manual_accepts_max_boundary(void)
{
    reset_test_state();
    uint8_t id;
    CHECK(f_source_add(g_src, SOURCE_TYPE_MANUAL, F_SOURCE_GPIO_NONE, "man", &id, NULL) == ESP_OK);
    CHECK(f_source_update_manual(g_src, id, 125.0f) == ESP_OK);
    f_source_info_t si;
    memset(&si, 0, sizeof(si));
    CHECK(f_source_get_info(g_src, id, &si) == ESP_OK);
    CHECK(si.temp_c == 125.0f);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* ================================================================
 * f_constraints_temp_c tests (CT-1..3)
 * ================================================================ */

/* CT-1 — in-range (inclusive): OK; err_msg NOT written on success. */
static void test_constraints_temp_c_in_range_returns_ok(void)
{
    reset_test_state();
    const char *em = (const char *)0x1;
    CHECK(f_constraints_temp_c(0.0f, &em) == ESP_OK);
    CHECK(em == (const char *)0x1); /* sentinel untouched on success */
    CHECK(f_constraints_temp_c(-40.0f, NULL) == ESP_OK);
    CHECK(f_constraints_temp_c(125.0f, NULL) == ESP_OK);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* CT-2 — below min: INVALID_ARG + ERR_TEMP literal; NULL err_msg guard no crash. */
static void test_constraints_temp_c_below_min_returns_invalid_arg(void)
{
    reset_test_state();
    const char *em = NULL;
    CHECK(f_constraints_temp_c(-40.01f, &em) == ESP_ERR_INVALID_ARG);
    CHECK(em != NULL && strcmp(em, "temp_c must be -40.0 to 125.0") == 0);
    CHECK(f_constraints_temp_c(-40.01f, NULL) == ESP_ERR_INVALID_ARG);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* CT-3 — above max: INVALID_ARG + ERR_TEMP literal; NULL err_msg guard no crash. */
static void test_constraints_temp_c_above_max_returns_invalid_arg(void)
{
    reset_test_state();
    const char *em = NULL;
    CHECK(f_constraints_temp_c(125.01f, &em) == ESP_ERR_INVALID_ARG);
    CHECK(em != NULL && strcmp(em, "temp_c must be -40.0 to 125.0") == 0);
    CHECK(f_constraints_temp_c(125.01f, NULL) == ESP_ERR_INVALID_ARG);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* ================================================================
 * main
 * ================================================================ */

int main(void)
{
    /* f_fan_add */
    test_fan_add_null_args_returns_invalid_arg();                  /* FA-P1 */
    test_fan_add_pwm_out_of_constraints_returns_invalid_arg();     /* FA-P2 */
    test_fan_add_tach_out_of_constraints_returns_invalid_arg();    /* FA-P3 */
    test_fan_add_registry_full_returns_no_mem();                   /* FA-P4 */
    test_fan_add_claim_pwm_reserved_returns_invalid_arg();         /* FA-P5 */
    test_fan_add_claim_pwm_out_of_range_returns_invalid_arg();     /* FA-P6 */
    test_fan_add_claim_pwm_foreign_claim_returns_invalid_state();  /* FA-P7 */
    test_fan_add_success_no_tach_claims_pwm_only();                /* FA-P8 */
    test_fan_add_success_with_tach_claims_both();                  /* FA-P9 */
    test_fan_add_tach_reserved_releases_pwm_returns_invalid_arg(); /* FA-P10 */
    test_fan_add_tach_out_of_range_releases_pwm();                 /* FA-P11 */
    test_fan_add_tach_foreign_claim_releases_pwm();                /* FA-P12 */
    test_fan_add_pwm_equals_tach_rejected_and_pwm_released();      /* FA-P13 */

    /* f_fan_set_gpio */
    test_fan_set_gpio_null_args_returns_invalid_arg();                   /* FS-P1 */
    test_fan_set_gpio_new_pwm_out_of_constraints_returns_invalid_arg();  /* FS-P2 */
    test_fan_set_gpio_new_tach_out_of_constraints_returns_invalid_arg(); /* FS-P3 */
    test_fan_set_gpio_slot_not_used_returns_not_found();                 /* FS-P4 */
    test_fan_set_gpio_pwm_equals_tach_returns_invalid_arg_fan_intact();  /* FS-P5 */
    test_fan_set_gpio_new_pwm_out_of_range_fan_intact();                 /* FS-P6 */
    test_fan_set_gpio_new_pwm_reserved_fan_intact();                     /* FS-P7 */
    test_fan_set_gpio_new_pwm_foreign_fan_intact();                      /* FS-P8 */
    test_fan_set_gpio_new_tach_out_of_range_fan_intact();                /* FS-P9 */
    test_fan_set_gpio_new_tach_reserved_fan_intact();                    /* FS-P10 */
    test_fan_set_gpio_new_tach_foreign_fan_intact();                     /* FS-P11 */
    test_fan_set_gpio_identity_succeeds_reclaims_same_pins();            /* FS-P12 */
    test_fan_set_gpio_pwm_only_change_reclaims();                        /* FS-P13 */
    test_fan_set_gpio_tach_only_change_reclaims();                       /* FS-P14 */
    test_fan_set_gpio_full_swap_succeeds();                              /* FS-P15 */
    test_fan_set_gpio_both_new_pins_succeeds();                          /* FS-P16 */
    test_fan_set_gpio_tach_removed_releases_tach();                      /* FS-P17 */
    test_fan_set_gpio_tach_added_claims_new_tach();                      /* FS-P18 */
    test_fan_set_gpio_pwm_claim_injected_failure_returns_error();        /* FS-P19 */
    test_fan_set_gpio_tach_claim_injected_failure_releases_new_pwm();    /* FS-P20 */

    /* f_fan_remove */
    test_fan_remove_null_args_returns_invalid_arg(); /* FR-P1 */
    test_fan_remove_unused_slot_returns_not_found(); /* FR-P2 */
    test_fan_remove_no_tach_releases_pwm();          /* FR-P3 */
    test_fan_remove_with_tach_releases_both();       /* FR-P4 */

    /* f_source_add */
    test_source_add_null_args_returns_invalid_arg();           /* SA-P1 */
    test_source_add_registry_full_returns_no_mem();            /* SA-P2 */
    test_source_add_ntc_claims_adc_succeeds();                 /* SA-P3 */
    test_source_add_ntc_sentinel_gpio_skips_claim();           /* SA-P4 */
    test_source_add_manual_skips_claim();                      /* SA-P5 */
    test_source_add_ntc_reserved_returns_invalid_arg();        /* SA-P6 */
    test_source_add_ntc_out_of_range_returns_invalid_arg();    /* SA-P7 */
    test_source_add_ntc_foreign_claim_returns_invalid_state(); /* SA-P8 */

    /* f_source_remove */
    test_source_remove_null_args_returns_invalid_arg(); /* SR-P1 */
    test_source_remove_unused_slot_returns_not_found(); /* SR-P2 */
    test_source_remove_ntc_releases_adc();              /* SR-P3 */
    test_source_remove_ntc_sentinel_no_release();       /* SR-P4 */
    test_source_remove_manual_no_release();             /* SR-P5 */

    /* f_ds18b20_init */
    test_ds18b20_init_null_handle_returns_invalid_arg();            /* DI-P1 */
    test_ds18b20_init_calloc_failure_returns_no_mem_handle_unset(); /* DI-P2 */
    test_ds18b20_init_success_claims_onwire();                      /* DI-P3 */
    test_ds18b20_init_reserved_cleans_up_handle_unset();            /* DI-P4 */
    test_ds18b20_init_out_of_range_cleans_up_handle_unset();        /* DI-P5 */
    test_ds18b20_init_foreign_claim_cleans_up_handle_unset();       /* DI-P6 */

    /* f_source_update_manual */
    test_source_update_manual_null_handle_returns_invalid_arg();      /* SU-P1 */
    test_source_update_manual_id_out_of_range_returns_invalid_arg();  /* SU-P2 */
    test_source_update_manual_temp_below_min_rejected_before_write(); /* SU-P3 */
    test_source_update_manual_temp_above_max_rejected_before_write(); /* SU-P4 */
    test_source_update_manual_unused_slot_returns_not_found();        /* SU-P5 */
    test_source_update_manual_non_manual_type_rejected();             /* SU-P6 */
    test_source_update_manual_happy_path_writes_and_marks_valid();    /* SU-P7 */
    test_source_update_manual_accepts_min_boundary();                 /* SU-P8 */
    test_source_update_manual_accepts_max_boundary();                 /* SU-P9 */

    /* f_constraints_temp_c */
    test_constraints_temp_c_in_range_returns_ok();           /* CT-1 */
    test_constraints_temp_c_below_min_returns_invalid_arg(); /* CT-2 */
    test_constraints_temp_c_above_max_returns_invalid_arg(); /* CT-3 */

    printf("\nRESULT: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
