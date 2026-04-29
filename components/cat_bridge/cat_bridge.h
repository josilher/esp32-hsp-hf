#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAT_UART_NUM    UART_NUM_2
#define CAT_TX_PIN      32
#define CAT_RX_PIN      34
#define CAT_BAUD_RATE   9600
#define PTT_PIN         33
#define COS_PIN         39

typedef void (*cat_rx_cb_t)(const uint8_t *data, uint16_t len);

esp_err_t cat_bridge_init(cat_rx_cb_t rx_cb);
void      cat_bridge_send(const uint8_t *data, uint16_t len);
void      cat_bridge_set_ptt(bool active);
bool      cat_bridge_get_cos(void);

#ifdef __cplusplus
}
#endif
