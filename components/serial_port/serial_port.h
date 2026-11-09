/*
 * serial_port.h
 *
 * Puerto serie genérico sobre el driver UART de ESP-IDF.
 *
 *  - Una instancia por UART, con su propia tarea de recepción.
 *  - Entrega tramas ya delimitadas según el modo de framing elegido.
 *  - Escritura protegida con mutex, usable desde cualquier tarea.
 *  - Estadísticas de bytes, tramas y desbordamientos para diagnóstico.
 *
 * No sabe nada de radio ni de Bluetooth: el mismo componente sirve para el
 * enlace con el transceptor (CAT) y para el enlace por cable con el PC.
 *
 * El callback de trama se ejecuta en la tarea de recepción del puerto. Si el
 * consumidor vive en otro contexto (por ejemplo el run loop de BTstack) es
 * responsabilidad suya cruzar los datos de forma segura.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Cómo se delimitan las tramas recibidas. */
typedef enum {
    /** Línea de texto terminada en CR o LF. El delimitador no se incluye y la
     *  trama se entrega terminada en NUL (usable como cadena C). */
    SERIAL_FRAMING_LINE = 0,
    /** Trama terminada por un byte concreto (p. ej. ';' Kenwood/Elecraft,
     *  0xFD Icom CI-V). El terminador SÍ se incluye en la trama. */
    SERIAL_FRAMING_TERMINATOR,
    /** Sin delimitador: se entrega lo recibido cuando pasa idle_timeout_ms sin
     *  datos o cuando se llena max_frame_len. Para protocolos binarios. */
    SERIAL_FRAMING_RAW,
} serial_framing_t;

typedef struct {
    uart_port_t        uart_num;
    int                tx_pin;          /**< UART_PIN_NO_CHANGE para los pines por defecto */
    int                rx_pin;
    int                baud_rate;
    uart_word_length_t data_bits;
    uart_parity_t      parity;
    uart_stop_bits_t   stop_bits;
    bool               invert_tx;       /**< Invierte la polaridad de TXD en el periférico */
    bool               invert_rx;       /**< Invierte la polaridad de RXD */
    serial_framing_t   framing;
    uint8_t            terminator;      /**< Solo SERIAL_FRAMING_TERMINATOR */
    uint32_t           idle_timeout_ms; /**< Solo SERIAL_FRAMING_RAW. 0 = solo por tamaño */
    size_t             max_frame_len;   /**< Tamaño máximo de trama entregada */
    size_t             rx_buf_size;     /**< Buffer del driver UART (mínimo 256) */
    size_t             tx_buf_size;     /**< 0 = escritura bloqueante hasta vaciar */
    UBaseType_t        task_priority;
    uint32_t           task_stack;
    BaseType_t         task_core;        /**< tskNO_AFFINITY (por defecto) o 0/1.
                                           *   Se ignora, con aviso por log, si el
                                           *   firmware esta compilado en unicore
                                           *   (CONFIG_FREERTOS_UNICORE). */
    const char        *name;            /**< Para logs y nombre de la tarea */
} serial_port_config_t;

#define SERIAL_PORT_CONFIG_DEFAULT() {          \
    .uart_num        = UART_NUM_1,              \
    .tx_pin          = UART_PIN_NO_CHANGE,      \
    .rx_pin          = UART_PIN_NO_CHANGE,      \
    .baud_rate       = 115200,                  \
    .data_bits       = UART_DATA_8_BITS,        \
    .parity          = UART_PARITY_DISABLE,     \
    .stop_bits       = UART_STOP_BITS_1,        \
    .invert_tx       = false,                   \
    .invert_rx       = false,                   \
    .framing         = SERIAL_FRAMING_LINE,     \
    .terminator      = '\n',                    \
    .idle_timeout_ms = 20,                      \
    .max_frame_len   = 256,                     \
    .rx_buf_size     = 1024,                    \
    .tx_buf_size     = 1024,                    \
    .task_priority   = 5,                       \
    .task_stack      = 3072,                    \
    .task_core       = tskNO_AFFINITY,          \
    .name            = "serial",                \
}

typedef struct {
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_frames;
    uint32_t overruns;        /**< Desbordamientos de FIFO o buffer: se perdieron datos */
    uint32_t line_errors;     /**< Errores de trama o paridad detectados por la UART */
    uint32_t dropped_frames;  /**< Tramas descartadas por superar max_frame_len */
} serial_port_stats_t;

typedef struct serial_port serial_port_t;

/**
 * @brief Callback de trama completa. Se ejecuta en la tarea de recepción del puerto.
 * @param frame  Datos de la trama. Siempre hay un NUL en frame[len] (no contado en len).
 */
typedef void (*serial_frame_cb_t)(serial_port_t *port, const uint8_t *frame, size_t len, void *ctx);

/**
 * @brief Abre la UART, instala el driver y arranca la tarea de recepción.
 * @return ESP_ERR_INVALID_STATE si la UART ya tiene un driver instalado.
 */
esp_err_t serial_port_open(const serial_port_config_t *cfg, serial_frame_cb_t cb, void *ctx,
                           serial_port_t **out);

/** @brief Detiene la tarea, desinstala el driver y libera recursos. */
esp_err_t serial_port_close(serial_port_t *port);

/** @brief Escribe datos. Seguro desde cualquier tarea. */
esp_err_t serial_port_write(serial_port_t *port, const uint8_t *data, size_t len);

/** @brief Escribe una cadena C (sin el NUL). */
esp_err_t serial_port_write_str(serial_port_t *port, const char *str);

/** @brief Cambia la velocidad en caliente. */
esp_err_t serial_port_set_baud(serial_port_t *port, int baud_rate);
int       serial_port_get_baud(const serial_port_t *port);

void        serial_port_get_stats(const serial_port_t *port, serial_port_stats_t *out);
const char *serial_port_get_name(const serial_port_t *port);
uart_port_t serial_port_get_uart(const serial_port_t *port);

/** @brief Nucleo donde corre la tarea de recepcion (0 o 1), o -1 si no hay tarea. */
BaseType_t serial_port_get_core(const serial_port_t *port);

#ifdef __cplusplus
}
#endif
