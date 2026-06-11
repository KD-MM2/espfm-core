#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct f_wifi *f_wifi_handle_t;

esp_err_t f_wifi_init(f_wifi_handle_t *handle);
esp_err_t f_wifi_wait_connected(f_wifi_handle_t handle, TickType_t timeout);
bool f_wifi_is_connected(f_wifi_handle_t handle);
esp_err_t f_wifi_get_ip_str(f_wifi_handle_t handle, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
