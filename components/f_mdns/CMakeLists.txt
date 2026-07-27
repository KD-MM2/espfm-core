idf_component_register(
    SRCS f_mdns.c
    INCLUDE_DIRS include
    REQUIRES mdns esp_event esp_netif nvs_flash f_core
    PRIV_REQUIRES esp_wifi
)
