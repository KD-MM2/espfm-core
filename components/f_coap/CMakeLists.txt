idf_component_register(
    SRCS f_coap.c
    INCLUDE_DIRS include
    REQUIRES f_core f_fan f_source f_curve f_schedule f_config f_constraints f_wifi microcoap nanopb
    PRIV_REQUIRES lwip esp_wifi esp_netif
)
