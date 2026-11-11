/*
 * host_link.h
 *
 * Enlace con el "host" (PC o móvil) independiente del transporte.
 *
 * Varios transportes (Bluetooth SPP, UART por cable, ...) se registran como
 * enlaces equivalentes. Todos alimentan el mismo despachador de comandos y
 * reciben los mismos datos de la radio:
 *
 *   - La respuesta a un comando vuelve por el enlace que lo envió.
 *   - Lo que la radio emite por su cuenta se difunde a todos los enlaces
 *     conectados.
 *
 * Protocolo (líneas terminadas en CR y/o LF):
 *   PING            -> PONG
 *   PTT=1 / PTT=0   -> OK            activa / suelta el PTT
 *   PTT?            -> PTT=0|1
 *   AUTOPTT=1 / =0  -> OK            el PTT sigue (o no) a la llamada HFP
 *   AUTOPTT?        -> AUTOPTT=0|1
 *   COS?            -> COS=0|1       estado del squelch / portadora
 *   BAUD=<n>        -> OK | ERR      cambia la velocidad del enlace CAT
 *   INFO            -> INFO ...      versión, enlaces y estadísticas
 *   cualquier otra cosa se reenvía tal cual al transceptor (CAT), incluido su
 *   delimitador. Si el enlace CAT usa un byte terminador (p. ej. ';'), ese
 *   byte también cierra trama aquí. Datos binarios sin delimitador se
 *   reenvían tras un breve silencio.
 *
 * Contexto de ejecución: TODO host_link corre en el run loop de BTstack. Los
 * transportes que reciben en otra tarea usan host_link_rx_from_task(), que
 * cruza al run loop de forma segura.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "btstack_run_loop.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOST_LINK_MAX_LINE 256

typedef struct host_link host_link_t;

typedef struct {
    const char *name;
    /** Envía datos por el transporte. Se llama en el run loop de BTstack. */
    void (*send)(host_link_t *link, const uint8_t *data, size_t len);
    /** true si hay alguien al otro lado (para SPP: canal abierto). */
    bool (*is_connected)(host_link_t *link);
} host_link_ops_t;

/** Un enlace. Los transportes lo alojan (estático) y lo pasan a host_link_register. */
struct host_link {
    const host_link_ops_t *ops;
    void                  *impl;

    /* --- privado: gestionado por host_link.c --- */
    uint8_t                rx_buf[HOST_LINK_MAX_LINE + 1];
    size_t                 rx_len;
    btstack_timer_source_t idle_timer;
    uint32_t               rx_frames;
    uint32_t               tx_bytes;
    host_link_t           *next;
};

/** @brief Inicializa el despachador y abre el enlace con la radio (cat_bridge). */
void host_link_init(void);

/** @brief Registra un transporte. */
void host_link_register(host_link_t *link, const host_link_ops_t *ops, void *impl);

/** @brief Datos recibidos del host. SOLO desde el run loop de BTstack. */
void host_link_rx(host_link_t *link, const uint8_t *data, size_t len);

/** @brief Datos recibidos del host desde cualquier tarea FreeRTOS. */
void host_link_rx_from_task(host_link_t *link, const uint8_t *data, size_t len);

/** @brief Envía a un enlace concreto (run loop de BTstack). */
void host_link_send(host_link_t *link, const uint8_t *data, size_t len);
void host_link_send_str(host_link_t *link, const char *str);

/** @brief Envía a todos los enlaces conectados (run loop de BTstack). */
void host_link_broadcast(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
