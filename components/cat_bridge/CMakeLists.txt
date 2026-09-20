idf_component_register(
    SRCS         "cat_bridge.c"
    INCLUDE_DIRS "."
    REQUIRES     serial_port driver log
)
