#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct f_mdns *f_mdns_handle_t;

/**
 * @brief Initialize mDNS service discovery.
 *
 * Registers WiFi event handlers. Reads hostname from NVS key "mdns_hostname"
 * or generates default "espfm-xxYY" from MAC address. mDNS starts/stops
 * automatically on WiFi connect/disconnect events.
 *
 * @param[out] handle  Opaque handle for this instance.
 * @return ESP_OK on success.
 */
esp_err_t f_mdns_init(f_mdns_handle_t *handle);

/**
 * @brief Tear down mDNS — unregister event handlers, free resources.
 */
esp_err_t f_mdns_deinit(f_mdns_handle_t handle);

/**
 * @brief Set a custom hostname (stored in NVS, survives reboot).
 *
 * Validates: 1-63 chars, lowercase alphanumeric + hyphens (RFC 1035).
 * If mDNS is currently running, updates the hostname live via mdns_hostname_set().
 * Also updates the in-memory instance so the new hostname persists across
 * WiFi reconnects within the same boot.
 *
 * @param hostname  New hostname (without .local suffix).
 * @return ESP_ERR_INVALID_ARG if validation fails.
 */
esp_err_t f_mdns_set_hostname(const char *hostname);

#ifdef __cplusplus
}
#endif
