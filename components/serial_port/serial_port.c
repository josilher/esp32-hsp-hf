#include "serial_port.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "serial_port";

#define UART_EVENT_QUEUE_LEN  20
#define RX_CHUNK_SIZE         128
#define MIN_RX_BUF_SIZE       256
#define CLOSE_TIMEOUT_MS      500

struct serial_port {
    serial_port_config_t cfg;
    serial_frame_cb_t    cb;
    void                *ctx;

    QueueHandle_t        events;      /* cola de eventos del driver UART */
    TaskHandle_t         task;
    SemaphoreHandle_t    tx_mutex;
    SemaphoreHandle_t    task_done;
    volatile bool        running;

    uint8_t             *frame;       /* max_frame_len + 1 (NUL) */
    size_t               frame_len;
    bool                 frame_overflow;

    serial_port_stats_t  stats;
};

/* ------------------------------------------------------------------------- */
/* Ensamblado de tramas                                                       */
/* ------------------------------------------------------------------------- */

static void deliver(serial_port_t *p)
{
    if (p->frame_len == 0) return;
    p->frame[p->frame_len] = '\0';
    p->stats.rx_frames++;
    if (p->cb) p->cb(p, p->frame, p->frame_len, p->ctx);
    p->frame_len = 0;
}

static void feed_byte(serial_port_t *p, uint8_t b)
{
    const serial_framing_t f = p->cfg.framing;

    if (f == SERIAL_FRAMING_LINE && (b == '\r' || b == '\n')) {
        if (p->frame_overflow) p->frame_overflow = false;   /* fin de la línea descartada */
        else                   deliver(p);
        p->frame_len = 0;
        return;
    }

    if (p->frame_overflow) {
        /* Descartando una trama demasiado larga hasta encontrar su fin */
        if (f == SERIAL_FRAMING_TERMINATOR && b == p->cfg.terminator) {
            p->frame_overflow = false;
            p->frame_len = 0;
        }
        return;
    }

    if (p->frame_len >= p->cfg.max_frame_len) {
        if (f == SERIAL_FRAMING_RAW) {
            deliver(p);                     /* en RAW el tamaño máximo cierra la trama */
        } else {
            p->stats.dropped_frames++;
            p->frame_overflow = true;
            p->frame_len = 0;
            return;
        }
    }

    p->frame[p->frame_len++] = b;

    if (f == SERIAL_FRAMING_TERMINATOR && b == p->cfg.terminator) {
        deliver(p);
    }
}

/* ------------------------------------------------------------------------- */
/* Tarea de recepción                                                         */
/* ------------------------------------------------------------------------- */

