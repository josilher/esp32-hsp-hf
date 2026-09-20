idf_component_register(
    SRCS         "serial_port.c"
    INCLUDE_DIRS "."
    REQUIRES     driver freertos log esp_common
)
