idf_component_register(
    SRCS f_coap.c f_coap_routes.c
    INCLUDE_DIRS include .
    REQUIRES f_core f_fan f_source f_curve f_schedule f_config f_mdns f_constraints f_wifi f_schema f_ds18b20 nanopb
    PRIV_REQUIRES lwip esp_wifi esp_netif coap
)