static void rx_task(void *arg)
{
    serial_port_t *p = (serial_port_t *)arg;
    uart_event_t ev;
    uint8_t chunk[RX_CHUNK_SIZE];

    /* En RAW el timeout de la cola es el detector de silencio. En los demás
     * modos solo sirve para comprobar periódicamente si hay que salir. */
    uint32_t wait_ms = (p->cfg.framing == SERIAL_FRAMING_RAW && p->cfg.idle_timeout_ms)
                       ? p->cfg.idle_timeout_ms : 100;
    const TickType_t wait = pdMS_TO_TICKS(wait_ms) ? pdMS_TO_TICKS(wait_ms) : 1;

    while (p->running) {
        if (xQueueReceive(p->events, &ev, wait) != pdTRUE) {
            if (p->cfg.framing == SERIAL_FRAMING_RAW) deliver(p);   /* silencio: cerrar trama */
            continue;
        }

        switch (ev.type) {
        case UART_DATA: {
            size_t remaining = ev.size;
            while (remaining > 0) {
                size_t want = remaining > RX_CHUNK_SIZE ? RX_CHUNK_SIZE : remaining;
                int n = uart_read_bytes(p->cfg.uart_num, chunk, want, pdMS_TO_TICKS(10));
                if (n <= 0) break;
                p->stats.rx_bytes += (uint32_t)n;
                for (int i = 0; i < n; i++) feed_byte(p, chunk[i]);
                remaining -= (size_t)n;
            }
            break;
        }
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
            p->stats.overruns++;
            uart_flush_input(p->cfg.uart_num);
            xQueueReset(p->events);
            p->frame_len = 0;
            p->frame_overflow = false;
            ESP_LOGW(TAG, "%s: desbordamiento de recepcion, datos perdidos", p->cfg.name);
            break;
        case UART_FRAME_ERR:
        case UART_PARITY_ERR:
            p->stats.line_errors++;
            break;
        default:
            break;
        }
    }

    xSemaphoreGive(p->task_done);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------------- */
/* API                                                                        */
/* ------------------------------------------------------------------------- */

static void free_port(serial_port_t *p)
{
    if (p->tx_mutex)  vSemaphoreDelete(p->tx_mutex);
    if (p->task_done) vSemaphoreDelete(p->task_done);
    free(p->frame);
    free(p);
}

esp_err_t serial_port_open(const serial_port_config_t *cfg, serial_frame_cb_t cb, void *ctx,
                           serial_port_t **out)
{
    if (!cfg || !out || cfg->max_frame_len == 0 || cfg->uart_num >= UART_NUM_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (uart_is_driver_installed(cfg->uart_num)) {
        ESP_LOGE(TAG, "%s: UART%d ya tiene un driver instalado", cfg->name, cfg->uart_num);
        return ESP_ERR_INVALID_STATE;
    }

    serial_port_t *p = calloc(1, sizeof(*p));
    if (!p) return ESP_ERR_NO_MEM;
    p->cfg = *cfg;
    p->cb  = cb;
    p->ctx = ctx;
    if (!p->cfg.name) p->cfg.name = "serial";
    if (p->cfg.rx_buf_size < MIN_RX_BUF_SIZE) p->cfg.rx_buf_size = MIN_RX_BUF_SIZE;

    p->frame     = malloc(p->cfg.max_frame_len + 1);
    p->tx_mutex  = xSemaphoreCreateMutex();
    p->task_done = xSemaphoreCreateBinary();
    if (!p->frame || !p->tx_mutex || !p->task_done) {
        free_port(p);
        return ESP_ERR_NO_MEM;
    }

    uart_config_t uc = {
        .baud_rate  = p->cfg.baud_rate,
        .data_bits  = p->cfg.data_bits,
        .parity     = p->cfg.parity,
        .stop_bits  = p->cfg.stop_bits,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(p->cfg.uart_num, &uc);
    if (err == ESP_OK) {
        err = uart_set_pin(p->cfg.uart_num, p->cfg.tx_pin, p->cfg.rx_pin,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
        err = uart_driver_install(p->cfg.uart_num, (int)p->cfg.rx_buf_size, (int)p->cfg.tx_buf_size,
                                  UART_EVENT_QUEUE_LEN, &p->events, 0);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: error configurando UART%d: %s", p->cfg.name, p->cfg.uart_num,
                 esp_err_to_name(err));
        free_port(p);
        return err;
    }

    p->running = true;
    char task_name[configMAX_TASK_NAME_LEN];
    snprintf(task_name, sizeof(task_name), "%.*s_rx", (int)(sizeof(task_name) - 4), p->cfg.name);

    /* El core 1 esta apagado a nivel de reloj en un build unicore (no solo
     * excluido del scheduler): pedirlo ahi no tiene efecto util y el kernel
     * no lo degrada de forma predecible. Se decide en compilacion, nunca se
     * confia en que el propio xTaskCreatePinnedToCore lo resuelva. */
#if CONFIG_FREERTOS_UNICORE
    BaseType_t core = tskNO_AFFINITY;
    if (p->cfg.task_core != tskNO_AFFINITY) {
        ESP_LOGW(TAG, "%s: afinidad de nucleo %d ignorada (build unicore)",
                 p->cfg.name, (int)p->cfg.task_core);
    }
#else
    BaseType_t core = p->cfg.task_core;
#endif

    if (xTaskCreatePinnedToCore(rx_task, task_name, p->cfg.task_stack, p, p->cfg.task_priority,
                                &p->task, core) != pdPASS) {
        ESP_LOGE(TAG, "%s: no se pudo crear la tarea de recepcion", p->cfg.name);
        uart_driver_delete(p->cfg.uart_num);
        free_port(p);
        return ESP_ERR_NO_MEM;
    }
    if (core != tskNO_AFFINITY) {
        ESP_LOGI(TAG, "%s: tarea de recepcion anclada al nucleo %d", p->cfg.name, (int)core);
    }

    ESP_LOGI(TAG, "%s: UART%d a %d baudios (TX=%d RX=%d), framing=%d",
             p->cfg.name, p->cfg.uart_num, p->cfg.baud_rate, p->cfg.tx_pin, p->cfg.rx_pin,
             p->cfg.framing);
    *out = p;
    return ESP_OK;
}

esp_err_t serial_port_close(serial_port_t *p)
{
    if (!p) return ESP_ERR_INVALID_ARG;

    p->running = false;
    if (xSemaphoreTake(p->task_done, pdMS_TO_TICKS(CLOSE_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "%s: la tarea no termino a tiempo, forzando", p->cfg.name);
        vTaskDelete(p->task);
    }
    esp_err_t err = uart_driver_delete(p->cfg.uart_num);
    free_port(p);
    return err;
}

esp_err_t serial_port_write(serial_port_t *p, const uint8_t *data, size_t len)
{
    if (!p || (!data && len)) return ESP_ERR_INVALID_ARG;
    if (len == 0) return ESP_OK;

    if (xSemaphoreTake(p->tx_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    int n = uart_write_bytes(p->cfg.uart_num, data, len);
    xSemaphoreGive(p->tx_mutex);

    if (n < 0) return ESP_FAIL;
    p->stats.tx_bytes += (uint32_t)n;
    return ((size_t)n == len) ? ESP_OK : ESP_FAIL;
}

esp_err_t serial_port_write_str(serial_port_t *p, const char *str)
{
    if (!str) return ESP_ERR_INVALID_ARG;
    return serial_port_write(p, (const uint8_t *)str, strlen(str));
}

esp_err_t serial_port_set_baud(serial_port_t *p, int baud_rate)
{
    if (!p || baud_rate <= 0) return ESP_ERR_INVALID_ARG;
    esp_err_t err = uart_set_baudrate(p->cfg.uart_num, (uint32_t)baud_rate);
    if (err == ESP_OK) {
        p->cfg.baud_rate = baud_rate;
        ESP_LOGI(TAG, "%s: velocidad cambiada a %d", p->cfg.name, baud_rate);
    }
    return err;
}

int serial_port_get_baud(const serial_port_t *p)
{
    return p ? p->cfg.baud_rate : 0;
}

void serial_port_get_stats(const serial_port_t *p, serial_port_stats_t *out)
{
    if (!out) return;
    if (p) *out = p->stats;
    else   memset(out, 0, sizeof(*out));
}

const char *serial_port_get_name(const serial_port_t *p)
{
    return p ? p->cfg.name : "";
}

uart_port_t serial_port_get_uart(const serial_port_t *p)
{
    return p ? p->cfg.uart_num : UART_NUM_MAX;
}

BaseType_t serial_port_get_core(const serial_port_t *p)
{
    if (!p || !p->task) return -1;
    return xTaskGetCoreID(p->task);
}
