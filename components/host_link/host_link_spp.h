/*
 * host_link_spp.h
 *
 * Transporte Bluetooth SPP (puerto serie virtual) para host_link.
 * Requiere que la aplicación haya llamado antes a l2cap_init(), rfcomm_init()
 * y sdp_init(). Admite una conexión a la vez.
 */
#pragma once

#include <stdint.h>
#include "host_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Registra el servicio RFCOMM, publica el registro SDP y registra el
 *        enlace en host_link.
 * @param rfcomm_channel  Canal RFCOMM del servidor (distinto del de HFP).
 * @param service_name    Nombre del servicio que verá el PC al emparejar.
 */
void host_link_spp_init(uint8_t rfcomm_channel, const char *service_name);

#ifdef __cplusplus
}
#endif
