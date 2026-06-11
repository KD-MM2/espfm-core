idf_component_register(
    SRCS main.c
    INCLUDE_DIRS . ../components/f_core/include
    REQUIRES nvs_flash f_core
)
