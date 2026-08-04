idf_component_register(
    SRCS f_source.c
    INCLUDE_DIRS include
    REQUIRES f_core f_adc f_ds18b20 esp_timer f_gpio
)
