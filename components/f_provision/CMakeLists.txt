idf_component_register(
    SRCS f_provision.c f_provision_http.c f_provision_dns.c
    INCLUDE_DIRS include
    REQUIRES f_core f_wifi
    PRIV_REQUIRES esp_wifi esp_http_server esp_event nvs_flash lwip
)
