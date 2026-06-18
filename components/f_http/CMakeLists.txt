idf_component_register(
    SRCS f_http.c
    INCLUDE_DIRS include
    REQUIRES f_core f_fan f_source f_curve f_schedule f_config f_constraints esp_http_server esp_wifi
    PRIV_REQUIRES espressif__cjson
)
