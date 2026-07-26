#pragma once
#include "esp_err.h"
#include "f_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct f_provision *f_provision_handle_t;

esp_err_t f_provision_init(f_provision_handle_t *handle, f_wifi_handle_t wifi);
esp_err_t f_provision_start(f_provision_handle_t handle);
esp_err_t f_provision_stop(f_provision_handle_t handle);

#ifdef __cplusplus
}
#endif
