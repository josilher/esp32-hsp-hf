#include "host_link_serial.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "serial_port.h"

static const char *TAG = "host_link_serial";

static host_link_t   s_link;
static serial_port_t *s_port = NULL;

/* ------------------------------------------------------------------------- */
/* /dev/null para silenciar stdout cuando la UART de consola pasa a modo CAT  */
/* ------------------------------------------------------------------------- */

static int null_open(const char *path, int flags, int mode)
{
    (void)path; (void)flags; (void)mode;
    return 0;
}

static int null_close(int fd)
{
    (void)fd;
    return 0;
}

static ssize_t null_write(int fd, const void *data, size_t size)
{
    (void)fd; (void)data;
    return (ssize_t)size;
}

static int null_fstat(int fd, struct stat *st)
{
    (void)fd;
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR;
    return 0;
}

static void mute_console(void)
{
    static const esp_vfs_t null_vfs = {
        .flags = ESP_VFS_FLAG_DEFAULT,
        .open  = &null_open,
        .close = &null_close,
        .write = &null_write,
        .fstat = &null_fstat,
    };
    /* Si ya existe (por ejemplo lo registró otro módulo) no pasa nada. */
    esp_vfs_register("/dev/null", &null_vfs, NULL);

    ESP_LOGI(TAG, "consola silenciada: la UART pasa a modo CAT");
    esp_log_level_set("*", ESP_LOG_NONE);
    fflush(stdout);
    fflush(stderr);
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);
}

/* ------------------------------------------------------------------------- */
/* Transporte                                                                 */
/* ------------------------------------------------------------------------- */

static void frame_cb(serial_port_t *port, const uint8_t *frame, size_t len, void *ctx)
{
    (void)port; (void)ctx;
    host_link_rx_from_task(&s_link, frame, len);
}

static void serial_send(host_link_t *link, const uint8_t *data, size_t len)
{
    (void)link;
    if (s_port) serial_port_write(s_port, data, len);
}

static bool serial_is_connected(host_link_t *link)
{
    (void)link;
    return s_port != NULL;   /* un cable no avisa de si hay alguien escuchando */
}

static const host_link_ops_t s_ops = {
    .name         = "serial",
    .send         = serial_send,
    .is_connected = serial_is_connected,
};

esp_err_t host_link_serial_init(uart_port_t uart_num, int baud_rate, bool mute)
{
    if (s_port) return ESP_ERR_INVALID_STATE;

    if (mute) mute_console();

    /* Los bytes se pasan a host_link en cuanto llegan; el ensamblado de
     * líneas y la detección de binario los hace host_link. */
    serial_port_config_t cfg = SERIAL_PORT_CONFIG_DEFAULT();
    cfg.uart_num        = uart_num;
    cfg.baud_rate       = baud_rate;
    cfg.framing         = SERIAL_FRAMING_RAW;
    cfg.idle_timeout_ms = 5;
    cfg.max_frame_len   = 128;
    cfg.name            = "host";
#if CONFIG_SERIAL_PORT_HOST_SERIAL_ON_CORE1 && !CONFIG_FREERTOS_UNICORE
    cfg.task_core = 1;
#endif

    esp_err_t err = serial_port_open(&cfg, frame_cb, NULL, &s_port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo abrir la UART%d: %s", uart_num, esp_err_to_name(err));
        return err;
    }

    host_link_register(&s_link, &s_ops, NULL);
    return ESP_OK;
}

int host_link_serial_get_core(void)
{
    return (int)serial_port_get_core(s_port);
}
