idf_component_register(
    SRCS main.c
    INCLUDE_DIRS . ../components/f_core/include ../components/f_wifi/include
    REQUIRES nvs_flash f_core f_wifi
)
