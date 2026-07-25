idf_component_register(
    SRCS f_fan.c
    INCLUDE_DIRS include
    REQUIRES f_core f_ledc f_pcnt f_constraints
)
