idf_component_register(
    SRCS main.c
    INCLUDE_DIRS .
    REQUIRES nvs_flash esp_event f_core f_wifi f_http f_config
)
