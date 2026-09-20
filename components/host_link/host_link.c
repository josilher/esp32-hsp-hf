#include "host_link.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "cat_bridge.h"

static const char *TAG = "host_link";

#define HOST_LINK_FW_VERSION  "1.0"
#define HOST_LINK_MSG_MAX     128
#define HOST_LINK_QUEUE_LEN   16

#ifndef CONFIG_HOST_LINK_IDLE_FLUSH_MS
#define CONFIG_HOST_LINK_IDLE_FLUSH_MS 30
#endif

/* Mensaje que cruza de una tarea FreeRTOS al run loop de BTstack.
 * link == NULL significa "viene de la radio": se difunde a todos. */
typedef struct {
    host_link_t *link;
    uint16_t     len;
    uint8_t      data[HOST_LINK_MSG_MAX];
} host_msg_t;

static host_link_t  *s_links = NULL;
static QueueHandle_t s_queue = NULL;
static atomic_bool   s_drain_scheduled = false;
static btstack_context_callback_registration_t s_drain_reg;

static uint32_t s_radio_frames  = 0;   /* tramas recibidas de la radio */
static uint32_t s_dropped_msgs  = 0;   /* mensajes perdidos por cola llena */

/* ------------------------------------------------------------------------- */
/* Salida                                                                     */
/* ------------------------------------------------------------------------- */

void host_link_send(host_link_t *link, const uint8_t *data, size_t len)
{
    if (!link || !link->ops || !link->ops->send || len == 0) return;
    link->ops->send(link, data, len);
    link->tx_bytes += (uint32_t)len;
}

void host_link_send_str(host_link_t *link, const char *str)
{
    host_link_send(link, (const uint8_t *)str, strlen(str));
}

void host_link_broadcast(const uint8_t *data, size_t len)
{
    for (host_link_t *l = s_links; l; l = l->next) {
        if (l->ops->is_connected && !l->ops->is_connected(l)) continue;
        host_link_send(l, data, len);
    }
}

static void forward_to_radio(const uint8_t *data, size_t len)
{
    if (len == 0) return;
    esp_err_t err = cat_bridge_send(data, (uint16_t)len);
    if (err != ESP_OK) ESP_LOGW(TAG, "envio CAT fallido: %s", esp_err_to_name(err));
}

/* ------------------------------------------------------------------------- */
/* Comandos de control                                                        */
/* ------------------------------------------------------------------------- */

static bool eq(const uint8_t *line, size_t len, const char *cmd)
{
    size_t n = strlen(cmd);
    return len == n && memcmp(line, cmd, n) == 0;
}

static bool starts(const uint8_t *line, size_t len, const char *prefix)
{
    size_t n = strlen(prefix);
    return len >= n && memcmp(line, prefix, n) == 0;
}

static void send_info(host_link_t *link)
{
    serial_port_stats_t st;
    cat_bridge_get_stats(&st);

    char buf[320];
    int n = snprintf(buf, sizeof(buf),
                     "INFO fw=%s cat_baud=%d ptt=%d cos=%d "
                     "cat_rx=%u cat_tx=%u cat_frames=%u cat_overruns=%u cat_dropped=%u "
                     "radio_frames=%u queue_dropped=%u links=",
                     HOST_LINK_FW_VERSION, cat_bridge_get_baud(),
                     cat_bridge_get_ptt() ? 1 : 0, cat_bridge_get_cos() ? 1 : 0,
                     (unsigned)st.rx_bytes, (unsigned)st.tx_bytes, (unsigned)st.rx_frames,
                     (unsigned)st.overruns, (unsigned)st.dropped_frames,
                     (unsigned)s_radio_frames, (unsigned)s_dropped_msgs);

    bool first = true;
    for (host_link_t *l = s_links; l && n > 0 && (size_t)n < sizeof(buf); l = l->next) {
        bool up = l->ops->is_connected ? l->ops->is_connected(l) : true;
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%s:%d", first ? "" : ",",
                      l->ops->name, up ? 1 : 0);
        first = false;
    }
    if (n > 0 && (size_t)n < sizeof(buf) - 2) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "\r\n");
    }
    if (n > 0) host_link_send(link, (const uint8_t *)buf, (size_t)n);
}

