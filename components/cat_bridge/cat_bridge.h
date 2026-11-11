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

/** @brief Nucleo donde corre la tarea de recepcion CAT (0 o 1), o -1 si no hay tarea. */
int cat_bridge_get_core(void);

void cat_bridge_set_ptt(bool active);
bool cat_bridge_get_ptt(void);
bool cat_bridge_get_cos(void);

/**
 * @brief Notifica que el canal de audio HFP se ha abierto o cerrado.
 *
 * Si el PTT esta configurado para seguir al audio, lo activa o lo suelta.
 * Lo llama la aplicacion desde los eventos de conexion de audio de HFP.
 */
void cat_bridge_audio_activity(bool active);

/**
 * @brief Hace que el PTT siga la actividad de la llamada HFP (o deja de hacerlo).
 *
 * Valor inicial: CONFIG_CAT_PTT_FOLLOWS_AUDIO. Los comandos PTT=1/0 explicitos
 * siguen funcionando siempre; esto solo añade la activacion automatica.
 */
void cat_bridge_set_ptt_follows_audio(bool on);
bool cat_bridge_get_ptt_follows_audio(void);

void cat_bridge_get_stats(serial_port_stats_t *out);

#ifdef __cplusplus
}
#endif
