idf_component_register(
    SRCS f_ds18b20.c
    INCLUDE_DIRS include
    REQUIRES driver f_core f_gpio
    PRIV_REQUIRES espressif__onewire_bus espressif__ds18b20
)
