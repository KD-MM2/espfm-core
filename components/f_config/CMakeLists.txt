idf_component_register(
    SRCS f_config.c
    INCLUDE_DIRS include
    REQUIRES f_core f_fan f_source f_curve spiffs
    PRIV_REQUIRES espressif__cjson
)
