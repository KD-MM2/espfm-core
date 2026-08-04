#pragma once
#include <stdint.h>
#include "esp_err.h"

/* Minimal ESP-IDF esp_netif.h stub for host-based unit tests of f_coap_routes.c. */

typedef struct esp_netif_obj esp_netif_t;

typedef struct {
    struct {
        uint32_t addr;
    } ip;
} esp_netif_ip_info_t;

esp_netif_t *esp_netif_get_handle_from_ifkey(const char *if_key);
esp_err_t esp_netif_get_ip_info(esp_netif_t *esp_netif, esp_netif_ip_info_t *ip_info);

#define IPSTR "%d.%d.%d.%d"
#define IP2STR(ipaddr)                                                            \
    ((int)((const uint8_t *)(ipaddr))[0]), ((int)((const uint8_t *)(ipaddr))[1]), \
        ((int)((const uint8_t *)(ipaddr))[2]), ((int)((const uint8_t *)(ipaddr))[3])
