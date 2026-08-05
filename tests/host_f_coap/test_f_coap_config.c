/*
 * Host-based C unit tests for handle_config_get / coap_free_config_data
 * (components/f_coap/f_coap_routes.c, spec-01 phase-3) and
 * handle_config_post (spec-02 phase-4).
 *
 * The real f_coap_routes.c is compiled with `static` demoted so the
 * functions under test are externally reachable, against stubbed ESP-IDF
 * layers (see stubs/) and a fake coap3/coap.h, plus real nanopb + espfm.pb.c
 * and espfm_conv.c.  GNU ld --wrap hooks intercept f_config_export_all,
 * f_config_import_all, coap_add_data_large_response, coap_add_data,
 * coap_get_data, coap_pdu_set_code, coap_resource_get_userdata,
 * esp_timer_create, esp_timer_start_once, esp_err_to_name, calloc and free so
 * each EPA execution path can be driven deterministically.
 * -ffunction-sections/--gc-sections drop the other (unrelated) handlers at
 * link time, so no wifi/mdns/ds18b20 definitions are needed.
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

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "coap3/coap.h"

#include "f_config.h"
#include "espfm.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

/* The (gc'd) system-reboot handler uses ESP_ERROR_CHECK; give it a no-op so
 * the whole TU compiles on host. */
#ifndef ESP_ERROR_CHECK
#define ESP_ERROR_CHECK(x) \
    do {                   \
        (void)(x);         \
    } while (0)
#endif

/* ================================================================
 * Real source under test, with `static` demoted.  All the headers the source
 * includes are #pragma-once guarded and already included above, so this only
 * demotes the source's own static functions/variables.
 * ================================================================ */
#define static
#include "../../components/f_coap/f_coap_routes.c"
#undef static

/* ================================================================
 * --wrap target declarations (resolved by GNU ld --wrap)
 * ================================================================ */

extern void *__real_calloc(size_t n, size_t s);
extern void __real_free(void *p);

/* ================================================================
 * Test-hook globals (controlled per test)
 * ================================================================ */

static esp_err_t g_export_err; /* return value of f_config_export_all */
static uint8_t *g_export_buf;  /* buffer handed back via *buf_out */
static size_t g_export_len;    /* byte length handed back via *len_out */

static int g_adlr_calls;
static int g_adlr_ret;              /* coap_add_data_large_response return value */
static int g_adlr_simulate_failure; /* mirror libcoap: set 5.00 + release once, return 0 */
static int g_adlr_simulate_release; /* success + release via callback (blockwise) */
static void *g_adlr_resource;
static void *g_adlr_session;
static const void *g_adlr_req;
static void *g_adlr_resp;
static const coap_string_t *g_adlr_query;
static uint16_t g_adlr_media_type;
static int g_adlr_maxage;
static uint64_t g_adlr_etag;
static size_t g_adlr_length;
static const uint8_t *g_adlr_data;
static coap_release_large_data_t g_adlr_release_func;
static void *g_adlr_app_ptr;

static uint32_t g_last_code; /* code recorded by coap_pdu_set_code */
static int g_net_allocs;     /* live wrapped allocations (calloc - free) */
static int g_calloc_calls;

static struct f_coap g_fake_h; /* userdata returned by coap_resource_get_userdata */

/* handle_config_post test hooks (spec-02 phase-4) */
static esp_err_t g_import_err;                /* f_config_import_all return value */
static const char *g_import_err_msg;          /* *err_msg written by the wrap */
static int g_import_calls;                    /* number of f_config_import_all calls */
static f_config_handle_t g_import_config;     /* recorded handle arg */
static f_fan_handle_t g_import_fan;           /* recorded fan arg */
static f_source_handle_t g_import_source;     /* recorded source arg */
static f_curve_handle_t g_import_curve;       /* recorded curve arg */
static f_schedule_handle_t g_import_schedule; /* recorded schedule arg */
static const ConfigFile *g_import_cfg_ptr;    /* recorded cfg pointer */

static int g_cgd_ret;             /* coap_get_data return value */
static size_t g_cgd_len;          /* coap_get_data *len */
static const uint8_t *g_cgd_data; /* coap_get_data *data */
static int g_cgd_calls;           /* number of coap_get_data calls */

static int g_cad_calls;                         /* number of coap_add_data calls */
static size_t g_cad_len;                        /* captured body byte length */
static uint8_t g_cad_data[StatusResponse_size]; /* captured body bytes (copied) */

/* --- fan/source claim-wiring CoAP handler hooks (gpio phases 2-5) --- */

static int g_config_save_all_calls; /* f_config_save_all invocations (save_config) */
static f_config_handle_t g_config_save_handle;
static f_fan_handle_t g_config_save_fan;
static f_source_handle_t g_config_save_source;

static int g_fan_add_calls;
static esp_err_t g_fan_add_err;
static const char *g_fan_add_err_msg;
static uint8_t g_fan_add_pwm;
static uint8_t g_fan_add_tach;
static char g_fan_add_name[16];

static int g_set_gpio_calls;
static esp_err_t g_set_gpio_err;
static const char *g_set_gpio_err_msg;
static uint8_t g_set_gpio_pwm;
static uint8_t g_set_gpio_tach;

static int g_fan_get_info_calls;
static esp_err_t g_get_info_err; /* shared f_fan_get_info / f_source_get_info return */
static f_fan_info_t g_fan_info;  /* configurable info returned by f_fan_get_info */

static int g_fan_set_name_calls;
static int g_fan_set_mode_calls;
static int g_fan_set_duty_calls;
static int g_fan_set_source_calls;
static int g_fan_set_curve_calls;
static int g_fan_set_schedule_calls;
static int g_fan_set_group_calls;
static int g_fan_set_inverted_calls;
static int g_fan_set_enabled_calls;

static int g_source_add_calls;
static esp_err_t g_source_add_err;
static const char *g_source_add_err_msg;
static source_type_t g_source_add_type;
static uint8_t g_source_add_gpio;

static int g_source_add_ds18b20_calls;
static esp_err_t g_source_add_ds18b20_err;

static int g_source_get_info_calls;
static f_source_info_t g_source_info; /* configurable info returned by f_source_get_info */

static int g_update_manual_calls;
static esp_err_t g_update_manual_err;

/* --- control tunables handler hooks (ctrl phase-5) --- */

static esp_err_t g_ctrl_get_err;             /* f_control_get_tunables return value */
static int g_ctrl_get_calls;                 /* number of f_control_get_tunables calls */
static f_control_handle_t g_ctrl_get_handle; /* recorded handle arg */
static uint8_t g_ctrl_get_hyst;              /* current hysteresis returned by getter */
static uint8_t g_ctrl_get_up;                /* current ramp_up returned by getter */
static uint8_t g_ctrl_get_down;              /* current ramp_down returned by getter */
static failsafe_policy_t g_ctrl_get_policy;  /* current policy returned by getter */
static uint8_t g_ctrl_get_safe_duty;         /* current safe_duty returned by getter */

static int g_set_hyst_calls;                    /* f_control_set_hysteresis invocations */
static uint8_t g_set_hyst_arg;                  /* recorded hysteresis arg */
static int g_set_ramp_calls;                    /* f_control_set_ramp_rates invocations */
static uint8_t g_set_ramp_up;                   /* recorded ramp_up arg */
static uint8_t g_set_ramp_down;                 /* recorded ramp_down arg */
static int g_set_failsafe_calls;                /* f_control_set_failsafe invocations */
static failsafe_policy_t g_set_failsafe_policy; /* recorded policy arg */
static uint8_t g_set_failsafe_duty;             /* recorded safe_duty arg */

static coap_string_t g_uri_path_string; /* path returned by coap_get_uri_path */

/* Call-ordering signal: g_event_seq is the global call counter; g_cad_seq /
 * g_etso_seq snapshot it in __wrap_coap_add_data / __wrap_esp_timer_start_once
 * so tests can assert the 2.04 encode happens BEFORE the reboot timer starts. */
static uint32_t g_event_seq;
static uint32_t g_cad_seq;
static uint32_t g_etso_seq;

static int g_etc_calls;                     /* number of esp_timer_create calls */
static esp_err_t g_etc_ret;                 /* esp_timer_create return value */
static esp_timer_create_args_t g_etc_args;  /* copied *args */
static esp_timer_handle_t g_etc_out_handle; /* *out_handle written by the wrap */

static int g_etso_calls;                 /* number of esp_timer_start_once calls */
static esp_err_t g_etso_ret;             /* esp_timer_start_once return value */
static esp_timer_handle_t g_etso_handle; /* recorded timer arg */
static uint64_t g_etso_timeout_us;       /* recorded timeout_us arg */

static const char *g_e2n;       /* canned esp_err_to_name result */
static int g_esp_restart_calls; /* number of esp_restart() invocations */
static int g_log_calls;         /* total __test_log invocations */
static int g_log_err_calls;     /* __test_log invocations at level 'E' */

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

esp_err_t __wrap_f_config_export_all(f_fan_handle_t fan, f_source_handle_t source,
                                     f_curve_handle_t curve, f_schedule_handle_t schedule,
                                     uint8_t **buf_out, size_t *len_out)
{
    (void)fan;
    (void)source;
    (void)curve;
    (void)schedule;
    if (buf_out) *buf_out = g_export_buf;
    if (len_out) *len_out = g_export_len;
    return g_export_err;
}

int __wrap_coap_add_data_large_response(coap_resource_t *resource, coap_session_t *session,
                                        const coap_pdu_t *request, coap_pdu_t *response,
                                        const coap_string_t *query, uint16_t media_type, int maxage,
                                        uint64_t etag, size_t length, const uint8_t *data,
                                        coap_release_large_data_t release_func, void *app_ptr)
{
    g_adlr_calls++;
    g_adlr_resource     = resource;
    g_adlr_session      = session;
    g_adlr_req          = request;
    g_adlr_resp         = response;
    g_adlr_query        = query;
    g_adlr_media_type   = media_type;
    g_adlr_maxage       = maxage;
    g_adlr_etag         = etag;
    g_adlr_length       = length;
    g_adlr_data         = data;
    g_adlr_release_func = release_func;
    g_adlr_app_ptr      = app_ptr;

    if (g_adlr_simulate_failure) {
        /* Mirror libcoap's failure contract: set 5.00 and release the buffer
         * exactly once, then return 0 (the handler must not double-free). */
        coap_pdu_set_code(response, COAP_RESPONSE_CODE_INTERNAL_ERROR);
        if (release_func) release_func(session, app_ptr);
        return 0;
    }
    if (g_adlr_simulate_release && release_func) {
        release_func(session, app_ptr); /* simulate post-transmission release */
    }
    return g_adlr_ret;
}

void __wrap_coap_pdu_set_code(coap_pdu_t *pdu, coap_pdu_code_t code)
{
    (void)pdu;
    g_last_code = (uint32_t)code;
}

void *__wrap_coap_resource_get_userdata(coap_resource_t *resource)
{
    (void)resource;
    return &g_fake_h;
}

esp_err_t __wrap_f_config_import_all(f_config_handle_t handle, f_fan_handle_t fan,
                                     f_source_handle_t source, f_curve_handle_t curve,
                                     f_schedule_handle_t schedule, const ConfigFile *cfg,
                                     const char **err_msg)
{
    g_import_calls++;
    g_import_config   = handle;
    g_import_fan      = fan;
    g_import_source   = source;
    g_import_curve    = curve;
    g_import_schedule = schedule;
    g_import_cfg_ptr  = cfg;
    if (err_msg) *err_msg = g_import_err_msg;
    return g_import_err;
}

int __wrap_coap_get_data(const coap_pdu_t *pdu, size_t *len, const uint8_t **data)
{
    (void)pdu;
    g_cgd_calls++;
    if (len) *len = g_cgd_len;
    if (data) *data = g_cgd_data;
    return g_cgd_ret;
}

