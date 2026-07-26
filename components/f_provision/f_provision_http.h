#pragma once
#include "esp_http_server.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register all HTTP URI handlers for the provisioning captive portal.
 *
 * Called from _start_http_server() after httpd_start() succeeds.
 *
 * @param server Running HTTP server handle
 * @return ESP_OK on success
 */
esp_err_t f_provision_register_http_handlers(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
