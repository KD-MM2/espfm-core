idf_component_register(
    SRCS main.c
    INCLUDE_DIRS .
    REQUIRES nvs_flash f_core f_wifi f_fan
)