int __wrap_coap_add_data(coap_pdu_t *pdu, size_t len, const uint8_t *data)
{
    (void)pdu;
    g_cad_calls++;
    g_cad_len = len;
    if (len > sizeof(g_cad_data)) len = sizeof(g_cad_data);
    if (len && data) memcpy(g_cad_data, data, len);
    g_cad_seq = ++g_event_seq;
    return 1;
}

esp_err_t __wrap_esp_timer_create(const esp_timer_create_args_t *args,
                                  esp_timer_handle_t *out_handle)
{
    g_etc_calls++;
    if (args) g_etc_args = *args;
    if (out_handle) *out_handle = g_etc_out_handle;
    return g_etc_ret;
}

esp_err_t __wrap_esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us)
{
    g_etso_calls++;
    g_etso_handle     = timer;
    g_etso_timeout_us = timeout_us;
    g_etso_seq        = ++g_event_seq;
    return g_etso_ret;
}

const char *__wrap_esp_err_to_name(esp_err_t code)
{
    (void)code;
    return g_e2n;
}

/* Stub esp_log.h forwards every ESP_LOG* macro here; count and capture. */
static char g_last_log[256];
static char g_last_log_level;

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

/* reboot_timer_cb (kept by gc-sections via handle_config_post) calls
 * esp_restart() when the 2s one-shot fires — never during a handler call. */
void esp_restart(void)
{
    g_esp_restart_calls++;
}

/* ---- fan/source registry + save_config wraps (gpio claim-wiring handlers) ---- */

esp_err_t __wrap_f_config_save_all(f_config_handle_t handle, f_fan_handle_t fan,
                                   f_source_handle_t source, f_curve_handle_t curve,
                                   f_schedule_handle_t schedule)
{
    (void)curve;
    (void)schedule;
    g_config_save_all_calls++;
    g_config_save_handle = handle;
    g_config_save_fan    = fan;
    g_config_save_source = source;
    return ESP_OK;
}

coap_string_t *__wrap_coap_get_uri_path(const coap_pdu_t *request)
{
    (void)request;
    return &g_uri_path_string;
}

void __wrap_coap_delete_string(coap_string_t *string)
{
    (void)string; /* no-op: g_uri_path_string is a static the test owns */
}

esp_err_t __wrap_f_fan_add(f_fan_handle_t handle, uint8_t pwm_gpio, uint8_t tach_gpio,
                           const char *name, uint8_t *id_out, const char **err_msg)
{
    g_fan_add_calls++;
    if (handle == NULL) return ESP_ERR_INVALID_ARG; /* err_msg NOT written (HFP-P3) */
    g_fan_add_pwm  = pwm_gpio;
    g_fan_add_tach = tach_gpio;
    if (name) {
        strncpy(g_fan_add_name, name, sizeof(g_fan_add_name) - 1);
        g_fan_add_name[sizeof(g_fan_add_name) - 1] = '\0';
    }
    if (err_msg) *err_msg = g_fan_add_err_msg;
    if (g_fan_add_err != ESP_OK) return g_fan_add_err;
    if (id_out) *id_out = 0;
    return ESP_OK;
}

esp_err_t __wrap_f_fan_set_gpio(f_fan_handle_t handle, uint8_t id, uint8_t new_pwm_gpio,
                                uint8_t new_tach_gpio, const char **err_msg)
{
    (void)id;
    g_set_gpio_calls++;
    if (handle == NULL) return ESP_ERR_INVALID_ARG; /* err_msg NOT written (HFU-P7) */
    g_set_gpio_pwm  = new_pwm_gpio;
    g_set_gpio_tach = new_tach_gpio;
    if (err_msg) *err_msg = g_set_gpio_err_msg;
    if (g_set_gpio_err != ESP_OK) return g_set_gpio_err;
    return ESP_OK;
}

esp_err_t __wrap_f_fan_get_info(f_fan_handle_t handle, uint8_t id, f_fan_info_t *info_out)
{
    (void)handle;
    (void)id;
    g_fan_get_info_calls++;
    if (g_get_info_err != ESP_OK) return g_get_info_err;
    if (info_out) *info_out = g_fan_info;
    return ESP_OK;
}

esp_err_t __wrap_f_fan_set_name(f_fan_handle_t handle, uint8_t id, const char *name)
{
    (void)handle;
    (void)id;
    (void)name;
    g_fan_set_name_calls++;
    return ESP_OK;
}

esp_err_t __wrap_f_fan_set_mode(f_fan_handle_t handle, uint8_t id, fan_mode_t mode)
{
    (void)handle;
    (void)id;
    (void)mode;
    g_fan_set_mode_calls++;
    return ESP_OK;
}

esp_err_t __wrap_f_fan_set_duty(f_fan_handle_t handle, uint8_t id, uint8_t duty)
{
    (void)handle;
    (void)id;
    (void)duty;
    g_fan_set_duty_calls++;
    return ESP_OK;
}

esp_err_t __wrap_f_fan_set_source(f_fan_handle_t handle, uint8_t id, uint8_t source_id)
{
    (void)handle;
    (void)id;
    (void)source_id;
    g_fan_set_source_calls++;
    return ESP_OK;
}

esp_err_t __wrap_f_fan_set_curve(f_fan_handle_t handle, uint8_t id, uint8_t curve_id)
{
    (void)handle;
    (void)id;
    (void)curve_id;
    g_fan_set_curve_calls++;
    return ESP_OK;
}

esp_err_t __wrap_f_fan_set_schedule(f_fan_handle_t handle, uint8_t id, uint8_t schedule_id)
{
    (void)handle;
    (void)id;
    (void)schedule_id;
    g_fan_set_schedule_calls++;
    return ESP_OK;
}

esp_err_t __wrap_f_fan_set_group(f_fan_handle_t handle, uint8_t id, uint8_t group_id)
{
    (void)handle;
    (void)id;
    (void)group_id;
    g_fan_set_group_calls++;
    return ESP_OK;
}

esp_err_t __wrap_f_fan_set_inverted(f_fan_handle_t handle, uint8_t id, bool inverted)
{
    (void)handle;
    (void)id;
    (void)inverted;
    g_fan_set_inverted_calls++;
    return ESP_OK;
}

esp_err_t __wrap_f_fan_set_enabled(f_fan_handle_t handle, uint8_t id, bool enabled)
{
    (void)handle;
    (void)id;
    (void)enabled;
    g_fan_set_enabled_calls++;
    return ESP_OK;
}

esp_err_t __wrap_f_fan_remove(f_fan_handle_t handle, uint8_t id)
{
    (void)handle;
    (void)id;
    return ESP_OK;
}

esp_err_t __wrap_f_source_add(f_source_handle_t handle, source_type_t type, uint8_t gpio,
                              const char *name, uint8_t *id_out, const char **err_msg)
{
    (void)name;
    g_source_add_calls++;
    if (handle == NULL) return ESP_ERR_INVALID_ARG; /* err_msg NOT written (HSP-P9) */
    g_source_add_type = type;
    g_source_add_gpio = gpio;
    if (err_msg) *err_msg = g_source_add_err_msg;
    if (g_source_add_err != ESP_OK) return g_source_add_err;
    if (id_out) *id_out = 0;
    return ESP_OK;
}

esp_err_t __wrap_f_source_add_ds18b20(f_source_handle_t handle, uint64_t rom_code, const char *name,
                                      uint8_t *id_out)
{
    (void)rom_code;
    (void)name;
    g_source_add_ds18b20_calls++;
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    if (g_source_add_ds18b20_err != ESP_OK) return g_source_add_ds18b20_err;
    if (id_out) *id_out = 0;
    return ESP_OK;
}

esp_err_t __wrap_f_source_get_info(f_source_handle_t handle, uint8_t id, f_source_info_t *info_out)
{
    (void)handle;
    (void)id;
    g_source_get_info_calls++;
    if (g_get_info_err != ESP_OK) return g_get_info_err;
    if (info_out) *info_out = g_source_info;
    return ESP_OK;
}

esp_err_t __wrap_f_source_update_manual(f_source_handle_t handle, uint8_t id, float temp_c)
{
    (void)handle;
    (void)id;
    (void)temp_c;
    g_update_manual_calls++;
    return g_update_manual_err;
}

esp_err_t __wrap_f_source_remove(f_source_handle_t handle, uint8_t id)
{
    (void)handle;
    (void)id;
    return ESP_OK;
}

/* ---- control tunables wraps (ctrl phase-5) ---- */

esp_err_t __wrap_f_control_get_tunables(f_control_handle_t handle, uint8_t *h, uint8_t *u,
                                        uint8_t *d, failsafe_policy_t *p, uint8_t *s)
{
    g_ctrl_get_calls++;
    g_ctrl_get_handle = handle;
    if (g_ctrl_get_err != ESP_OK) return g_ctrl_get_err;
    if (h) *h = g_ctrl_get_hyst;
    if (u) *u = g_ctrl_get_up;
    if (d) *d = g_ctrl_get_down;
    if (p) *p = g_ctrl_get_policy;
    if (s) *s = g_ctrl_get_safe_duty;
    return ESP_OK;
}

esp_err_t __wrap_f_control_set_hysteresis(f_control_handle_t h, uint8_t v)
{
    (void)h;
    g_set_hyst_calls++;
    g_set_hyst_arg = v;
    return ESP_OK;
}

esp_err_t __wrap_f_control_set_ramp_rates(f_control_handle_t h, uint8_t u, uint8_t d)
{
    (void)h;
    g_set_ramp_calls++;
    g_set_ramp_up   = u;
    g_set_ramp_down = d;
    return ESP_OK;
}

