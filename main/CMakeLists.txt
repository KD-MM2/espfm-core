idf_component_register(
    SRCS main.c
    INCLUDE_DIRS .
    REQUIRES nvs_flash esp_event f_core f_gpio f_wifi f_ledc f_pcnt f_adc f_ds18b20 f_fan f_source f_curve f_control f_schedule f_coap f_config f_provision f_mdns
)
