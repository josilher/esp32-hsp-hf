#include "cat_bridge.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG         "CAT"
#define RX_BUF_SIZE 1024

static cat_rx_cb_t s_rx_cb = NULL;

static void cat_rx_task(void *arg)
{
    uint8_t buf[256];
    while (1) {
        int len = uart_read_bytes(CAT_UART_NUM, buf, sizeof(buf),
                                  pdMS_TO_TICKS(20));
        if (len > 0 && s_rx_cb) {
            s_rx_cb(buf, (uint16_t)len);
        }
    }
}

esp_err_t cat_bridge_init(cat_rx_cb_t rx_cb)
{
    s_rx_cb = rx_cb;

    // Configurar UART2
    uart_config_t cfg = {
        .baud_rate  = CAT_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_param_config(CAT_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(CAT_UART_NUM, CAT_TX_PIN, CAT_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(CAT_UART_NUM, RX_BUF_SIZE, 0,
                                        0, NULL, 0));

    // Configurar PTT como salida (activo en bajo — optoacoplador)
    gpio_config_t ptt = {
        .pin_bit_mask = (1ULL << PTT_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&ptt));
    gpio_set_level(PTT_PIN, 1); // reposo = no transmite

    // Configurar COS como entrada
    gpio_config_t cos = {
        .pin_bit_mask = (1ULL << COS_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,  // GPIO39 es input-only, sin PU interno
    };
    ESP_ERROR_CHECK(gpio_config(&cos));

    // Tarea de recepción UART → SPP
    xTaskCreate(cat_rx_task, "cat_rx", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "CAT bridge listo (TX=%d RX=%d PTT=%d COS=%d)",
             CAT_TX_PIN, CAT_RX_PIN, PTT_PIN, COS_PIN);
    return ESP_OK;
}

void cat_bridge_send(const uint8_t *data, uint16_t len)
{
    uart_write_bytes(CAT_UART_NUM, (const char *)data, len);
}

void cat_bridge_set_ptt(bool active)
{
    // Optoacoplador: activo en bajo
    gpio_set_level(PTT_PIN, active ? 0 : 1);
}

bool cat_bridge_get_cos(void)
{
    return gpio_get_level(COS_PIN) == 0;
}
