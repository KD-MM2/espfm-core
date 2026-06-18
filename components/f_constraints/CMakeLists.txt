idf_component_register(
    SRCS f_constraints.c
    INCLUDE_DIRS include
    REQUIRES f_core f_fan f_source f_curve f_schedule
)
