idf_component_register(
    SRCS         "host_link.c" "host_link_spp.c" "host_link_serial.c"
    INCLUDE_DIRS "."
    REQUIRES     btstack btstack_config cat_bridge serial_port driver vfs log freertos
)