/* Devuelve true si la línea era un comando de control (ya respondido). */
static bool handle_command(host_link_t *link, const uint8_t *line, size_t len)
{
    if (len == 0) return true;                       /* delimitador suelto: ignorar */

    if (eq(line, len, "PING")) {
        host_link_send_str(link, "PONG\r\n");
    } else if (eq(line, len, "PTT=1")) {
        cat_bridge_set_ptt(true);
        host_link_send_str(link, "OK\r\n");
    } else if (eq(line, len, "PTT=0")) {
        cat_bridge_set_ptt(false);
        host_link_send_str(link, "OK\r\n");
    } else if (eq(line, len, "PTT?")) {
        host_link_send_str(link, cat_bridge_get_ptt() ? "PTT=1\r\n" : "PTT=0\r\n");
    } else if (eq(line, len, "COS?")) {
        host_link_send_str(link, cat_bridge_get_cos() ? "COS=1\r\n" : "COS=0\r\n");
    } else if (starts(line, len, "BAUD=")) {
        int baud = atoi((const char *)line + 5);
        host_link_send_str(link, cat_bridge_set_baud(baud) == ESP_OK ? "OK\r\n" : "ERR\r\n");
    } else if (eq(line, len, "INFO")) {
        send_info(link);
    } else {
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Entrada del host                                                           */
/* ------------------------------------------------------------------------- */

static bool is_delimiter(uint8_t b)
{
    if (b == '\r' || b == '\n') return true;
#if CONFIG_CAT_FRAMING_TERMINATOR
    if (b == (uint8_t)CONFIG_CAT_TERMINATOR) return true;
#endif
    return false;
}

static bool has_binary(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if ((buf[i] < 0x20 || buf[i] > 0x7E) && buf[i] != '\t') return true;
    }
    return false;
}

static void idle_timer_handler(btstack_timer_source_t *ts)
{
    host_link_t *link = (host_link_t *)btstack_run_loop_get_timer_context(ts);
    if (!link || link->rx_len == 0) return;

    /* Silencio sin delimitador. Si parece binario (protocolo CAT sin
     * terminador) se reenvía tal cual. Si es texto, seguimos esperando el
     * fin de línea: probablemente alguien teclea en un terminal. */
    if (has_binary(link->rx_buf, link->rx_len)) {
        forward_to_radio(link->rx_buf, link->rx_len);
        link->rx_len = 0;
    }
}

static void restart_idle_timer(host_link_t *link)
{
    if (CONFIG_HOST_LINK_IDLE_FLUSH_MS <= 0) return;
    btstack_run_loop_remove_timer(&link->idle_timer);
    btstack_run_loop_set_timer(&link->idle_timer, CONFIG_HOST_LINK_IDLE_FLUSH_MS);
    btstack_run_loop_add_timer(&link->idle_timer);
}

void host_link_rx(host_link_t *link, const uint8_t *data, size_t len)
{
    if (!link) return;

    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];

        if (is_delimiter(b)) {
            link->rx_buf[link->rx_len] = '\0';
            link->rx_frames++;
            if (!handle_command(link, link->rx_buf, link->rx_len)) {
                link->rx_buf[link->rx_len++] = b;          /* reenviar con su delimitador */
                forward_to_radio(link->rx_buf, link->rx_len);
            }
            link->rx_len = 0;
            btstack_run_loop_remove_timer(&link->idle_timer);
            continue;
        }

        if (link->rx_len >= HOST_LINK_MAX_LINE) {
            /* Flujo sin delimitador: pasar lo acumulado y seguir */
            forward_to_radio(link->rx_buf, link->rx_len);
            link->rx_len = 0;
        }
        link->rx_buf[link->rx_len++] = b;
    }

    if (link->rx_len > 0) restart_idle_timer(link);
}

/* ------------------------------------------------------------------------- */
/* Cruce de contexto: tareas FreeRTOS -> run loop de BTstack                 */
/* ------------------------------------------------------------------------- */

static void drain_queue(void *context)
{
    (void)context;
    atomic_store(&s_drain_scheduled, false);

    host_msg_t msg;
    while (xQueueReceive(s_queue, &msg, 0) == pdTRUE) {
        if (msg.link) {
            host_link_rx(msg.link, msg.data, msg.len);
        } else {
            s_radio_frames++;
            host_link_broadcast(msg.data, msg.len);
        }
    }
}

static void enqueue_from_task(host_link_t *link, const uint8_t *data, size_t len)
{
    if (!s_queue) return;

    while (len > 0) {
        host_msg_t msg;
        msg.link = link;
        msg.len  = (uint16_t)(len > HOST_LINK_MSG_MAX ? HOST_LINK_MSG_MAX : len);
        memcpy(msg.data, data, msg.len);
        if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
            s_dropped_msgs++;
            break;
        }
        data += msg.len;
        len  -= msg.len;
    }

    if (!atomic_exchange(&s_drain_scheduled, true)) {
        btstack_run_loop_execute_on_main_thread(&s_drain_reg);
    }
}

void host_link_rx_from_task(host_link_t *link, const uint8_t *data, size_t len)
{
    enqueue_from_task(link, data, len);
}

static void radio_rx_cb(const uint8_t *data, uint16_t len)
{
    enqueue_from_task(NULL, data, len);
}

/* ------------------------------------------------------------------------- */
/* Registro e inicialización                                                  */
/* ------------------------------------------------------------------------- */

void host_link_register(host_link_t *link, const host_link_ops_t *ops, void *impl)
{
    if (!link || !ops) return;
    memset(link, 0, sizeof(*link));
    link->ops  = ops;
    link->impl = impl;
    btstack_run_loop_set_timer_handler(&link->idle_timer, idle_timer_handler);
    btstack_run_loop_set_timer_context(&link->idle_timer, link);

    /* al final de la lista, para que INFO los liste en orden de registro */
    host_link_t **pp = &s_links;
    while (*pp) pp = &(*pp)->next;
    *pp = link;

    ESP_LOGI(TAG, "enlace registrado: %s", ops->name);
}

void host_link_init(void)
{
    if (s_queue) return;

    s_queue = xQueueCreate(HOST_LINK_QUEUE_LEN, sizeof(host_msg_t));
    s_drain_reg.callback = drain_queue;
    s_drain_reg.context  = NULL;

    esp_err_t err = cat_bridge_init(radio_rx_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cat_bridge no disponible: %s", esp_err_to_name(err));
    }
}
