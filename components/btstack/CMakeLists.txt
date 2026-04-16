set(BTSTACK_ROOT ${CMAKE_CURRENT_LIST_DIR})

if (CMAKE_HOST_WIN32)
    string(REPLACE "\\" "/" IDF_PATH $ENV{IDF_PATH})
else()
    set(IDF_PATH $ENV{IDF_PATH})
endif()

set(include_dirs
    ${BTSTACK_ROOT}/3rd-party/bluedroid/decoder/include
    ${BTSTACK_ROOT}/3rd-party/bluedroid/encoder/include
    ${BTSTACK_ROOT}/3rd-party/hxcmod-player
    ${BTSTACK_ROOT}/3rd-party/hxcmod-player/mods
    ${BTSTACK_ROOT}/3rd-party/lc3-google/include
    ${BTSTACK_ROOT}/3rd-party/lwip/dhcp-server
    ${BTSTACK_ROOT}/3rd-party/md5
    ${BTSTACK_ROOT}/3rd-party/micro-ecc
    ${BTSTACK_ROOT}/3rd-party/qr-code-generator
    ${BTSTACK_ROOT}/3rd-party/yxml
    ${BTSTACK_ROOT}/src/ble/gatt-service
    ${BTSTACK_ROOT}/src/ble
    ${BTSTACK_ROOT}/src/le-audio
    ${BTSTACK_ROOT}/src/le-audio/gatt-service
    ${BTSTACK_ROOT}/src/classic
    ${BTSTACK_ROOT}/src
    ${BTSTACK_ROOT}/platform/embedded
    ${BTSTACK_ROOT}/platform/freertos
    ${BTSTACK_ROOT}/platform/lwip
    ${IDF_PATH}/components/lwip/lwip/src/include
    include)

# NOTE: platform/embedded headers are needed but its .c files must NOT be
# compiled for ESP32/FreeRTOS (btstack_stdio_esp32.c replaces them)

set(src_dirs
    ${BTSTACK_ROOT}/3rd-party/bluedroid/decoder/srce
    ${BTSTACK_ROOT}/3rd-party/bluedroid/encoder/srce
    ${BTSTACK_ROOT}/3rd-party/hxcmod-player
    ${BTSTACK_ROOT}/3rd-party/hxcmod-player/mods
    ${BTSTACK_ROOT}/3rd-party/lc3-google/src
    ${BTSTACK_ROOT}/3rd-party/lwip/dhcp-server
    ${BTSTACK_ROOT}/3rd-party/md5
    ${BTSTACK_ROOT}/3rd-party/micro-ecc
    ${BTSTACK_ROOT}/3rd-party/qr-code-generator
    ${BTSTACK_ROOT}/3rd-party/yxml
    ${BTSTACK_ROOT}/src/ble/gatt-service
    ${BTSTACK_ROOT}/src/ble
    ${BTSTACK_ROOT}/src/le-audio
    ${BTSTACK_ROOT}/src/le-audio/gatt-service
    ${BTSTACK_ROOT}/src/mesh
    ${BTSTACK_ROOT}/src/classic
    ${BTSTACK_ROOT}/src
    ${BTSTACK_ROOT}/platform/freertos
    ${BTSTACK_ROOT}/platform/lwip
    ${IDF_PATH}/components/lwip/lwip/src/apps/http
    .)

idf_component_register(
    SRC_DIRS     ${src_dirs}
    INCLUDE_DIRS ${include_dirs}
    REQUIRES     bt nvs_flash esp_timer vfs driver wm8731 i2cdev
    PRIV_REQUIRES btstack_config
    EXCLUDE_SRCS ${BTSTACK_ROOT}/btstack_audio_esp32_v4.c
                 ${BTSTACK_ROOT}/es8388.c
)