esp_err_t __wrap_f_control_set_failsafe(f_control_handle_t h, failsafe_policy_t p, uint8_t d)
{
    (void)h;
    g_set_failsafe_calls++;
    g_set_failsafe_policy = p;
    g_set_failsafe_duty   = d;
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

static void reset_test_state(void)
{
    g_export_err            = ESP_OK;
    g_export_buf            = NULL;
    g_export_len            = 0;
    g_adlr_calls            = 0;
    g_adlr_ret              = 1;
    g_adlr_simulate_failure = 0;
    g_adlr_simulate_release = 0;
    g_adlr_resource         = NULL;
    g_adlr_session          = NULL;
    g_adlr_req              = NULL;
    g_adlr_resp             = NULL;
    g_adlr_query            = NULL;
    g_adlr_media_type       = 0;
    g_adlr_maxage           = 0;
    g_adlr_etag             = 0;
    g_adlr_length           = 0;
    g_adlr_data             = NULL;
    g_adlr_release_func     = NULL;
    g_adlr_app_ptr          = NULL;
    g_last_code             = 0;
    g_net_allocs            = 0;
    g_calloc_calls          = 0;
    memset(&g_fake_h, 0, sizeof(g_fake_h));

    /* handle_config_post hooks */
    g_import_err      = ESP_OK;
    g_import_err_msg  = NULL;
    g_import_calls    = 0;
    g_import_config   = NULL;
    g_import_fan      = NULL;
    g_import_source   = NULL;
    g_import_curve    = NULL;
    g_import_schedule = NULL;
    g_import_cfg_ptr  = NULL;

    g_cgd_ret         = 1;
    g_cgd_len         = 0;
    g_cgd_data        = NULL;
    g_cgd_calls       = 0;

    g_cad_calls       = 0;
    g_cad_len         = 0;
    memset(g_cad_data, 0, sizeof(g_cad_data));

    g_etc_calls = 0;
    g_etc_ret   = ESP_OK;
    memset(&g_etc_args, 0, sizeof(g_etc_args));
    g_etc_out_handle    = (esp_timer_handle_t)0x1234;

    g_etso_calls        = 0;
    g_etso_ret          = ESP_OK;
    g_etso_handle       = NULL;
    g_etso_timeout_us   = 0;

    g_event_seq         = 0;
    g_cad_seq           = 0;
    g_etso_seq          = 0;

    g_e2n               = "ESP_FAIL";
    g_esp_restart_calls = 0;
    g_log_calls         = 0;
    g_log_err_calls     = 0;
    g_last_log[0]       = '\0';
    g_last_log_level    = '\0';

    /* fan/source claim-wiring handler hooks */
    g_config_save_all_calls = 0;
    g_config_save_handle    = NULL;
    g_config_save_fan       = NULL;
    g_config_save_source    = NULL;

    g_fan_add_calls         = 0;
    g_fan_add_err           = ESP_OK;
    g_fan_add_err_msg       = NULL;
    g_fan_add_pwm           = 0;
    g_fan_add_tach          = 0;
    g_fan_add_name[0]       = '\0';

    g_set_gpio_calls        = 0;
    g_set_gpio_err          = ESP_OK;
    g_set_gpio_err_msg      = NULL;
    g_set_gpio_pwm          = 0;
    g_set_gpio_tach         = 0;

    g_fan_get_info_calls    = 0;
    g_get_info_err          = ESP_OK;
    memset(&g_fan_info, 0, sizeof(g_fan_info));
    g_fan_info.tach_gpio       = 0xFF; /* default: no tach */
    g_fan_info.enabled         = true;

    g_fan_set_name_calls       = 0;
    g_fan_set_mode_calls       = 0;
    g_fan_set_duty_calls       = 0;
    g_fan_set_source_calls     = 0;
    g_fan_set_curve_calls      = 0;
    g_fan_set_schedule_calls   = 0;
    g_fan_set_group_calls      = 0;
    g_fan_set_inverted_calls   = 0;
    g_fan_set_enabled_calls    = 0;

    g_source_add_calls         = 0;
    g_source_add_err           = ESP_OK;
    g_source_add_err_msg       = NULL;
    g_source_add_type          = SOURCE_TYPE_NTC;
    g_source_add_gpio          = 0;
    g_source_add_ds18b20_calls = 0;
    g_source_add_ds18b20_err   = ESP_OK;
    g_source_get_info_calls    = 0;
    memset(&g_source_info, 0, sizeof(g_source_info));
    strcpy(g_source_info.name, "s0");
    g_source_info.type    = SOURCE_TYPE_MANUAL;
    g_update_manual_calls = 0;
    g_update_manual_err   = ESP_OK;

    /* control tunables handler hooks */
    g_ctrl_get_err        = ESP_OK;
    g_ctrl_get_calls      = 0;
    g_ctrl_get_handle     = NULL;
    g_ctrl_get_hyst       = 3;  /* match f_control_init DEFAULT_HYSTERESIS */
    g_ctrl_get_up         = 10; /* match f_control_init DEFAULT_RAMP_UP */
    g_ctrl_get_down       = 3;  /* match f_control_init DEFAULT_RAMP_DOWN */
    g_ctrl_get_policy     = FAILSAFE_HOLD;
    g_ctrl_get_safe_duty  = 50; /* match f_control_init DEFAULT_SAFE_DUTY */

    g_set_hyst_calls      = 0;
    g_set_hyst_arg        = 0;
    g_set_ramp_calls      = 0;
    g_set_ramp_up         = 0;
    g_set_ramp_down       = 0;
    g_set_failsafe_calls  = 0;
    g_set_failsafe_policy = FAILSAFE_HOLD;
    g_set_failsafe_duty   = 0;

    memset(&g_uri_path_string, 0, sizeof(g_uri_path_string));

    /* Demoted file-static reboot machinery (f_coap_routes.c). */
    s_reboot_pending = false;
    s_reboot_timer   = NULL;
}

/* Fake opaque endpoint handles — the handler only forwards them to the wrapped
 * f_config_export_all and coap_add_data_large_response, never dereferences. */
#define H_FAN ((f_fan_handle_t)0x1)
#define H_SRC ((f_source_handle_t)0x1)
#define H_CUR ((f_curve_handle_t)0x1)
#define H_SCH ((f_schedule_handle_t)0x1)

/* Dummy lvalue endpoints passed to the handler. */
static void setup_endpoints(coap_resource_t *resource, coap_session_t *session, coap_pdu_t *req,
                            coap_string_t *query, coap_pdu_t *resp)
{
    memset(resource, 0, sizeof(*resource));
    memset(session, 0, sizeof(*session));
    memset(req, 0, sizeof(*req));
    memset(query, 0, sizeof(*query));
    memset(resp, 0, sizeof(*resp));
}

static size_t encode_config(const ConfigFile *cfg, uint8_t *buf, size_t bufsz)
{
    pb_ostream_t os = pb_ostream_from_buffer(buf, bufsz);
    if (!pb_encode(&os, &ConfigFile_msg, cfg)) return 0;
    return os.bytes_written;
}

/* Heap-allocate g_export_buf from a nanopb-encoded ConfigFile (wrapped calloc
 * bumps g_net_allocs to 1). */
static void set_export_from_cfg(const ConfigFile *cfg)
{
    uint8_t enc[ConfigFile_size];
    size_t n     = encode_config(cfg, enc, sizeof(enc));
    g_export_buf = calloc(1, n);
    g_export_len = n;
    memcpy(g_export_buf, enc, n);
}

static void install_nonnull_handles(void)
{
    g_fake_h.fan      = H_FAN;
    g_fake_h.source   = H_SRC;
    g_fake_h.curve    = H_CUR;
    g_fake_h.schedule = H_SCH;
    g_fake_h.control  = (f_control_handle_t)0xABC;
}

/* Decode the captured response body (g_cad_data/g_cad_len) as a StatusResponse. */
static bool decode_status(StatusResponse *out)
{
    pb_istream_t is = pb_istream_from_buffer(g_cad_data, g_cad_len);
    return pb_decode(&is, &StatusResponse_msg, out);
}

/* Encode a non-empty ConfigFile (1 fan + 1 source) into buf; returns bytes. */
static size_t build_body_nonempty(uint8_t *buf, size_t bufsz)
{
    ConfigFile cfg = ConfigFile_init_default;
    strcpy(cfg.version, "3.0");
    cfg.has_fans        = true;
    cfg.fans.fans_count = 1;
    strcpy(cfg.fans.fans[0].name, "f0");
    cfg.fans.fans[0].pwm_gpio = 15;
    cfg.has_sources           = true;
    cfg.sources.sources_count = 1;
    strcpy(cfg.sources.sources[0].name, "s0");
    return encode_config(&cfg, buf, bufsz);
}

/* Encode an EMPTY ConfigFile (all lists zero-count) into buf; returns bytes. */
static size_t build_body_empty(uint8_t *buf, size_t bufsz)
{
    ConfigFile cfg = ConfigFile_init_default;
    strcpy(cfg.version, "3.0");
    cfg.has_fans      = true; /* fans_count stays 0 */
    cfg.has_sources   = true; /* sources_count stays 0 */
    cfg.has_curves    = true; /* curves_count stays 0 */
    cfg.has_schedules = false;
    return encode_config(&cfg, buf, bufsz);
}

/* ---- claim-wiring handler helpers (gpio phases 2-5) ---- */

/* Point __wrap_coap_get_uri_path at a string literal path. */
static void set_uri_path(const char *path)
{
    g_uri_path_string.s      = (uint8_t *)(uintptr_t)path;
    g_uri_path_string.length = strlen(path);
}

static size_t encode_req(const pb_msgdesc_t *desc, const void *msg, uint8_t *buf, size_t bufsz)
{
    pb_ostream_t os = pb_ostream_from_buffer(buf, bufsz);
    if (!pb_encode(&os, desc, msg)) return 0;
    return os.bytes_written;
}

static size_t build_fan_create_req(uint8_t *buf, size_t bufsz, uint32_t pwm, uint32_t tach,
                                   const char *name, bool has_duty, uint32_t duty)
{
    FanCreateRequest cr = FanCreateRequest_init_default;
    cr.pwm_gpio         = pwm;
    cr.tach_gpio        = tach;
    cr.has_duty         = has_duty;
    cr.duty             = duty;
    cr.has_name         = true;
    strncpy(cr.name, name, sizeof(cr.name) - 1);
    cr.name[sizeof(cr.name) - 1] = '\0';
    return encode_req(&FanCreateRequest_msg, &cr, buf, bufsz);
}

static size_t build_fan_update_req(uint8_t *buf, size_t bufsz, bool has_pwm, uint32_t pwm,
                                   bool has_tach, uint32_t tach, bool has_duty, uint32_t duty)
{
    FanUpdateRequest ur = FanUpdateRequest_init_default;
    ur.id               = 1; /* non-zero so the id field always encodes (n > 0) */
    ur.has_pwm_gpio     = has_pwm;
    ur.pwm_gpio         = pwm;
    ur.has_tach_gpio    = has_tach;
    ur.tach_gpio        = tach;
    ur.has_duty         = has_duty;
    ur.duty             = duty;
    return encode_req(&FanUpdateRequest_msg, &ur, buf, bufsz);
}

static size_t build_source_create_req(uint8_t *buf, size_t bufsz, SourceType type, uint32_t gpio,
                                      uint64_t rom, const char *name)
{
    SourceCreateRequest cr = SourceCreateRequest_init_default;
    cr.type                = type;
    cr.gpio                = gpio;
    cr.ds18b20_rom_code    = rom;
    strncpy(cr.name, name, sizeof(cr.name) - 1);
    cr.name[sizeof(cr.name) - 1] = '\0';
    return encode_req(&SourceCreateRequest_msg, &cr, buf, bufsz);
}

static size_t build_manual_temp_req(uint8_t *buf, size_t bufsz, uint32_t id, float temp_c)
{
    ManualTempRequest mtr = ManualTempRequest_init_default;
    mtr.id                = id;
    mtr.temp_c            = temp_c;
    return encode_req(&ManualTempRequest_msg, &mtr, buf, bufsz);
}

/* Decode the captured response body (g_cad_data/g_cad_len) as FanInfo. */
static bool decode_fan_info(FanInfo *out)
{
    pb_istream_t is = pb_istream_from_buffer(g_cad_data, g_cad_len);
    return pb_decode(&is, &FanInfo_msg, out);
}

/* Decode the captured response body as SourceInfo. */
static bool decode_source_info(SourceInfo *out)
{
    pb_istream_t is = pb_istream_from_buffer(g_cad_data, g_cad_len);
    return pb_decode(&is, &SourceInfo_msg, out);
}

/* ---- control tunables handler helpers (ctrl phase-5) ---- */

/* Encode a ControlConfig PUT body with per-field presence flags. */
static size_t build_control_req(uint8_t *buf, size_t bufsz, bool h_hyst, uint32_t hyst, bool h_up,
                                uint32_t up, bool h_down, uint32_t down, bool h_pol,
                                FailsafePolicy pol, bool h_duty, uint32_t duty)
{
    ControlConfig cc       = ControlConfig_init_default;
    cc.has_hysteresis      = h_hyst;
    cc.hysteresis          = hyst;
    cc.has_ramp_up         = h_up;
    cc.ramp_up             = up;
    cc.has_ramp_down       = h_down;
    cc.ramp_down           = down;
    cc.has_failsafe_policy = h_pol;
    cc.failsafe_policy     = pol;
    cc.has_safe_duty       = h_duty;
    cc.safe_duty           = duty;
    return encode_req(&ControlConfig_msg, &cc, buf, bufsz);
}

/* Decode the captured response body (g_cad_data/g_cad_len) as ControlConfig. */
static bool decode_control(ControlConfig *out)
{
    pb_istream_t is = pb_istream_from_buffer(g_cad_data, g_cad_len);
    return pb_decode(&is, &ControlConfig_msg, out);
}

/* ================================================================
 * handle_config_get tests
 * ================================================================ */

/* P1 — f_config_export_all failure -> 5.00, no large-response call, no leak */
static void test_config_get_export_failure_returns_500(void)
{
    reset_test_state();
    install_nonnull_handles();
    g_export_err = ESP_FAIL;
    g_export_buf = NULL;
    g_export_len = 0;
    g_adlr_ret   = 1; /* must not be reached */

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);

    handle_config_get(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_INTERNAL_ERROR);
    CHECK(g_adlr_calls == 0);
    CHECK(g_net_allocs == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P2 — export success + large-response success (single datagram): 2.05 and the
 * full buffer is handed to libcoap with the release callback; handler does not
 * free the buffer. */
static void test_config_get_success_2_05_passes_buffer_to_libcoap(void)
{
    reset_test_state();
    install_nonnull_handles();
    g_export_err   = ESP_OK;
    g_adlr_ret     = 1;

    ConfigFile cfg = ConfigFile_init_default;
    strcpy(cfg.version, "3.0");
    cfg.has_fans              = true;
    cfg.fans.fans_count       = 2;
    cfg.has_sources           = true;
    cfg.sources.sources_count = 1;
    set_export_from_cfg(&cfg);

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);

    handle_config_get(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_CONTENT);
    CHECK(g_adlr_calls == 1);
    CHECK(g_adlr_media_type == COAP_MEDIATYPE_APPLICATION_OCTET_STREAM);
    CHECK(g_adlr_maxage == -1);
    CHECK(g_adlr_etag == 0);
    CHECK(g_adlr_length == g_export_len);
    CHECK(g_adlr_data == g_export_buf);
    CHECK(g_adlr_release_func == coap_free_config_data);
    CHECK(g_adlr_app_ptr == g_export_buf);
    CHECK(g_net_allocs == 1); /* handler did NOT free the buffer */

    /* Simulate libcoap releasing the buffer after transmission. */
    coap_free_config_data(&session, g_export_buf);
    CHECK(g_net_allocs == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P3 — export success with empty registries: payload decodes to empty lists. */
static void test_config_get_success_empty_registries_empty_lists(void)
{
    reset_test_state();
    install_nonnull_handles();
    g_export_err   = ESP_OK;
    g_adlr_ret     = 1;

    ConfigFile cfg = ConfigFile_init_default;
    strcpy(cfg.version, "3.0");
    cfg.has_fans      = true; /* fans_count stays 0 */
    cfg.has_sources   = true; /* sources_count stays 0 */
    cfg.has_curves    = true; /* curves_count stays 0 */
    cfg.has_schedules = false;
    set_export_from_cfg(&cfg);

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);

    handle_config_get(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_CONTENT);
    CHECK(g_adlr_calls == 1);
    CHECK(g_adlr_media_type == COAP_MEDIATYPE_APPLICATION_OCTET_STREAM);
    CHECK(g_adlr_release_func == coap_free_config_data);
    CHECK(g_adlr_app_ptr == g_export_buf);

    ConfigFile got;
    memset(&got, 0, sizeof(got));
    pb_istream_t is = pb_istream_from_buffer(g_adlr_data, g_adlr_length);
    CHECK(pb_decode(&is, &ConfigFile_msg, &got));
    CHECK(strcmp(got.version, "3.0") == 0);
    CHECK(got.has_fans && got.fans.fans_count == 0);
    CHECK(got.has_sources && got.sources.sources_count == 0);
    CHECK(got.has_curves && got.curves.curves_count == 0);
    CHECK(!got.has_schedules && got.schedules.schedules_count == 0);

    coap_free_config_data(&session, g_export_buf);
    CHECK(g_net_allocs == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P4 — export success with partial registries: decoded counts match used slots. */
static void test_config_get_success_partial_registries_counts_match(void)
{
    reset_test_state();
    install_nonnull_handles();
    g_export_err   = ESP_OK;
    g_adlr_ret     = 1;

    ConfigFile cfg = ConfigFile_init_default;
    strcpy(cfg.version, "3.0");
    cfg.has_fans                  = true;
    cfg.fans.fans_count           = 3;
    cfg.has_sources               = true; /* sources_count stays 0 */
    cfg.has_curves                = true;
    cfg.curves.curves_count       = 2;
    cfg.has_schedules             = true;
    cfg.schedules.schedules_count = 1;
    set_export_from_cfg(&cfg);

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);

    handle_config_get(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_CONTENT);
    CHECK(g_adlr_calls == 1);

    ConfigFile got;
    memset(&got, 0, sizeof(got));
    pb_istream_t is = pb_istream_from_buffer(g_adlr_data, g_adlr_length);
    CHECK(pb_decode(&is, &ConfigFile_msg, &got));
    CHECK(strcmp(got.version, "3.0") == 0);
    CHECK(got.fans.fans_count == 3);
    CHECK(got.sources.sources_count == 0);
    CHECK(got.curves.curves_count == 2);
    CHECK(got.schedules.schedules_count == 1);
    CHECK(got.has_schedules == true);

    coap_free_config_data(&session, g_export_buf);
    CHECK(g_net_allocs == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P5 — fully-loaded/oversized config: the ENTIRE buffer is handed to libcoap
 * (enabling Block2/Size2/ETag); the callback frees it after transmission. */
static void test_config_get_success_fully_loaded_passes_full_buffer_for_block2(void)
{
    reset_test_state();
    install_nonnull_handles();
    g_export_err            = ESP_OK;
    g_export_buf            = calloc(1, 2500); /* > COAP_MTU 1280, blockwise handoff */
    g_export_len            = 2500;
    g_adlr_ret              = 1;
    g_adlr_simulate_release = 1; /* wrapper invokes release_func after recording */

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);

    handle_config_get(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_CONTENT);
    CHECK(g_adlr_calls == 1);
    CHECK(g_adlr_length == 2500); /* full encoded length, not truncated */
    CHECK(g_adlr_data == g_export_buf);
    CHECK(g_adlr_release_func == coap_free_config_data);
    CHECK(g_adlr_app_ptr == g_export_buf);
    CHECK(g_net_allocs == 0); /* freed via the callback during the simulated release */
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P6 — large-response failure: libcoap (simulated) set 5.00 and freed the
 * buffer once; the handler must not double-free and must not overwrite 5.00. */
static void test_config_get_large_response_failure_no_double_free(void)
{
    reset_test_state();
    install_nonnull_handles();
    g_export_err            = ESP_OK;
    g_export_buf            = calloc(1, 64);
    g_export_len            = 64;
    g_adlr_ret              = 0;
    g_adlr_simulate_failure = 1;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);

    handle_config_get(&resource, &session, &req, &query, &resp);

    CHECK(g_adlr_calls == 1);
    CHECK(g_last_code == COAP_RESPONSE_CODE_INTERNAL_ERROR); /* libcoap 5.00 kept */
    CHECK(g_net_allocs == 0); /* freed exactly once, no double-free */
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* ================================================================
 * coap_free_config_data tests
 * ================================================================ */

/* P7 — release callback frees the non-NULL buffer exactly once. */
static void test_coap_free_config_data_frees_buffer(void)
{
    reset_test_state();
    uint8_t *ptr = calloc(1, 64);
    CHECK(g_net_allocs == 1);

    coap_session_t session;
    memset(&session, 0, sizeof(session));
    coap_free_config_data(&session, ptr);
    CHECK(g_net_allocs == 0); /* wrapped free called once on ptr */
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P8 — release callback with NULL app_ptr is a defensive no-op. */
static void test_coap_free_config_data_null_is_noop(void)
{
    reset_test_state();
    coap_session_t session;
    memset(&session, 0, sizeof(session));
    coap_free_config_data(&session, NULL);
    CHECK(g_net_allocs == 0); /* free(NULL) is a no-op, no crash */
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* ================================================================
 * handle_config_post tests (spec-02 phase-4)
 * ================================================================ */

/* P1 — decode failure with no body (coap_get_data returns 0) or a
 * zero-length body (the len == 0 arm of decode_request L77): 4.00 with an
 * empty body; f_config_import_all is never reached; no timer ops; no reboot. */
static void test_config_post_decode_failure_empty_body_returns_400(void)
{
    static uint8_t body[ConfigFile_size];

    /* Sub-case (a): coap_get_data returns 0 — no body at all. */
    reset_test_state();
    install_nonnull_handles();
    g_fake_h.config = (f_config_handle_t)0x1;
    g_cgd_ret       = 0;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_config_post(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_BAD_REQUEST);
    CHECK(g_cad_calls == 0);
    CHECK(g_import_calls == 0);
    CHECK(g_etc_calls == 0);
    CHECK(g_etso_calls == 0);
    CHECK(s_reboot_pending == false);

    /* Sub-case (b): body present but zero length (L77 len == 0 arm). */
    reset_test_state();
    install_nonnull_handles();
    g_fake_h.config = (f_config_handle_t)0x1;
    g_cgd_ret       = 1;
    g_cgd_len       = 0;
    g_cgd_data      = body;

    handle_config_post(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_BAD_REQUEST);
    CHECK(g_cad_calls == 0);
    CHECK(g_import_calls == 0);
    CHECK(g_etc_calls == 0);
    CHECK(g_etso_calls == 0);
    CHECK(s_reboot_pending == false);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P2 — decode failure with a malformed/truncated protobuf body (pb_decode
 * returns false): 4.00 with an empty body; import not reached; no reboot. */
static void test_config_post_decode_failure_malformed_body_returns_400(void)
{
    static const uint8_t malformed[5] = {0x0A, 0xFF, 0xFF, 0xFF, 0xFF};

    reset_test_state();
    install_nonnull_handles();
    g_fake_h.config = (f_config_handle_t)0x1;
    g_cgd_ret       = 1;
    g_cgd_len       = sizeof(malformed);
    g_cgd_data      = malformed;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_config_post(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_BAD_REQUEST);
    CHECK(g_cad_calls == 0);
    CHECK(g_import_calls == 0);
    CHECK(g_etc_calls == 0);
    CHECK(g_etso_calls == 0);
    CHECK(s_reboot_pending == false);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P3 — import validation failure with err_msg set: 4.00
 * StatusResponse{ok=false, error_code=ESP_ERR_INVALID_ARG, error_msg=<msg>};
 * no mutation, no reboot. */
static void test_config_post_validation_failure_returns_400_with_error_msg(void)
{
    static uint8_t body[ConfigFile_size];
    size_t n = build_body_nonempty(body, sizeof(body));
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    g_fake_h.config  = (f_config_handle_t)0x1;
    g_import_err     = ESP_ERR_INVALID_ARG;
    g_import_err_msg = "invalid gpio";
    g_cgd_ret        = 1;
    g_cgd_len        = n;
    g_cgd_data       = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_config_post(&resource, &session, &req, &query, &resp);

    CHECK(g_import_calls == 1);
    CHECK(g_import_config == g_fake_h.config);
    CHECK(g_import_fan == H_FAN);
    CHECK(g_import_source == H_SRC);
    CHECK(g_import_curve == H_CUR);
    CHECK(g_import_schedule == H_SCH);
    CHECK(g_import_cfg_ptr != NULL);
    CHECK(g_last_code == COAP_RESPONSE_CODE_BAD_REQUEST);
    CHECK(g_cad_calls == 1);
    StatusResponse sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(decode_status(&sr));
    CHECK(sr.ok == false);
    CHECK(sr.error_code == (uint32_t)ESP_ERR_INVALID_ARG);
    CHECK(strcmp(sr.error_msg, "invalid gpio") == 0);
    CHECK(g_etso_calls == 0);
    CHECK(s_reboot_pending == false);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P4 — import validation failure with err_msg left NULL (top-level guard,
 * h->config == NULL): 4.00 StatusResponse{ok=false, error_code=INVALID_ARG,
 * error_msg=""} — the L912 err_msg guard stays false. */
static void test_config_post_validation_failure_no_err_msg_returns_400_empty_error(void)
{
    static uint8_t body[ConfigFile_size];
    size_t n = build_body_nonempty(body, sizeof(body));
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    g_fake_h.config  = NULL; /* drives f_config_import_all L537 top-level guard */
    g_import_err     = ESP_ERR_INVALID_ARG;
    g_import_err_msg = NULL;
    g_cgd_ret        = 1;
    g_cgd_len        = n;
    g_cgd_data       = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_config_post(&resource, &session, &req, &query, &resp);

    CHECK(g_import_calls == 1);
    CHECK(g_last_code == COAP_RESPONSE_CODE_BAD_REQUEST);
    CHECK(g_cad_calls == 1);
    StatusResponse sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(decode_status(&sr));
    CHECK(sr.ok == false);
    CHECK(sr.error_code == (uint32_t)ESP_ERR_INVALID_ARG);
    CHECK(sr.error_msg[0] == '\0');
    CHECK(g_etso_calls == 0);
    CHECK(s_reboot_pending == false);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P5 — import persist/apply failure (any non-ESP_OK, non-INVALID_ARG): 5.00
 * StatusResponse{ok=false, error_code=0, error_msg="persist failed: <name>"};
 * no reboot. */
static void test_config_post_persist_failure_returns_500(void)
{
    static uint8_t body[ConfigFile_size];
    size_t n = build_body_nonempty(body, sizeof(body));
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    g_fake_h.config  = (f_config_handle_t)0x1;
    g_import_err     = ESP_FAIL;
    g_import_err_msg = NULL; /* irrelevant on this branch */
    g_e2n            = "ESP_FAIL";
    g_cgd_ret        = 1;
    g_cgd_len        = n;
    g_cgd_data       = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_config_post(&resource, &session, &req, &query, &resp);

    CHECK(g_import_calls == 1);
    CHECK(g_last_code == COAP_RESPONSE_CODE_INTERNAL_ERROR);
    CHECK(g_cad_calls == 1);
    StatusResponse sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(decode_status(&sr));
    CHECK(sr.ok == false);
    CHECK(sr.error_code == 0); /* NOT set on this branch */
    CHECK(strcmp(sr.error_msg, "persist failed: ESP_FAIL") == 0);
    CHECK(g_etso_calls == 0);
    CHECK(s_reboot_pending == false);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P6 — success + first-ever reboot: 2.04 StatusResponse{ok=true} encoded
 * FIRST, then timer created once and started once with 2,000,000 us;
 * s_reboot_pending set true. */
static void test_config_post_success_schedules_reboot_timer_created(void)
{
    static uint8_t body[ConfigFile_size];
    size_t n = build_body_nonempty(body, sizeof(body));
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    g_fake_h.config  = (f_config_handle_t)0x1;
    g_import_err     = ESP_OK;
    g_cgd_ret        = 1;
    g_cgd_len        = n;
    g_cgd_data       = body;
    s_reboot_pending = false;
    s_reboot_timer   = NULL;
    g_etc_ret        = ESP_OK;
    g_etc_out_handle = (esp_timer_handle_t)0x1234;
    g_etso_ret       = ESP_OK;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_config_post(&resource, &session, &req, &query, &resp);

    CHECK(g_import_calls == 1);
    CHECK(g_last_code == COAP_RESPONSE_CODE_CHANGED);
    CHECK(g_cad_calls == 1);
    StatusResponse sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(decode_status(&sr));
    CHECK(sr.ok == true);
    CHECK(g_etc_calls == 1);
    CHECK(g_etc_args.callback == reboot_timer_cb);
    CHECK(g_etc_args.name != NULL && strcmp(g_etc_args.name, "coap_reboot") == 0);
    CHECK(g_etso_calls == 1);
    CHECK(g_etso_handle == (esp_timer_handle_t)0x1234);
    CHECK(g_etso_timeout_us == 2000000);
    CHECK(g_cad_seq != 0 && g_etso_seq != 0 && g_cad_seq < g_etso_seq);
    CHECK(s_reboot_pending == true);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P7 — success with an existing timer: 2.04 ok=true, timer reused (create
 * skipped), start_once called once with the existing handle, pending set. */
static void test_config_post_success_reuses_existing_timer(void)
{
    static uint8_t body[ConfigFile_size];
    size_t n = build_body_nonempty(body, sizeof(body));
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    g_fake_h.config  = (f_config_handle_t)0x1;
    g_import_err     = ESP_OK;
    g_cgd_ret        = 1;
    g_cgd_len        = n;
    g_cgd_data       = body;
    s_reboot_pending = false;
    s_reboot_timer   = (esp_timer_handle_t)0x5678;
    g_etso_ret       = ESP_OK;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_config_post(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_CHANGED);
    CHECK(g_cad_calls == 1);
    StatusResponse sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(decode_status(&sr));
    CHECK(sr.ok == true);
    CHECK(g_etc_calls == 0);
    CHECK(g_etso_calls == 1);
    CHECK(g_etso_handle == (esp_timer_handle_t)0x5678);
    CHECK(g_etso_timeout_us == 2000000);
    CHECK(s_reboot_pending == true);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P8 — success but timer start FAILS: 2.04 ok=true still returned (encoded
 * before the timer block), no reboot scheduled (pending stays false), ESP_LOGE
 * emitted. */
static void test_config_post_success_timer_start_failure_no_pending(void)
{
    static uint8_t body[ConfigFile_size];
    size_t n = build_body_nonempty(body, sizeof(body));
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    g_fake_h.config  = (f_config_handle_t)0x1;
    g_import_err     = ESP_OK;
    g_cgd_ret        = 1;
    g_cgd_len        = n;
    g_cgd_data       = body;
    s_reboot_pending = false;
    s_reboot_timer   = (esp_timer_handle_t)0x5678; /* isolate start failure */
    g_etso_ret       = ESP_ERR_INVALID_STATE;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_config_post(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_CHANGED); /* encoded before timer block */
    CHECK(g_cad_calls == 1);
    StatusResponse sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(decode_status(&sr));
    CHECK(sr.ok == true);
    CHECK(g_etc_calls == 0);
    CHECK(g_etso_calls == 1);
    CHECK(g_etso_timeout_us == 2000000);
    CHECK(s_reboot_pending == false); /* L939 false */
    CHECK(g_log_err_calls == 1);      /* ESP_LOGE at L942 */
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P9 — success with a reboot ALREADY pending: 2.04 ok=true, the entire timer
 * block is skipped (no create, no start), pending stays true. */
static void test_config_post_success_reboot_already_pending_skips_timer(void)
{
    static uint8_t body[ConfigFile_size];
    size_t n = build_body_nonempty(body, sizeof(body));
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    g_fake_h.config  = (f_config_handle_t)0x1;
    g_import_err     = ESP_OK;
    g_cgd_ret        = 1;
    g_cgd_len        = n;
    g_cgd_data       = body;
    s_reboot_pending = true;
    s_reboot_timer   = (esp_timer_handle_t)0x5678;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_config_post(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_CHANGED);
    CHECK(g_cad_calls == 1);
    StatusResponse sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(decode_status(&sr));
    CHECK(sr.ok == true);
    CHECK(g_etc_calls == 0);
    CHECK(g_etso_calls == 0);        /* whole timer block skipped (L930 false) */
    CHECK(s_reboot_pending == true); /* unchanged */
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* P10 — success with an EMPTY ConfigFile: the handler forwards the freshly
 * re-initialized (empty) cfg to f_config_import_all, still returns 2.04 ok=true
 * and schedules the reboot. Reads the forwarded cfg counts directly. */
static void test_config_post_success_empty_config_clears_and_reboots(void)
{
    static uint8_t body[ConfigFile_size];
    size_t n = build_body_empty(body, sizeof(body));
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    g_fake_h.config  = (f_config_handle_t)0x1;
    g_import_err     = ESP_OK;
    g_cgd_ret        = 1;
    g_cgd_len        = n;
    g_cgd_data       = body;
    s_reboot_pending = false;
    s_reboot_timer   = NULL;
    g_etc_ret        = ESP_OK;
    g_etc_out_handle = (esp_timer_handle_t)0x1234;
    g_etso_ret       = ESP_OK;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_config_post(&resource, &session, &req, &query, &resp);

    /* Read the forwarded cfg pointer immediately (stack frame is dead after
     * the handler returns; no intervening calls before these loads). */
    CHECK(g_import_cfg_ptr != NULL);
    pb_size_t nfans   = g_import_cfg_ptr->fans.fans_count;
    pb_size_t nsrcs   = g_import_cfg_ptr->sources.sources_count;
    pb_size_t ncurves = g_import_cfg_ptr->curves.curves_count;
    pb_size_t nschs   = g_import_cfg_ptr->schedules.schedules_count;
    CHECK(nfans == 0 && nsrcs == 0 && ncurves == 0 && nschs == 0);

    CHECK(g_last_code == COAP_RESPONSE_CODE_CHANGED);
    CHECK(g_cad_calls == 1);
    StatusResponse sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(decode_status(&sr));
    CHECK(sr.ok == true);
    CHECK(g_etc_calls == 1);
    CHECK(g_etso_calls == 1);
    CHECK(g_etso_timeout_us == 2000000);
    CHECK(s_reboot_pending == true);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* ================================================================
 * handle_fan_post tests (HFP-P1..P4)
 * ================================================================ */

/* HFP-P1 — decode failure (no body): 4.00, f_fan_add not reached, no body. */
static void test_fan_post_decode_failure_returns_400(void)
{
    reset_test_state();
    install_nonnull_handles();
    g_cgd_ret = 0;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_fan_post(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_BAD_REQUEST);
    CHECK(g_fan_add_calls == 0);
    CHECK(g_cad_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HFP-P2 — f_fan_add fails with err_msg: 4.00 StatusResponse{ok=false,
 * error_code, error_msg}; save_config NOT called. */
static void test_fan_post_add_failure_returns_400_with_error_msg(void)
{
    static uint8_t body[FanCreateRequest_size];
    size_t n = build_fan_create_req(body, sizeof(body), 15, 0xFF, "f", false, 0);
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    g_fake_h.config   = (f_config_handle_t)0x1;
    g_fan_add_err     = ESP_ERR_INVALID_STATE;
    g_fan_add_err_msg = "GPIO 15 already in use by PWM";
    g_cgd_ret         = 1;
    g_cgd_len         = n;
    g_cgd_data        = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_fan_post(&resource, &session, &req, &query, &resp);

    CHECK(g_fan_add_calls == 1);
    CHECK(g_fan_add_pwm == 15);
    CHECK(g_last_code == COAP_RESPONSE_CODE_BAD_REQUEST);
    CHECK(g_cad_calls == 1);
    StatusResponse sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(decode_status(&sr));
    CHECK(sr.ok == false);
    CHECK(sr.error_code == (uint32_t)ESP_ERR_INVALID_STATE);
    CHECK(strcmp(sr.error_msg, "GPIO 15 already in use by PWM") == 0);
    CHECK(g_config_save_all_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HFP-P3 — f_fan_add fails, err_msg NULL (h->fan NULL): 4.00 StatusResponse
 * with error_msg empty; save_config NOT called. */
static void test_fan_post_add_failure_no_err_msg_returns_400_empty_error(void)
{
    static uint8_t body[FanCreateRequest_size];
    size_t n = build_fan_create_req(body, sizeof(body), 15, 0xFF, "f", false, 0);
    CHECK(n > 0);

    reset_test_state();                  /* g_fake_h.fan stays NULL */
    g_fan_add_err = ESP_ERR_INVALID_ARG; /* stub null-guards, no msg written */
    g_cgd_ret     = 1;
    g_cgd_len     = n;
    g_cgd_data    = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_fan_post(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_BAD_REQUEST);
    CHECK(g_cad_calls == 1);
    StatusResponse sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(decode_status(&sr));
    CHECK(sr.ok == false);
    CHECK(sr.error_code == (uint32_t)ESP_ERR_INVALID_ARG);
    CHECK(sr.error_msg[0] == '\0');
    CHECK(g_config_save_all_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HFP-P4 — success: save_config called once, 2.01 FanInfo body, set_duty applied. */
static void test_fan_post_success_returns_201_fan_info(void)
{
    static uint8_t body[FanCreateRequest_size];
    size_t n = build_fan_create_req(body, sizeof(body), 15, 0xFF, "f", true, 50);
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    g_fake_h.config = (f_config_handle_t)0x1;
    g_fan_add_err   = ESP_OK;
    strcpy(g_fan_info.name, "f");
    g_fan_info.pwm_gpio  = 15;
    g_fan_info.tach_gpio = 0xFF;
    g_fan_info.duty      = 50;
    g_cgd_ret            = 1;
    g_cgd_len            = n;
    g_cgd_data           = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_fan_post(&resource, &session, &req, &query, &resp);

    CHECK(g_fan_add_calls == 1);
    CHECK(g_fan_set_duty_calls == 1);
    CHECK(g_config_save_all_calls == 1);
    CHECK(g_last_code == COAP_RESPONSE_CODE_CREATED);
    CHECK(g_cad_calls == 1);
    FanInfo fi;
    memset(&fi, 0, sizeof(fi));
    CHECK(decode_fan_info(&fi));
    CHECK(fi.pwm_gpio == 15);
    CHECK(strcmp(fi.name, "f") == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* ================================================================
 * handle_fan_put tests (HFU-P1..P7)
 * ================================================================ */

/* HFU-P1 — short path (/fans): 4.04, decode_request not called. */
static void test_fan_put_short_path_returns_404(void)
{
    reset_test_state();
    install_nonnull_handles();
    set_uri_path("/fans");
    g_cgd_ret = 1;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_fan_put(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_NOT_FOUND);
    CHECK(g_cgd_calls == 0); /* parse_segments failed before decode_request */
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HFU-P2 — decode failure: 4.00, f_fan_get_info not called. */
static void test_fan_put_decode_failure_returns_400(void)
{
    reset_test_state();
    install_nonnull_handles();
    set_uri_path("/fans/0");
    g_cgd_ret = 0;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_fan_put(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_BAD_REQUEST);
    CHECK(g_fan_get_info_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HFU-P3 — fan does not exist: 4.04, f_fan_set_gpio not called. */
static void test_fan_put_fan_not_found_returns_404(void)
{
    static uint8_t body[FanUpdateRequest_size];
    size_t n = build_fan_update_req(body, sizeof(body), false, 0, false, 0, false, 0);
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    set_uri_path("/fans/0");
    g_get_info_err = ESP_ERR_NOT_FOUND;
    g_cgd_ret      = 1;
    g_cgd_len      = n;
    g_cgd_data     = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_fan_put(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_NOT_FOUND);
    CHECK(g_set_gpio_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HFU-P4 — no GPIO fields: f_fan_set_gpio NOT called; save + 2.04 FanInfo. */
static void test_fan_put_no_gpio_fields_skips_set_gpio(void)
{
    static uint8_t body[FanUpdateRequest_size];
    size_t n = build_fan_update_req(body, sizeof(body), false, 0, false, 0, true, 50);
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    g_fake_h.config = (f_config_handle_t)0x1;
    set_uri_path("/fans/0");
    g_get_info_err  = ESP_OK;
    g_fan_info.duty = 50;
    g_cgd_ret       = 1;
    g_cgd_len       = n;
    g_cgd_data      = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_fan_put(&resource, &session, &req, &query, &resp);

    CHECK(g_set_gpio_calls == 0);
    CHECK(g_config_save_all_calls == 1);
    CHECK(g_last_code == COAP_RESPONSE_CODE_CHANGED);
    CHECK(g_cad_calls == 1);
    FanInfo fi;
    memset(&fi, 0, sizeof(fi));
    CHECK(decode_fan_info(&fi));
    CHECK(fi.duty == 50);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HFU-P5 — GPIO update succeeds: f_fan_set_gpio called, save + 2.04. */
static void test_fan_put_gpio_update_success_returns_204(void)
{
    static uint8_t body[FanUpdateRequest_size];
    size_t n = build_fan_update_req(body, sizeof(body), true, 20, false, 0, false, 0);
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    g_fake_h.config = (f_config_handle_t)0x1;
    set_uri_path("/fans/0");
    g_get_info_err = ESP_OK; /* g_fan_info default: pwm 15, tach 0xFF */
    g_set_gpio_err = ESP_OK;
    g_cgd_ret      = 1;
    g_cgd_len      = n;
    g_cgd_data     = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_fan_put(&resource, &session, &req, &query, &resp);

    CHECK(g_set_gpio_calls == 1);
    CHECK(g_set_gpio_pwm == 20);
    CHECK(g_set_gpio_tach == 0xFF); /* merged from existing info */
    CHECK(g_config_save_all_calls == 1);
    CHECK(g_last_code == COAP_RESPONSE_CODE_CHANGED);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HFU-P6 — GPIO update fails with err_msg: 4.00 StatusResponse, save NOT called. */
static void test_fan_put_gpio_update_failure_returns_400_with_error_msg_no_save(void)
{
    static uint8_t body[FanUpdateRequest_size];
    size_t n = build_fan_update_req(body, sizeof(body), true, 20, false, 0, false, 0);
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    g_fake_h.config = (f_config_handle_t)0x1;
    set_uri_path("/fans/0");
    g_get_info_err     = ESP_OK;
    g_set_gpio_err     = ESP_ERR_INVALID_STATE;
    g_set_gpio_err_msg = "GPIO 20 already in use by ADC";
    g_cgd_ret          = 1;
    g_cgd_len          = n;
    g_cgd_data         = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_fan_put(&resource, &session, &req, &query, &resp);

    CHECK(g_set_gpio_calls == 1);
    CHECK(g_last_code == COAP_RESPONSE_CODE_BAD_REQUEST);
    CHECK(g_cad_calls == 1);
    StatusResponse sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(decode_status(&sr));
    CHECK(sr.ok == false);
    CHECK(sr.error_code == (uint32_t)ESP_ERR_INVALID_STATE);
    CHECK(strcmp(sr.error_msg, "GPIO 20 already in use by ADC") == 0);
    CHECK(g_config_save_all_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HFU-P7 — GPIO update fails, err_msg NULL (h->fan NULL): 4.00 empty error. */
static void test_fan_put_gpio_update_failure_no_err_msg_returns_400_empty_error(void)
{
    static uint8_t body[FanUpdateRequest_size];
    size_t n = build_fan_update_req(body, sizeof(body), true, 20, false, 0, false, 0);
    CHECK(n > 0);

    reset_test_state(); /* g_fake_h.fan stays NULL */
    g_fake_h.config = (f_config_handle_t)0x1;
    set_uri_path("/fans/0");
    g_get_info_err = ESP_OK;              /* stub returns info even with NULL fan */
    g_set_gpio_err = ESP_ERR_INVALID_ARG; /* stub null-guards, no msg */
    g_cgd_ret      = 1;
    g_cgd_len      = n;
    g_cgd_data     = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_fan_put(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_BAD_REQUEST);
    CHECK(g_cad_calls == 1);
    StatusResponse sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(decode_status(&sr));
    CHECK(sr.ok == false);
    CHECK(sr.error_msg[0] == '\0');
    CHECK(g_config_save_all_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* ================================================================
 * handle_source_post tests (HSP-P1..P10)
 * ================================================================ */

/* HSP-P1 — /sources/temp decode failure: 4.00. */
static void test_source_post_temp_decode_failure_returns_400(void)
{
    reset_test_state();
    install_nonnull_handles();
    set_uri_path("/sources/temp");
    g_cgd_ret = 0;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_source_post(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_BAD_REQUEST);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HSP-P2 — /sources/temp source missing: 4.04. */
static void test_source_post_temp_source_not_found_returns_404(void)
{
    static uint8_t body[ManualTempRequest_size];
    size_t n = build_manual_temp_req(body, sizeof(body), 0, 20.0f);
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    set_uri_path("/sources/temp");
    g_get_info_err = ESP_ERR_NOT_FOUND;
    g_cgd_ret      = 1;
    g_cgd_len      = n;
    g_cgd_data     = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_source_post(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_NOT_FOUND);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HSP-P3 — /sources/temp update fails: 4.00, no StatusResponse body. */
static void test_source_post_temp_update_failure_returns_400(void)
{
    static uint8_t body[ManualTempRequest_size];
    size_t n = build_manual_temp_req(body, sizeof(body), 0, 20.0f);
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    set_uri_path("/sources/temp");
    g_get_info_err      = ESP_OK;
    g_update_manual_err = ESP_ERR_INVALID_ARG;
    g_cgd_ret           = 1;
    g_cgd_len           = n;
    g_cgd_data          = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_source_post(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_BAD_REQUEST);
    CHECK(g_cad_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HSP-P4 — /sources/temp success: 2.04 StatusResponse ok==true. */
static void test_source_post_temp_success_returns_204_ok(void)
{
    static uint8_t body[ManualTempRequest_size];
    size_t n = build_manual_temp_req(body, sizeof(body), 0, 20.0f);
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    set_uri_path("/sources/temp");
    g_get_info_err      = ESP_OK;
    g_update_manual_err = ESP_OK;
    g_cgd_ret           = 1;
    g_cgd_len           = n;
    g_cgd_data          = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_source_post(&resource, &session, &req, &query, &resp);

    CHECK(g_update_manual_calls == 1);
    CHECK(g_last_code == COAP_RESPONSE_CODE_CHANGED);
    CHECK(g_cad_calls == 1);
    StatusResponse sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(decode_status(&sr));
    CHECK(sr.ok == true);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HSP-P5 — /sources create decode failure: 4.00, no f_source_add. */
static void test_source_post_create_decode_failure_returns_400(void)
{
    reset_test_state();
    install_nonnull_handles();
    set_uri_path("/sources");
    g_cgd_ret = 0;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_source_post(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_BAD_REQUEST);
    CHECK(g_source_add_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HSP-P6 — DS18B20 create fails: 4.00, NO StatusResponse body. */
static void test_source_post_ds18b20_add_failure_returns_400_no_status_body(void)
{
    static uint8_t body[SourceCreateRequest_size];
    size_t n =
        build_source_create_req(body, sizeof(body), SourceType_SOURCE_TYPE_DS18B20, 0, 0x1234, "d");
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    set_uri_path("/sources");
    g_source_add_ds18b20_err = ESP_ERR_NO_MEM;
    g_cgd_ret                = 1;
    g_cgd_len                = n;
    g_cgd_data               = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_source_post(&resource, &session, &req, &query, &resp);

    CHECK(g_source_add_ds18b20_calls == 1);
    CHECK(g_last_code == COAP_RESPONSE_CODE_BAD_REQUEST);
    CHECK(g_cad_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HSP-P7 — DS18B20 create succeeds: 2.01, save_config called, SourceInfo body. */
static void test_source_post_ds18b20_add_success_returns_201(void)
{
    static uint8_t body[SourceCreateRequest_size];
    size_t n =
        build_source_create_req(body, sizeof(body), SourceType_SOURCE_TYPE_DS18B20, 0, 0x1234, "d");
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    g_fake_h.config = (f_config_handle_t)0x1;
    set_uri_path("/sources");
    g_source_add_ds18b20_err = ESP_OK;
    strcpy(g_source_info.name, "d");
    g_source_info.type = SOURCE_TYPE_DS18B20;
    g_cgd_ret          = 1;
    g_cgd_len          = n;
    g_cgd_data         = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_source_post(&resource, &session, &req, &query, &resp);

    CHECK(g_source_add_ds18b20_calls == 1);
    CHECK(g_config_save_all_calls == 1);
    CHECK(g_last_code == COAP_RESPONSE_CODE_CREATED);
    CHECK(g_cad_calls == 1);
    SourceInfo si;
    memset(&si, 0, sizeof(si));
    CHECK(decode_source_info(&si));
    CHECK(strcmp(si.name, "d") == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HSP-P8 — NTC create fails with err_msg: 4.00 StatusResponse, no save. */
static void test_source_post_ntc_add_failure_returns_400_with_error_msg(void)
{
    static uint8_t body[SourceCreateRequest_size];
    size_t n = build_source_create_req(body, sizeof(body), SourceType_SOURCE_TYPE_NTC, 15, 0, "s");
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    g_fake_h.config = (f_config_handle_t)0x1;
    set_uri_path("/sources");
    g_source_add_err     = ESP_ERR_INVALID_STATE;
    g_source_add_err_msg = "GPIO 15 already in use by PWM";
    g_cgd_ret            = 1;
    g_cgd_len            = n;
    g_cgd_data           = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_source_post(&resource, &session, &req, &query, &resp);

    CHECK(g_source_add_calls == 1);
    CHECK(g_source_add_type == SOURCE_TYPE_NTC);
    CHECK(g_source_add_gpio == 15);
    CHECK(g_last_code == COAP_RESPONSE_CODE_BAD_REQUEST);
    CHECK(g_cad_calls == 1);
    StatusResponse sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(decode_status(&sr));
    CHECK(sr.ok == false);
    CHECK(sr.error_code == (uint32_t)ESP_ERR_INVALID_STATE);
    CHECK(strcmp(sr.error_msg, "GPIO 15 already in use by PWM") == 0);
    CHECK(g_config_save_all_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HSP-P9 — NTC create fails, err_msg NULL (h->source NULL): 4.00 empty error. */
static void test_source_post_ntc_add_failure_no_err_msg_returns_400_empty_error(void)
{
    static uint8_t body[SourceCreateRequest_size];
    size_t n = build_source_create_req(body, sizeof(body), SourceType_SOURCE_TYPE_NTC, 15, 0, "s");
    CHECK(n > 0);

    reset_test_state(); /* g_fake_h.source stays NULL */
    set_uri_path("/sources");
    g_source_add_err = ESP_ERR_INVALID_ARG; /* stub null-guards, no msg */
    g_cgd_ret        = 1;
    g_cgd_len        = n;
    g_cgd_data       = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_source_post(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_BAD_REQUEST);
    CHECK(g_cad_calls == 1);
    StatusResponse sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(decode_status(&sr));
    CHECK(sr.ok == false);
    CHECK(sr.error_msg[0] == '\0');
    CHECK(g_config_save_all_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HSP-P10 — MANUAL create succeeds: 2.01, save_config called, SourceInfo body. */
static void test_source_post_manual_add_success_returns_201(void)
{
    static uint8_t body[SourceCreateRequest_size];
    size_t n =
        build_source_create_req(body, sizeof(body), SourceType_SOURCE_TYPE_MANUAL, 15, 0, "s");
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    g_fake_h.config = (f_config_handle_t)0x1;
    set_uri_path("/sources");
    g_source_add_err = ESP_OK;
    strcpy(g_source_info.name, "s");
    g_source_info.type = SOURCE_TYPE_MANUAL;
    g_cgd_ret          = 1;
    g_cgd_len          = n;
    g_cgd_data         = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_source_post(&resource, &session, &req, &query, &resp);

    CHECK(g_source_add_calls == 1);
    CHECK(g_source_add_type == SOURCE_TYPE_MANUAL);
    CHECK(g_config_save_all_calls == 1);
    CHECK(g_last_code == COAP_RESPONSE_CODE_CREATED);
    CHECK(g_cad_calls == 1);
    SourceInfo si;
    memset(&si, 0, sizeof(si));
    CHECK(decode_source_info(&si));
    CHECK(strcmp(si.name, "s") == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* ================================================================
 * handle_control_get tests (HCG-P1..P3)
 * ================================================================ */

/* HCG-P1 — h->control == NULL: 5.03, getter NOT called, no body. */
static void test_control_get_null_control_returns_503(void)
{
    reset_test_state(); /* g_fake_h.control stays NULL */

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_control_get(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE);
    CHECK(g_ctrl_get_calls == 0);
    CHECK(g_cad_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HCG-P2 — getter error: 5.00, no body. */
static void test_control_get_tunables_failure_returns_500(void)
{
    reset_test_state();
    install_nonnull_handles();
    g_ctrl_get_err = ESP_FAIL;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_control_get(&resource, &session, &req, &query, &resp);

    CHECK(g_ctrl_get_calls == 1);
    CHECK(g_last_code == COAP_RESPONSE_CODE_INTERNAL_ERROR);
    CHECK(g_cad_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HCG-P3 — happy path: 2.05 with all five fields, presence flags true. */
static void test_control_get_success_returns_205_all_fields(void)
{
    reset_test_state();
    install_nonnull_handles();
    g_ctrl_get_err       = ESP_OK;
    g_ctrl_get_hyst      = 5;
    g_ctrl_get_up        = 15;
    g_ctrl_get_down      = 20;
    g_ctrl_get_policy    = FAILSAFE_FULL_SPEED;
    g_ctrl_get_safe_duty = 70;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_control_get(&resource, &session, &req, &query, &resp);

    CHECK(g_ctrl_get_calls == 1);
    CHECK(g_ctrl_get_handle == g_fake_h.control);
    CHECK(g_last_code == COAP_RESPONSE_CODE_CONTENT);
    CHECK(g_cad_calls == 1);
    ControlConfig cc;
    memset(&cc, 0, sizeof(cc));
    CHECK(decode_control(&cc));
    CHECK(cc.has_hysteresis && cc.hysteresis == 5);
    CHECK(cc.has_ramp_up && cc.ramp_up == 15);
    CHECK(cc.has_ramp_down && cc.ramp_down == 20);
    CHECK(cc.has_failsafe_policy && cc.failsafe_policy == FailsafePolicy_FAILSAFE_FULL_SPEED);
    CHECK(cc.has_safe_duty && cc.safe_duty == 70);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* ================================================================
 * handle_control_put tests (HCP-P1..P15)
 * ================================================================ */

/* HCP-P1 — h->control == NULL: 5.03, coap_get_data NOT called, no body. */
static void test_control_put_null_control_returns_503(void)
{
    reset_test_state(); /* g_fake_h.control stays NULL */

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_control_put(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE);
    CHECK(g_cgd_calls == 0);
    CHECK(g_cad_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HCP-P2 — decode failure (no body OR malformed protobuf): 4.00
 * StatusResponse{ok:false, error_code:INVALID_ARG, error_msg:"decode failed"};
 * getter and setters never reached. */
static void test_control_put_decode_failure_returns_400_decode_failed(void)
{
    /* Sub-case (a): coap_get_data returns 0 — no body at all. */
    reset_test_state();
    install_nonnull_handles();
    g_cgd_ret = 0;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_control_put(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_BAD_REQUEST);
    CHECK(g_cad_calls == 1);
    StatusResponse sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(decode_status(&sr));
    CHECK(sr.ok == false);
    CHECK(sr.error_code == (uint32_t)ESP_ERR_INVALID_ARG);
    CHECK(strcmp(sr.error_msg, "decode failed") == 0);
    CHECK(g_ctrl_get_calls == 0);
    CHECK(g_set_hyst_calls == 0 && g_set_ramp_calls == 0 && g_set_failsafe_calls == 0);

    /* Sub-case (b): malformed/truncated protobuf body (pb_decode false). */
    reset_test_state();
    install_nonnull_handles();
    static const uint8_t malformed[5] = {0x0A, 0xFF, 0xFF, 0xFF, 0xFF};
    g_cgd_ret                         = 1;
    g_cgd_len                         = sizeof(malformed);
    g_cgd_data                        = malformed;

    handle_control_put(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_BAD_REQUEST);
    CHECK(g_cad_calls == 1);
    memset(&sr, 0, sizeof(sr));
    CHECK(decode_status(&sr));
    CHECK(sr.ok == false);
    CHECK(strcmp(sr.error_msg, "decode failed") == 0);
    CHECK(g_ctrl_get_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HCP-P3 — merge-base getter error: 5.00, no body, no setter. */
static void test_control_put_merge_base_failure_returns_500(void)
{
    static uint8_t body[ControlConfig_size];
    size_t n =
        build_control_req(body, sizeof(body), true, 5, false, 0, false, 0, false, 0, false, 0);
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    g_ctrl_get_err = ESP_FAIL;
    g_cgd_ret      = 1;
    g_cgd_len      = n;
    g_cgd_data     = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_control_put(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_INTERNAL_ERROR);
    CHECK(g_cad_calls == 0);
    CHECK(g_set_hyst_calls == 0 && g_set_ramp_calls == 0 && g_set_failsafe_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HCP-P4 — hysteresis > 100: 4.00 "hysteresis out of range", no setter ran. */
static void test_control_put_hysteresis_oob_returns_400(void)
{
    static uint8_t body[ControlConfig_size];
    size_t n =
        build_control_req(body, sizeof(body), true, 101, false, 0, false, 0, false, 0, false, 0);
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    g_cgd_ret  = 1;
    g_cgd_len  = n;
    g_cgd_data = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_control_put(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_BAD_REQUEST);
    CHECK(g_cad_calls == 1);
    StatusResponse sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(decode_status(&sr));
    CHECK(sr.ok == false);
    CHECK(strcmp(sr.error_msg, "hysteresis out of range") == 0);
    CHECK(g_set_hyst_calls == 0 && g_set_ramp_calls == 0 && g_set_failsafe_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HCP-P5 — ramp_up > 100: 4.00 "ramp_up out of range", no setter ran. */
static void test_control_put_ramp_up_oob_returns_400(void)
{
    static uint8_t body[ControlConfig_size];
    size_t n =
        build_control_req(body, sizeof(body), false, 0, true, 101, false, 0, false, 0, false, 0);
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    g_cgd_ret  = 1;
    g_cgd_len  = n;
    g_cgd_data = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_control_put(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_BAD_REQUEST);
    CHECK(g_cad_calls == 1);
    StatusResponse sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(decode_status(&sr));
    CHECK(sr.ok == false);
    CHECK(strcmp(sr.error_msg, "ramp_up out of range") == 0);
    CHECK(g_set_hyst_calls == 0 && g_set_ramp_calls == 0 && g_set_failsafe_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HCP-P6 — ramp_down > 100: 4.00 "ramp_down out of range", no setter ran. */
static void test_control_put_ramp_down_oob_returns_400(void)
{
    static uint8_t body[ControlConfig_size];
    size_t n =
        build_control_req(body, sizeof(body), false, 0, false, 0, true, 101, false, 0, false, 0);
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    g_cgd_ret  = 1;
    g_cgd_len  = n;
    g_cgd_data = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_control_put(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_BAD_REQUEST);
    CHECK(g_cad_calls == 1);
    StatusResponse sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(decode_status(&sr));
    CHECK(sr.ok == false);
    CHECK(strcmp(sr.error_msg, "ramp_down out of range") == 0);
    CHECK(g_set_hyst_calls == 0 && g_set_ramp_calls == 0 && g_set_failsafe_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HCP-P7 — failsafe_policy outside enum 0-3: 4.00 "invalid failsafe policy". */
static void test_control_put_failsafe_policy_oob_returns_400(void)
{
    static uint8_t body[ControlConfig_size];
    size_t n = build_control_req(body, sizeof(body), false, 0, false, 0, false, 0, true,
                                 (FailsafePolicy)4, false, 0);
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    g_cgd_ret  = 1;
    g_cgd_len  = n;
    g_cgd_data = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_control_put(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_BAD_REQUEST);
    CHECK(g_cad_calls == 1);
    StatusResponse sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(decode_status(&sr));
    CHECK(sr.ok == false);
    CHECK(strcmp(sr.error_msg, "invalid failsafe policy") == 0);
    CHECK(g_set_hyst_calls == 0 && g_set_ramp_calls == 0 && g_set_failsafe_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HCP-P8 — safe_duty > 100: 4.00 "safe_duty out of range", no setter ran. */
static void test_control_put_safe_duty_oob_returns_400(void)
{
    static uint8_t body[ControlConfig_size];
    size_t n =
        build_control_req(body, sizeof(body), false, 0, false, 0, false, 0, false, 0, true, 101);
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    g_cgd_ret  = 1;
    g_cgd_len  = n;
    g_cgd_data = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_control_put(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_BAD_REQUEST);
    CHECK(g_cad_calls == 1);
    StatusResponse sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(decode_status(&sr));
    CHECK(sr.ok == false);
    CHECK(strcmp(sr.error_msg, "safe_duty out of range") == 0);
    CHECK(g_set_hyst_calls == 0 && g_set_ramp_calls == 0 && g_set_failsafe_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HCP-P9 — all five fields present + valid: 2.04 ok, all three setters once. */
static void test_control_put_all_fields_success_returns_204(void)
{
    static uint8_t body[ControlConfig_size];
    size_t n = build_control_req(body, sizeof(body), true, 5, true, 15, true, 20, true,
                                 (FailsafePolicy)2, true, 70);
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles();
    g_cgd_ret  = 1;
    g_cgd_len  = n;
    g_cgd_data = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_control_put(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_CHANGED);
    CHECK(g_cad_calls == 1);
    StatusResponse sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(decode_status(&sr));
    CHECK(sr.ok == true);
    CHECK(g_set_hyst_calls == 1 && g_set_hyst_arg == 5);
    CHECK(g_set_ramp_calls == 1 && g_set_ramp_up == 15 && g_set_ramp_down == 20);
    CHECK(g_set_failsafe_calls == 1 && g_set_failsafe_policy == FAILSAFE_SAFE_DUTY &&
          g_set_failsafe_duty == 70);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HCP-P10 — only hysteresis present: only set_hysteresis called with request. */
static void test_control_put_partial_hysteresis_only(void)
{
    static uint8_t body[ControlConfig_size];
    size_t n =
        build_control_req(body, sizeof(body), true, 7, false, 0, false, 0, false, 0, false, 0);
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles(); /* getter defaults: hyst=3, up=10, down=3, HOLD, 50 */
    g_cgd_ret  = 1;
    g_cgd_len  = n;
    g_cgd_data = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_control_put(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_CHANGED);
    CHECK(g_cad_calls == 1);
    StatusResponse sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(decode_status(&sr));
    CHECK(sr.ok == true);
    CHECK(g_set_hyst_calls == 1 && g_set_hyst_arg == 7);
    CHECK(g_set_ramp_calls == 0);
    CHECK(g_set_failsafe_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HCP-P11 — only ramp_up present: set_ramp_rates(request up, current getter down). */
static void test_control_put_partial_ramp_up_only(void)
{
    static uint8_t body[ControlConfig_size];
    size_t n =
        build_control_req(body, sizeof(body), false, 0, true, 25, false, 0, false, 0, false, 0);
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles(); /* getter down = 3 */
    g_cgd_ret  = 1;
    g_cgd_len  = n;
    g_cgd_data = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_control_put(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_CHANGED);
    CHECK(g_cad_calls == 1);
    CHECK(g_set_ramp_calls == 1 && g_set_ramp_up == 25 && g_set_ramp_down == 3);
    CHECK(g_set_hyst_calls == 0);
    CHECK(g_set_failsafe_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HCP-P12 — only ramp_down present: set_ramp_rates(current getter up, request down). */
static void test_control_put_partial_ramp_down_only(void)
{
    static uint8_t body[ControlConfig_size];
    size_t n =
        build_control_req(body, sizeof(body), false, 0, false, 0, true, 30, false, 0, false, 0);
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles(); /* getter up = 10 */
    g_cgd_ret  = 1;
    g_cgd_len  = n;
    g_cgd_data = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_control_put(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_CHANGED);
    CHECK(g_cad_calls == 1);
    CHECK(g_set_ramp_calls == 1 && g_set_ramp_up == 10 && g_set_ramp_down == 30);
    CHECK(g_set_hyst_calls == 0);
    CHECK(g_set_failsafe_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HCP-P13 — only failsafe_policy present: set_failsafe(request policy, current duty). */
static void test_control_put_partial_failsafe_policy_only(void)
{
    static uint8_t body[ControlConfig_size];
    size_t n = build_control_req(body, sizeof(body), false, 0, false, 0, false, 0, true,
                                 (FailsafePolicy)1, false, 0);
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles(); /* getter safe_duty = 50 */
    g_cgd_ret  = 1;
    g_cgd_len  = n;
    g_cgd_data = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_control_put(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_CHANGED);
    CHECK(g_cad_calls == 1);
    CHECK(g_set_failsafe_calls == 1 && g_set_failsafe_policy == FAILSAFE_FULL_SPEED &&
          g_set_failsafe_duty == 50);
    CHECK(g_set_hyst_calls == 0);
    CHECK(g_set_ramp_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HCP-P14 — only safe_duty present: set_failsafe(current getter policy, request duty). */
static void test_control_put_partial_safe_duty_only(void)
{
    static uint8_t body[ControlConfig_size];
    size_t n =
        build_control_req(body, sizeof(body), false, 0, false, 0, false, 0, false, 0, true, 60);
    CHECK(n > 0);

    reset_test_state();
    install_nonnull_handles(); /* getter policy = FAILSAFE_HOLD */
    g_cgd_ret  = 1;
    g_cgd_len  = n;
    g_cgd_data = body;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_control_put(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_CHANGED);
    CHECK(g_cad_calls == 1);
    CHECK(g_set_failsafe_calls == 1 && g_set_failsafe_policy == FAILSAFE_HOLD &&
          g_set_failsafe_duty == 60);
    CHECK(g_set_hyst_calls == 0);
    CHECK(g_set_ramp_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* HCP-P15 — body with only unrecognized fields decodes OK but has no has_*:
 * 2.04 no-op, no setter called. */
static void test_control_put_no_fields_noop_returns_204(void)
{
    static const uint8_t unknown_field[3] = {0x98, 0x06, 0x01}; /* field 99, varint 1 */

    reset_test_state();
    install_nonnull_handles();
    g_cgd_ret  = 1;
    g_cgd_len  = sizeof(unknown_field);
    g_cgd_data = unknown_field;

    static coap_resource_t resource;
    static coap_session_t session;
    static coap_pdu_t req;
    static coap_string_t query;
    static coap_pdu_t resp;
    setup_endpoints(&resource, &session, &req, &query, &resp);
    handle_control_put(&resource, &session, &req, &query, &resp);

    CHECK(g_last_code == COAP_RESPONSE_CODE_CHANGED);
    CHECK(g_cad_calls == 1);
    StatusResponse sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(decode_status(&sr));
    CHECK(sr.ok == true);
    CHECK(g_set_hyst_calls == 0 && g_set_ramp_calls == 0 && g_set_failsafe_calls == 0);
    printf("  [PASS] %s\n", __func__);
    g_pass++;
}

/* ================================================================
 * main
 * ================================================================ */

int main(void)
{
    test_config_get_export_failure_returns_500();
    test_config_get_success_2_05_passes_buffer_to_libcoap();
    test_config_get_success_empty_registries_empty_lists();
    test_config_get_success_partial_registries_counts_match();
    test_config_get_success_fully_loaded_passes_full_buffer_for_block2();
    test_config_get_large_response_failure_no_double_free();
    test_coap_free_config_data_frees_buffer();
    test_coap_free_config_data_null_is_noop();

    test_config_post_decode_failure_empty_body_returns_400();                 /* P1 */
    test_config_post_decode_failure_malformed_body_returns_400();             /* P2 */
    test_config_post_validation_failure_returns_400_with_error_msg();         /* P3 */
    test_config_post_validation_failure_no_err_msg_returns_400_empty_error(); /* P4 */
    test_config_post_persist_failure_returns_500();                           /* P5 */
    test_config_post_success_schedules_reboot_timer_created();                /* P6 */
    test_config_post_success_reuses_existing_timer();                         /* P7 */
    test_config_post_success_timer_start_failure_no_pending();                /* P8 */
    test_config_post_success_reboot_already_pending_skips_timer();            /* P9 */
    test_config_post_success_empty_config_clears_and_reboots();               /* P10 */

    /* handle_fan_post (gpio phases 2-5) */
    test_fan_post_decode_failure_returns_400();                     /* HFP-P1 */
    test_fan_post_add_failure_returns_400_with_error_msg();         /* HFP-P2 */
    test_fan_post_add_failure_no_err_msg_returns_400_empty_error(); /* HFP-P3 */
    test_fan_post_success_returns_201_fan_info();                   /* HFP-P4 */

    /* handle_fan_put (gpio phases 2-5) */
    test_fan_put_short_path_returns_404();                                 /* HFU-P1 */
    test_fan_put_decode_failure_returns_400();                             /* HFU-P2 */
    test_fan_put_fan_not_found_returns_404();                              /* HFU-P3 */
    test_fan_put_no_gpio_fields_skips_set_gpio();                          /* HFU-P4 */
    test_fan_put_gpio_update_success_returns_204();                        /* HFU-P5 */
    test_fan_put_gpio_update_failure_returns_400_with_error_msg_no_save(); /* HFU-P6 */
    test_fan_put_gpio_update_failure_no_err_msg_returns_400_empty_error(); /* HFU-P7 */

    /* handle_source_post (gpio phases 2-5) */
    test_source_post_temp_decode_failure_returns_400();                    /* HSP-P1 */
    test_source_post_temp_source_not_found_returns_404();                  /* HSP-P2 */
    test_source_post_temp_update_failure_returns_400();                    /* HSP-P3 */
    test_source_post_temp_success_returns_204_ok();                        /* HSP-P4 */
    test_source_post_create_decode_failure_returns_400();                  /* HSP-P5 */
    test_source_post_ds18b20_add_failure_returns_400_no_status_body();     /* HSP-P6 */
    test_source_post_ds18b20_add_success_returns_201();                    /* HSP-P7 */
    test_source_post_ntc_add_failure_returns_400_with_error_msg();         /* HSP-P8 */
    test_source_post_ntc_add_failure_no_err_msg_returns_400_empty_error(); /* HSP-P9 */
    test_source_post_manual_add_success_returns_201();                     /* HSP-P10 */

    /* handle_control_get (ctrl phase-5) */
    test_control_get_null_control_returns_503();       /* HCG-P1 */
    test_control_get_tunables_failure_returns_500();   /* HCG-P2 */
    test_control_get_success_returns_205_all_fields(); /* HCG-P3 */

    /* handle_control_put (ctrl phase-5) */
    test_control_put_null_control_returns_503();                 /* HCP-P1 */
    test_control_put_decode_failure_returns_400_decode_failed(); /* HCP-P2 */
    test_control_put_merge_base_failure_returns_500();           /* HCP-P3 */
    test_control_put_hysteresis_oob_returns_400();               /* HCP-P4 */
    test_control_put_ramp_up_oob_returns_400();                  /* HCP-P5 */
    test_control_put_ramp_down_oob_returns_400();                /* HCP-P6 */
    test_control_put_failsafe_policy_oob_returns_400();          /* HCP-P7 */
    test_control_put_safe_duty_oob_returns_400();                /* HCP-P8 */
    test_control_put_all_fields_success_returns_204();           /* HCP-P9 */
    test_control_put_partial_hysteresis_only();                  /* HCP-P10 */
    test_control_put_partial_ramp_up_only();                     /* HCP-P11 */
    test_control_put_partial_ramp_down_only();                   /* HCP-P12 */
    test_control_put_partial_failsafe_policy_only();             /* HCP-P13 */
    test_control_put_partial_safe_duty_only();                   /* HCP-P14 */
    test_control_put_no_fields_noop_returns_204();               /* HCP-P15 */

    printf("\nRESULT: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
