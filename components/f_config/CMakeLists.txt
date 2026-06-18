idf_component_register(
    SRCS f_config.c
    INCLUDE_DIRS include
    REQUIRES f_core f_fan f_source f_curve f_schedule f_constraints
    PRIV_REQUIRES espressif__cjson joltwallet__littlefs
)
