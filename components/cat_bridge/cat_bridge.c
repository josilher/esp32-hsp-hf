#include "cat_bridge.h"

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "cat_bridge";

static serial_port_t *s_port   = NULL;
static cat_rx_cb_t    s_rx_cb  = NULL;
static bool           s_ptt_on = false;

static void frame_cb(serial_port_t *port, const uint8_t *frame, size_t len, void *ctx)
{
    (void)port; (void)ctx;
    if (s_rx_cb && len > 0) s_rx_cb(frame, (uint16_t)len);
}

static int ptt_level(bool active)
{
#if CONFIG_CAT_PTT_ACTIVE_LOW
    return active ? 0 : 1;
#else
    return active ? 1 : 0;
#endif
}

esp_err_t cat_bridge_init(cat_rx_cb_t rx_cb)
{
    if (s_port) return ESP_ERR_INVALID_STATE;
    s_rx_cb = rx_cb;

    /* --- PTT: salida, en reposo antes de tocar nada más --- */
    gpio_config_t ptt = {
        .pin_bit_mask = 1ULL << CONFIG_CAT_PTT_PIN,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&ptt));
    gpio_set_level(CONFIG_CAT_PTT_PIN, ptt_level(false));
    s_ptt_on = false;

    /* --- COS: entrada. GPIO34..39 no tienen pull-up interno --- */
    gpio_config_t cos = {
        .pin_bit_mask = 1ULL << CONFIG_CAT_COS_PIN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cos));

    /* --- UART CAT --- */
    serial_port_config_t cfg = SERIAL_PORT_CONFIG_DEFAULT();
    cfg.uart_num  = CONFIG_CAT_UART_NUM;
    cfg.tx_pin    = CONFIG_CAT_TX_PIN;
    cfg.rx_pin    = CONFIG_CAT_RX_PIN;
    cfg.baud_rate = CONFIG_CAT_BAUD_RATE;
#if CONFIG_CAT_TX_INVERT
    cfg.invert_tx = true;
#endif
#if CONFIG_CAT_RX_INVERT
    cfg.invert_rx = true;
#endif
    cfg.name      = "cat";
#if CONFIG_SERIAL_PORT_CAT_ON_CORE1 && !CONFIG_FREERTOS_UNICORE
    cfg.task_core = 1;
#endif
#if CONFIG_CAT_FRAMING_LINE
    cfg.framing = SERIAL_FRAMING_LINE;
#elif CONFIG_CAT_FRAMING_TERMINATOR
    cfg.framing    = SERIAL_FRAMING_TERMINATOR;
    cfg.terminator = (uint8_t)CONFIG_CAT_TERMINATOR;
#else
    cfg.framing         = SERIAL_FRAMING_RAW;
    cfg.idle_timeout_ms = CONFIG_CAT_IDLE_TIMEOUT_MS;
#endif

    esp_err_t err = serial_port_open(&cfg, frame_cb, NULL, &s_port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo abrir la UART CAT: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "listo: UART%d %d baudios TX=%d%s RX=%d%s, PTT=%d COS=%d",
             CONFIG_CAT_UART_NUM, CONFIG_CAT_BAUD_RATE,
             CONFIG_CAT_TX_PIN, cfg.invert_tx ? " invertido" : "",
             CONFIG_CAT_RX_PIN, cfg.invert_rx ? " invertido" : "",
             CONFIG_CAT_PTT_PIN, CONFIG_CAT_COS_PIN);
    return ESP_OK;
}

esp_err_t cat_bridge_send(const uint8_t *data, uint16_t len)
{
    if (!s_port) return ESP_ERR_INVALID_STATE;
    return serial_port_write(s_port, data, len);
}

esp_err_t cat_bridge_set_baud(int baud_rate)
{
    if (!s_port) return ESP_ERR_INVALID_STATE;
    return serial_port_set_baud(s_port, baud_rate);
}

int cat_bridge_get_baud(void)
{
    return serial_port_get_baud(s_port);
}

int cat_bridge_get_core(void)
{
    return (int)serial_port_get_core(s_port);
}

void cat_bridge_set_ptt(bool active)
{
    s_ptt_on = active;
    gpio_set_level(CONFIG_CAT_PTT_PIN, ptt_level(active));
}

bool cat_bridge_get_ptt(void)
{
    return s_ptt_on;
}

bool cat_bridge_get_cos(void)
{
    int level = gpio_get_level(CONFIG_CAT_COS_PIN);
#if CONFIG_CAT_COS_ACTIVE_LOW
    return level == 0;
#else
    return level != 0;
#endif
}

void cat_bridge_get_stats(serial_port_stats_t *out)
{
    serial_port_get_stats(s_port, out);
}
