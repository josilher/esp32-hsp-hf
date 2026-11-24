/**
 * @file i2cdev.h
 * @defgroup i2cdev i2cdev
 * @{
 *
 * Acceso I2C seguro entre tareas, sobre el driver I2C maestro de ESP-IDF 5
 * (driver/i2c_master.h).
 *
 * La interfaz reproduce la de la libreria i2cdev de esp-idf-lib, de modo que
 * los drivers escritos contra aquella compilan sin cambios. La libreria
 * original esta escrita para el driver I2C antiguo (driver/i2c.h), que ESP-IDF
 * 5 da por obsoleto; esta implementacion es propia y usa el driver nuevo.
 *
 * Interfaz original: Copyright (C) 2018 Ruslan V. Uss <https://github.com/UncleRus>
 * Esta implementacion: Copyright (C) 2026 Jose Luis Silvan Hernandez
 * Las dos con licencia MIT; ver el fichero LICENSE de este directorio.
 */
#ifndef __I2CDEV_H__
#define __I2CDEV_H__

#include "driver/i2c_master.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Descriptor de un dispositivo I2C.
 *
 * El llamante rellena @c bus_cfg (puerto y terminales) y @c dev_cfg
 * (direccion y velocidad) y pasa el descriptor a i2c_setup_device(), que se
 * encarga de @c bus_handle y @c dev_handle.
 */
typedef struct {
    i2c_master_bus_config_t bus_cfg;    //!< Configuracion del bus: puerto, SDA y SCL
    i2c_device_config_t dev_cfg;        //!< Configuracion del dispositivo: direccion y velocidad
    i2c_master_bus_handle_t bus_handle; //!< Manejador del bus, lo rellena i2c_setup_device()
    i2c_master_dev_handle_t dev_handle; //!< Manejador del dispositivo, idem
    SemaphoreHandle_t mutex;            //!< Exclusion mutua del dispositivo
    uint32_t timeout_ticks;             //!< Sin usar; se conserva por compatibilidad de la interfaz
} i2c_dev_t;

/**
 * @brief Prepara la libreria.
 *
 * Hay que llamarla antes que a cualquier otra funcion. Es idempotente.
 */
esp_err_t i2cdev_init(void);

/**
 * @brief Suelta los recursos de la libreria y los buses abiertos.
 */
esp_err_t i2cdev_done(void);

/**
 * @brief Da de alta el dispositivo en su bus.
 *
 * Abre el bus del puerto indicado en @c bus_cfg la primera vez que se usa y lo
 * reaprovecha en las llamadas siguientes, de modo que varios dispositivos
 * pueden compartir terminales. Despues anade el dispositivo al bus.
 */
esp_err_t i2c_setup_device(i2c_dev_t *dev);

/** @brief Crea el mutex del descriptor. */
esp_err_t i2c_dev_create_mutex(i2c_dev_t *dev);

/** @brief Destruye el mutex del descriptor. */
esp_err_t i2c_dev_delete_mutex(i2c_dev_t *dev);

/** @brief Toma el mutex del descriptor. */
esp_err_t i2c_dev_take_mutex(i2c_dev_t *dev);

/** @brief Suelta el mutex del descriptor. */
esp_err_t i2c_dev_give_mutex(i2c_dev_t *dev);

/**
 * @brief Escribe en el dispositivo.
 *
 * Si @p out_reg no es NULL, sus @p out_reg_size bytes se envian delante de los
 * datos en la misma transaccion.
 */
esp_err_t i2c_dev_write(const i2c_dev_t *dev, const void *out_reg,
        size_t out_reg_size, const void *out_data, size_t out_size);

/** @brief Escribe @p out_size bytes en el registro @p reg. */
esp_err_t i2c_dev_write_reg(const i2c_dev_t *dev, uint8_t reg,
        const void *out_data, size_t out_size);

/**
 * @brief Lee del dispositivo.
 *
 * Si @p out_data no es NULL, se escribe primero y se repite el arranque antes
 * de leer.
 */
esp_err_t i2c_dev_read(const i2c_dev_t *dev, const void *out_data,
        size_t out_size, void *in_data, size_t in_size);

/** @brief Lee @p in_size bytes del registro @p reg. */
esp_err_t i2c_dev_read_reg(const i2c_dev_t *dev, uint8_t reg,
        void *in_data, size_t in_size);

#define I2C_DEV_TAKE_MUTEX(dev) do { \
        esp_err_t __ = i2c_dev_take_mutex(dev); \
        if (__ != ESP_OK) return __;\
    } while (0)

#define I2C_DEV_GIVE_MUTEX(dev) do { \
        esp_err_t __ = i2c_dev_give_mutex(dev); \
        if (__ != ESP_OK) return __;\
    } while (0)

#define I2C_DEV_CHECK(dev,X) do { \
        esp_err_t ___ = X; \
        if (___ != ESP_OK) { \
            I2C_DEV_GIVE_MUTEX(dev); \
            return ___; \
        } \
    } while (0)

#ifdef __cplusplus
}
#endif

/**@}*/

#endif /* __I2CDEV_H__ */
