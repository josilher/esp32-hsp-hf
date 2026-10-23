/*
 * host_link_serial.h
 *
 * Transporte por cable (UART) para host_link. Pensado para la UART0 del
 * USB de la placa, de modo que un PC pueda usar el mismo protocolo que por
 * Bluetooth sin emparejar nada.
 *
 * ATENCIÓN: en esta placa las líneas RTS y DTR del CH340G van al circuito de
 * autoreset (EN e IO0). Un programa que use RTS o DTR para el PTT reiniciará
 * el ESP32. El PTT por cable debe ir por comando (PTT=1) o por CAT.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "driver/uart.h"
#include "host_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Abre la UART indicada como enlace con el host y la registra.
 * @param uart_num      Normalmente UART_NUM_0 (USB).
 * @param baud_rate     Velocidad del enlace con el PC.
 * @param mute_console  Si es true, redirige stdout/stderr a /dev/null y
 *                      silencia ESP_LOG, para que los logs no se mezclen con
 *                      el protocolo. Imprescindible si uart_num es la UART de
 *                      consola.
 */
esp_err_t host_link_serial_init(uart_port_t uart_num, int baud_rate, bool mute_console);

/** @brief Nucleo donde corre la tarea de recepcion (0 o 1), o -1 si el enlace no esta activo. */
int host_link_serial_get_core(void);

#ifdef __cplusplus
}
#endif
