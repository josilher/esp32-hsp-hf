#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "wm8731.h"
#include <string.h>

/* ─── Privado ────────────────────────────────────────────────────────────── */
#define I2C_FREQ_HZ    100000
#define CHECK_ARG(ARG) do { if (!(ARG)) return ESP_ERR_INVALID_ARG; } while (0)

static const char *TAG = "WM8731";

/* Sombra de registros (WM8731 es write-only por I2C) */
static uint16_t reg_shadow[16];

/* ─────────────────────────────────────────────────────────────────────────
 * wm_write_reg
 *
 * El WM8731 tiene registros de 9 bits empaquetados en 2 bytes:
 *   Byte 1 (se envía como "dirección de registro" en i2c_dev_write_reg):
 *     bits [7:1] = reg[6:0]
 *     bit  [0]   = dato[8]
 *   Byte 2 (se envía como "dato"):
 *     bits [7:0] = dato[7:0]
 * ───────────────────────────────────────────────────────────────────────── */
static esp_err_t wm_write_reg(i2c_dev_t *dev, uint8_t reg, uint16_t val)
{
    uint8_t reg_byte  = (uint8_t)((reg << 1) | ((val >> 8) & 0x01));
    uint8_t data_byte = (uint8_t)(val & 0xFF);

    I2C_DEV_TAKE_MUTEX(dev);
    esp_err_t res = i2c_dev_write_reg(dev, reg_byte, &data_byte, 1);
    I2C_DEV_GIVE_MUTEX(dev);

    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Error escribiendo reg 0x%02x val 0x%03x: %s",
                 reg, val, esp_err_to_name(res));
    }
    return res;
}

static esp_err_t wm_write_reg_cached(i2c_dev_t *dev, uint8_t reg, uint16_t val)
{
    if (reg < 16) reg_shadow[reg] = val;
    return wm_write_reg(dev, reg, val);
}

static uint16_t wm_read_cached(uint8_t reg)
{
    return (reg < 16) ? reg_shadow[reg] : 0;
}

/* ─────────────────────────────────────────────────────────────────────────
 * wm8731_init_desc
 * ───────────────────────────────────────────────────────────────────────── */
esp_err_t wm8731_init_desc(i2c_dev_t *dev, i2c_port_t port,
                            gpio_num_t sda_gpio, gpio_num_t scl_gpio,
                            uint8_t addr)
{
    CHECK_ARG(dev);
    memset(reg_shadow, 0, sizeof(reg_shadow));

    dev->dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev->dev_cfg.device_address  = addr;
    dev->dev_cfg.scl_speed_hz    = I2C_FREQ_HZ;
    dev->bus_cfg.i2c_port        = port;
    dev->bus_cfg.sda_io_num      = sda_gpio;
    dev->bus_cfg.scl_io_num      = scl_gpio;
    i2c_setup_device(dev);

    return i2c_dev_create_mutex(dev);
}

esp_err_t wm8731_free_desc(i2c_dev_t *dev)
{
    CHECK_ARG(dev);
    return i2c_dev_delete_mutex(dev);
}

/* ─────────────────────────────────────────────────────────────────────────
 * wm8731_init
 *
 * Secuencia de inicialización vía I2C para Line In → DAC → Line Out.
 *
 * Orden crítico según datasheet WM8731 (sección "Recommended Power-Up"):
 *   1. Reset
 *   2. Configurar todos los registros excepto ACTIVATE
 *   3. Encender alimentación de los bloques necesarios
 *   4. Esperar estabilización
 *   5. Activar interfaz digital (ACTIVATE=1)
 *   6. Desactivar mute del DAC
 * ───────────────────────────────────────────────────────────────────────── */
