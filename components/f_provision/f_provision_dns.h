#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the captive portal DNS redirect server.
 *
 * Creates a UDP PCB bound to port 53 that responds to all DNS A-record
 * queries with 192.168.4.1, forcing clients to the provisioning portal.
 *
 * @return ESP_OK on success
 */
esp_err_t f_provision_dns_start(void);

/**
 * @brief Stop the DNS redirect server and release resources.
 */
void f_provision_dns_stop(void);

#ifdef __cplusplus
}
#endif
