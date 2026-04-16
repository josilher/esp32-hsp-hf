idf_component_register(
    SRCS "wm8731.c"
    INCLUDE_DIRS .
    REQUIRES i2cdev driver log freertos
)
