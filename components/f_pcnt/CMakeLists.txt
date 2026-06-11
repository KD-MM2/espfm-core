idf_component_register(
    SRCS f_pcnt.c
    INCLUDE_DIRS include
    REQUIRES driver f_core
    PRIV_REQUIRES esp_driver_pcnt
)