esp_err_t wm8731_init(i2c_dev_t *dev)
{
    esp_err_t res = ESP_OK;
    ESP_LOGI(TAG, "Iniciando WM8731S...");

    /* 1. Reset hardware del chip */
    res |= wm_write_reg_cached(dev, WM8731_REG_RESET, 0x000);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 2. Volumen Line In: 0 dB en ambos canales
     *    LINVOL=0x17 (23) → 0 dB
     *    LINMUTE=0, LRINBOTH=1 (aplica a dch también)                       */
    res |= wm_write_reg_cached(dev, WM8731_REG_LLINEIN,
                                0x17 | WM8731_LINEIN_BOTH);

    /* 3. Volumen salida: 0 dB en ambos canales
     *    HPVOL=0x79 (121) → 0 dB
     *    LRHPBOTH=1, ZCEN=1 (zero-cross para evitar clics)                  */
    res |= wm_write_reg_cached(dev, WM8731_REG_LHPOUT,
                                0x79 | WM8731_HPOUT_ZCEN | WM8731_HPOUT_BOTH);

    /* 4. Ruta analógica:
     *    INSEL=0  → Line In (no micrófono)
     *    DACSEL=1 → DAC conectado al mixer de salida
     *    BYPASS=0 → sin bypass analógico
     *    MUTEMIC=1 → mic muteado (no se usa)                                */
    res |= wm_write_reg_cached(dev, WM8731_REG_ANAPATH,
                                WM8731_ANAPATH_DACSEL |
                                WM8731_ANAPATH_MUTEMIC);

    /* 5. Ruta digital:
     *    ADCHPD=0  → HPF del ADC activo (elimina offset DC)
     *    De-emphasis para 48 kHz
     *    DACMU=1   → DAC muteado hasta que todo esté listo                  */
    res |= wm_write_reg_cached(dev, WM8731_REG_DIGPATH,
                                WM8731_DIGPATH_DEEMP_48K |
                                WM8731_DIGPATH_DACMU);

    /* 6. Interfaz digital:
     *    FORMAT=10 → I2S estándar (Philips)
     *    IWL=00    → 16 bits
     *    MS=0      → Slave (ESP32 genera los clocks)
     *    LRP=0, LRSWAP=0, BCLKINV=0                                         */
    res |= wm_write_reg_cached(dev, WM8731_REG_DIGIF,
                                (WM8731_FORMAT_I2S & 0x03) |
                                ((WM8731_BITS_16 & 0x03) << 2));

    /* 7. Sampling: 48 kHz en Normal mode (no USB)
     *    USB/NORM=0, BOSR=0, SR=0000, CLKIDIV2=0, CLKODIV2=0
     *    → con MCLK=12.288 MHz y SR=0000: fs = 12288000/256 = 48000 Hz     */
    res |= wm_write_reg_cached(dev, WM8731_REG_SAMPLING, 0x000);

    /* 8. Power Down: apagar solo lo que no usamos
     *    Mic apagado, Osc apagado (usamos MCLK externo), CLKOUT apagado
     *    Line In, ADC, DAC y salida: encendidos                             */
    res |= wm_write_reg_cached(dev, WM8731_REG_POWERDOWN,
                                WM8731_PWR_MICPD    |
                                WM8731_PWR_OSCPD    |
                                WM8731_PWR_CLKOUTPD);

    /* 9. Esperar estabilización de la alimentación interna */
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 10. Activar interfaz digital */
    res |= wm_write_reg_cached(dev, WM8731_REG_ACTIVATE, 0x001);

    /* 11. Desactivar mute del DAC */
    uint16_t digpath = wm_read_cached(WM8731_REG_DIGPATH);
    digpath &= ~WM8731_DIGPATH_DACMU;
    res |= wm_write_reg_cached(dev, WM8731_REG_DIGPATH, digpath);

    if (res == ESP_OK)
        ESP_LOGI(TAG, "WM8731S inicializado correctamente.");
    else
        ESP_LOGE(TAG, "Error en la inicialización (código: 0x%x).", res);

    return res;
}

/* ─────────────────────────────────────────────────────────────────────────
 * wm8731_set_input_volume
 *
 * LINVOL [4:0]: 0x00=-34.5 dB  …  0x1F=+12 dB  (32 pasos, 1.5 dB/paso)
 *               0x17 = 0 dB
 * Mapeamos 0%–100% → 0x00–0x1F
 * ───────────────────────────────────────────────────────────────────────── */
esp_err_t wm8731_set_input_volume(i2c_dev_t *dev, float volume)
{
    if (volume < 0.0f)   volume = 0.0f;
    if (volume > 100.0f) volume = 100.0f;

    uint8_t vol = (uint8_t)((volume / 100.0f) * 0x1F);
    ESP_LOGI(TAG, "Input volume: %.1f%% → reg=0x%02x", volume, vol);

    return wm_write_reg_cached(dev, WM8731_REG_LLINEIN,
                                (uint16_t)(vol | WM8731_LINEIN_BOTH));
}

/* ─────────────────────────────────────────────────────────────────────────
 * wm8731_set_output_volume
 *
 * HPVOL [6:0]: 0x30=silencio(-73 dB)  …  0x79=0 dB  …  0x7F=+6 dB
 * Valores ≤ 0x2F también producen silencio.
 * Mapeamos 0%–100% → 0x30–0x7F  (79 pasos audibles)
 * ───────────────────────────────────────────────────────────────────────── */
esp_err_t wm8731_set_output_volume(i2c_dev_t *dev, float volume)
{
    if (volume < 0.0f)   volume = 0.0f;
    if (volume > 100.0f) volume = 100.0f;

    uint8_t vol = (uint8_t)(0x30 + (uint8_t)((volume / 100.0f) * (0x7F - 0x30)));
    ESP_LOGI(TAG, "Output volume: %.1f%% → reg=0x%02x", volume, vol);

    return wm_write_reg_cached(dev, WM8731_REG_LHPOUT,
                                (uint16_t)(vol | WM8731_HPOUT_ZCEN |
                                           WM8731_HPOUT_BOTH));
}

/* ─────────────────────────────────────────────────────────────────────────
 * wm8731_set_mute
 * ───────────────────────────────────────────────────────────────────────── */
esp_err_t wm8731_set_mute(i2c_dev_t *dev, bool enable)
{
    uint16_t val = wm_read_cached(WM8731_REG_DIGPATH);
    if (enable) val |=  WM8731_DIGPATH_DACMU;
    else        val &= ~WM8731_DIGPATH_DACMU;

    ESP_LOGI(TAG, "DAC mute: %s", enable ? "ON" : "OFF");
    return wm_write_reg_cached(dev, WM8731_REG_DIGPATH, val);
}

/* ─────────────────────────────────────────────────────────────────────────
 * wm8731_set_bypass
 *
 * Conecta Line In directamente a la salida sin pasar por ADC/DAC.
 * Útil para monitorización sin latencia digital.
 * ───────────────────────────────────────────────────────────────────────── */
esp_err_t wm8731_set_bypass(i2c_dev_t *dev, bool enable)
{
    uint16_t val = wm_read_cached(WM8731_REG_ANAPATH);
    if (enable) val |=  WM8731_ANAPATH_BYPASS;
    else        val &= ~WM8731_ANAPATH_BYPASS;

    ESP_LOGI(TAG, "Analogue bypass: %s", enable ? "ON" : "OFF");
    return wm_write_reg_cached(dev, WM8731_REG_ANAPATH, val);
}
