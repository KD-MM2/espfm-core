idf_component_register(
    SRCS f_adc.c
    INCLUDE_DIRS include
    REQUIRES driver f_core
    PRIV_REQUIRES esp_adc
)
