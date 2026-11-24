/**
 * @file i2cdev.c
 *
 * Acceso I2C seguro entre tareas sobre el driver I2C maestro de ESP-IDF 5.
 * Ver i2cdev.h para la interfaz y la licencia.
 */
#include "i2cdev.h"

#include <string.h>
#include <esp_log.h>
#include <freertos/task.h>

static const char *TAG = "i2cdev";

#ifndef CONFIG_I2CDEV_TIMEOUT
#define CONFIG_I2CDEV_TIMEOUT 1000
#endif

/* Un bus por puerto. El driver nuevo no deja abrir dos veces el mismo puerto,
 * y varios dispositivos (el codec y cualquier otro) comparten terminales, asi
 * que el bus se abre una vez y se reaprovecha. */
#define I2CDEV_MAX_BUSES  SOC_I2C_NUM

static struct {
    i2c_master_bus_handle_t handle;
    bool abierto;
} s_bus[I2CDEV_MAX_BUSES];

static SemaphoreHandle_t s_lock = NULL;

#define CHECK_ARG(X) do { if (!(X)) return ESP_ERR_INVALID_ARG; } while (0)

esp_err_t i2cdev_init(void)
{
    if (s_lock) return ESP_OK;              /* ya preparada */

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "no hay memoria para el mutex de la libreria");
        return ESP_ERR_NO_MEM;
    }
    memset(s_bus, 0, sizeof(s_bus));
    return ESP_OK;
}

esp_err_t i2cdev_done(void)
{
    if (!s_lock) return ESP_OK;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < I2CDEV_MAX_BUSES; i++) {
        if (s_bus[i].abierto) {
            i2c_del_master_bus(s_bus[i].handle);
            s_bus[i].abierto = false;
            s_bus[i].handle = NULL;
        }
    }
    xSemaphoreGive(s_lock);

    vSemaphoreDelete(s_lock);
    s_lock = NULL;
    return ESP_OK;
}

esp_err_t i2c_setup_device(i2c_dev_t *dev)
{
    CHECK_ARG(dev);
    if (!s_lock) {
        ESP_LOGE(TAG, "i2cdev_init() no se ha llamado");
        return ESP_ERR_INVALID_STATE;
    }

    int puerto = dev->bus_cfg.i2c_port;
    if (puerto < 0 || puerto >= I2CDEV_MAX_BUSES) {
        ESP_LOGE(TAG, "puerto I2C %d fuera de rango", puerto);
        return ESP_ERR_INVALID_ARG;
    }

    /* Valores que el llamante no suele rellenar */
    if (dev->bus_cfg.clk_source == 0)
        dev->bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    if (dev->bus_cfg.glitch_ignore_cnt == 0)
        dev->bus_cfg.glitch_ignore_cnt = 7;
    dev->bus_cfg.flags.enable_internal_pullup = true;
    if (dev->dev_cfg.scl_speed_hz == 0)
        dev->dev_cfg.scl_speed_hz = 100000;

    esp_err_t res = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (!s_bus[puerto].abierto) {
        res = i2c_new_master_bus(&dev->bus_cfg, &s_bus[puerto].handle);
        if (res != ESP_OK) {
            ESP_LOGE(TAG, "no se pudo abrir el bus I2C %d: %s",
                     puerto, esp_err_to_name(res));
            xSemaphoreGive(s_lock);
            return res;
        }
        s_bus[puerto].abierto = true;
        ESP_LOGI(TAG, "bus I2C %d abierto (SDA=%d, SCL=%d)", puerto,
                 dev->bus_cfg.sda_io_num, dev->bus_cfg.scl_io_num);
    }
    dev->bus_handle = s_bus[puerto].handle;

    xSemaphoreGive(s_lock);

    res = i2c_master_bus_add_device(dev->bus_handle, &dev->dev_cfg,
                                    &dev->dev_handle);
    if (res != ESP_OK)
        ESP_LOGE(TAG, "no se pudo anadir el dispositivo 0x%02x: %s",
                 (unsigned)dev->dev_cfg.device_address, esp_err_to_name(res));
    return res;
}

esp_err_t i2c_dev_create_mutex(i2c_dev_t *dev)
{
    CHECK_ARG(dev);
    if (dev->mutex) return ESP_OK;

    dev->mutex = xSemaphoreCreateMutex();
    if (!dev->mutex) {
        ESP_LOGE(TAG, "no hay memoria para el mutex del dispositivo 0x%02x",
                 (unsigned)dev->dev_cfg.device_address);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t i2c_dev_delete_mutex(i2c_dev_t *dev)
{
    CHECK_ARG(dev);
    if (!dev->mutex) return ESP_OK;

    vSemaphoreDelete(dev->mutex);
    dev->mutex = NULL;
    return ESP_OK;
}

esp_err_t i2c_dev_take_mutex(i2c_dev_t *dev)
{
    CHECK_ARG(dev);
    CHECK_ARG(dev->mutex);

    if (!xSemaphoreTake(dev->mutex, pdMS_TO_TICKS(CONFIG_I2CDEV_TIMEOUT))) {
        ESP_LOGE(TAG, "se agoto la espera del mutex del dispositivo 0x%02x",
                 (unsigned)dev->dev_cfg.device_address);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t i2c_dev_give_mutex(i2c_dev_t *dev)
{
    CHECK_ARG(dev);
    CHECK_ARG(dev->mutex);

    if (!xSemaphoreGive(dev->mutex)) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t i2c_dev_write(const i2c_dev_t *dev, const void *out_reg,
        size_t out_reg_size, const void *out_data, size_t out_size)
{
    CHECK_ARG(dev);
    CHECK_ARG(dev->dev_handle);
    CHECK_ARG(out_data && out_size);

    /* El driver nuevo escribe de un tiron, asi que el registro y los datos se
     * juntan antes en un buffer temporal. */
    uint8_t pila[16];
    size_t total = out_reg_size + out_size;
    uint8_t *buf = pila;

    if (total > sizeof(pila)) {
        buf = malloc(total);
        if (!buf) return ESP_ERR_NO_MEM;
    }
    if (out_reg && out_reg_size) memcpy(buf, out_reg, out_reg_size);
    memcpy(buf + out_reg_size, out_data, out_size);

    esp_err_t res = i2c_master_transmit(dev->dev_handle, buf, total,
                                        CONFIG_I2CDEV_TIMEOUT);
    if (buf != pila) free(buf);
    return res;
}

esp_err_t i2c_dev_write_reg(const i2c_dev_t *dev, uint8_t reg,
        const void *out_data, size_t out_size)
{
    return i2c_dev_write(dev, &reg, 1, out_data, out_size);
}

esp_err_t i2c_dev_read(const i2c_dev_t *dev, const void *out_data,
        size_t out_size, void *in_data, size_t in_size)
{
    CHECK_ARG(dev);
    CHECK_ARG(dev->dev_handle);
    CHECK_ARG(in_data && in_size);

    if (out_data && out_size)
        return i2c_master_transmit_receive(dev->dev_handle, out_data, out_size,
                                           in_data, in_size,
                                           CONFIG_I2CDEV_TIMEOUT);
    return i2c_master_receive(dev->dev_handle, in_data, in_size,
                              CONFIG_I2CDEV_TIMEOUT);
}

esp_err_t i2c_dev_read_reg(const i2c_dev_t *dev, uint8_t reg,
        void *in_data, size_t in_size)
{
    return i2c_dev_read(dev, &reg, 1, in_data, in_size);
}
