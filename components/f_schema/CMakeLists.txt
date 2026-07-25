idf_component_register(
    SRCS espfm.pb.c espfm_conv.c
    INCLUDE_DIRS . include
    REQUIRES nanopb f_core f_fan f_source f_curve f_schedule
)
