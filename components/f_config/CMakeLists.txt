idf_component_register(
    SRCS f_config.c
    INCLUDE_DIRS include
    REQUIRES f_core f_fan f_source f_curve f_schedule f_constraints f_gpio f_schema nanopb
    PRIV_REQUIRES joltwallet__littlefs
)
