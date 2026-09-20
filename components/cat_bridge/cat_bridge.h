/*
 * cat_bridge.h
 *
 * Enlace físico con el transceptor: puerto serie CAT (sobre serial_port) más
 * las líneas PTT y COS. Pines, velocidad y framing se configuran en
 * menuconfig -> Component config -> CAT bridge (radio link).
 *
 * El callback de recepción se ejecuta en la tarea del puerto serie, NO en el
 * run loop de BTstack.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "serial_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Datos recibidos de la radio, ya agrupados según el framing configurado. */
typedef void (*cat_rx_cb_t)(const uint8_t *data, uint16_t len);

/** @brief Abre la UART CAT y configura PTT (salida) y COS (entrada). */
esp_err_t cat_bridge_init(cat_rx_cb_t rx_cb);

/** @brief Envía bytes tal cual a la radio. Seguro desde cualquier tarea. */
esp_err_t cat_bridge_send(const uint8_t *data, uint16_t len);

/** @brief Cambia la velocidad del enlace CAT en caliente. */
esp_err_t cat_bridge_set_baud(int baud_rate);
int       cat_bridge_get_baud(void);

void cat_bridge_set_ptt(bool active);
bool cat_bridge_get_ptt(void);
bool cat_bridge_get_cos(void);

void cat_bridge_get_stats(serial_port_stats_t *out);

#ifdef __cplusplus
}
#endif
