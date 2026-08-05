#pragma once
#include "esp_err.h"
#include "f_core.h"
#include "f_fan.h"
#include "f_source.h"
#include "f_curve.h"
#include "f_schedule.h"
#include "f_config.h"
#include "f_mdns.h"
#include "f_ds18b20.h"
#include "f_gpio.h"
#include "f_control.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct f_coap *f_coap_handle_t;

esp_err_t f_coap_init(f_coap_handle_t *handle, f_fan_handle_t fan, f_source_handle_t source,
                      f_curve_handle_t curve, f_schedule_handle_t schedule,
                      f_config_handle_t config, f_mdns_handle_t mdns,
                      f_ds18b20_handle_t *ds18b20_ref, f_gpio_handle_t gpio,
                      f_control_handle_t control);
esp_err_t f_coap_start(f_coap_handle_t handle);
esp_err_t f_coap_stop(f_coap_handle_t handle);
esp_err_t f_coap_deinit(f_coap_handle_t handle);

#ifdef __cplusplus
}
#endif
