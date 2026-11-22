#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "wm8731.h"
#include <string.h>
#include <inttypes.h>

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
 * Secuencia de inicialización vía I2C para Mic In → ADC → I2S y para
 * I2S → DAC → Headphone Out, que es el encaminamiento de esta placa.
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
     *    INSEL=1  → entrada de MICRÓFONO (MICIN) como fuente del ADC
     *    MUTEMIC=0 → sin mutear esa entrada
     *    DACSEL=1 → DAC conectado al mixer de salida
     *    BYPASS=0 → sin bypass analógico
     *    MICBOOST=0 → sin los +20 dB de la etapa de microfono. Se activo en
     *                 su momento porque no se conseguia captar voz, pero la
     *                 causa real era el cableado del banco de pruebas. Con
     *                 el cableado corregido el nivel es suficiente sin el
     *                 boost, y dejarlo puesto solo acerca la entrada a la
     *                 saturacion. Tampoco procede con una radio real, que
     *                 entrega nivel de linea.
     *
     * En la placa bluesp32 el audio que llega de la radio entra por MICIN
     * (pin 18, via R16/C15 y el transformador T2) y las patillas de LINE IN
     * (19 y 20) estan SIN CONECTAR. Por eso hay que seleccionar la entrada
     * de microfono: con INSEL=0 el ADC escucharia una entrada inexistente.
     * El prototipo anterior, hecho a mano, si usaba LINE IN.                */
    res |= wm_write_reg_cached(dev, WM8731_REG_ANAPATH,
                                WM8731_ANAPATH_DACSEL |
                                WM8731_ANAPATH_INSEL);

    /* 5. Ruta digital:
     *    ADCHPD=0  → HPF del ADC activo (elimina offset DC del microfono)
     *    DEEMP=00  → sin de-enfasis. El audio de HFP va a 8 o 16 kHz y no
     *                lleva pre-enfasis, asi que aplicarlo (y encima con la
     *                curva de 48 kHz) solo deformaria la reproduccion.
     *    DACMU=1   → DAC muteado hasta que todo esté listo                  */
    res |= wm_write_reg_cached(dev, WM8731_REG_DIGPATH,
                                WM8731_DIGPATH_DEEMP_NONE |
                                WM8731_DIGPATH_DACMU);

    /* 6. Interfaz digital:
     *    FORMAT=10 → I2S estándar (Philips)
     *    IWL=00    → 16 bits
     *    MS=0      → Slave (ESP32 genera los clocks)
     *    LRP=0, LRSWAP=0, BCLKINV=0                                         */
    res |= wm_write_reg_cached(dev, WM8731_REG_DIGIF,
                                (WM8731_FORMAT_I2S & 0x03) |
                                ((WM8731_BITS_16 & 0x03) << 2));

    /* 7. Sampling: valor provisional, Normal mode, fs = MCLK/256
     *    USB/NORM=0, BOSR=0, SR=0000, CLKIDIV2=0, CLKODIV2=0
     *    El valor definitivo depende del reloj que llega de verdad al
     *    terminal MCLK y de la frecuencia del canal de voz, y lo fija
     *    wm8731_set_clock() cada vez que se crea el canal I2S. En el ESP32
     *    ese reloj no es 256*fs sino el del APLL (512*fs a 16 kHz y 768*fs
     *    a 8 kHz): ver btstack_audio_esp32_v5.c.                           */
    res |= wm_write_reg_cached(dev, WM8731_REG_SAMPLING, 0x000);

    /* 8. Power Down: apagar solo lo que no usamos
     *    MICPD=0    → entrada de MICROFONO ENCENDIDA (es la que usa esta
     *                 placa; si se apaga, el ADC solo capta ruido de fondo)
     *    LINEINPD=1 → Line In apagado, en esta placa no esta conectado
     *    OSCPD=1    → oscilador apagado, usamos el MCLK externo del ESP32
     *    CLKOUTPD=1 → salida de reloj apagada
     *    ADC, DAC y salida: encendidos                                      */
    res |= wm_write_reg_cached(dev, WM8731_REG_POWERDOWN,
                                WM8731_PWR_LINEINPD |
                                WM8731_PWR_OSCPD    |
                                WM8731_PWR_CLKOUTPD);

    /* 9. Esperar estabilización de la alimentación interna */
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 10. Activar interfaz digital */
    res |= wm_write_reg_cached(dev, WM8731_REG_ACTIVATE, 0x001);
    vTaskDelay(pdMS_TO_TICKS(10));

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
 * Mapeamos 0%–100% → 0x30–0x79  (73 pasos audibles)
 *
 * El tope es 0 dB y no el +6 dB que admite el registro: 0 dB es el nivel
 * fijo con el que se valido la reproduccion en la placa, de modo que el
 * 100% suena exactamente igual que antes de que el volumen fuera
 * regulable, y los +6 dB de margen quedan sin exponer para no acercar la
 * salida al recorte.
 * ───────────────────────────────────────────────────────────────────────── */
esp_err_t wm8731_set_output_volume(i2c_dev_t *dev, float volume)
{
    if (volume < 0.0f)   volume = 0.0f;
    if (volume > 100.0f) volume = 100.0f;

    uint8_t vol = (uint8_t)(0x30 + (uint8_t)((volume / 100.0f) * (0x79 - 0x30)));
    ESP_LOGI(TAG, "Output volume: %.1f%% → reg=0x%02x", volume, vol);

    return wm_write_reg_cached(dev, WM8731_REG_LHPOUT,
                                (uint16_t)(vol | WM8731_HPOUT_ZCEN |
                                           WM8731_HPOUT_BOTH));
}

/* ─────────────────────────────────────────────────────────────────────────
 * wm8731_set_clock
 *
 * Programa el registro de muestreo (R8) para que el núcleo del códec trabaje
 * a fs con el reloj que recibe en su terminal MCLK. En modo normal hay dos
 * relaciones base: 256*fs (SR=0000) y 384*fs (SR=0110, la que la hoja llama
 * "32 kHz" con MCLK = 12.288 MHz). CLKIDIV2 divide antes el MCLK entre 2,
 * lo que cubre también 512*fs y 768*fs.
 *
 *   MCLK/fs   CLKIDIV2  SR     R8
 *     256        0      0000   0x000
 *     384        0      0110   0x018
 *     512        1      0000   0x040   (ESP32 con APLL a 16 kHz)
 *     768        1      0110   0x058   (ESP32 con APLL a  8 kHz)
 *
 * La hoja recomienda desactivar la interfaz digital (ACTIVE=0) mientras se
 * cambia la frecuencia de muestreo, así que se hace en tres escrituras.
 * ───────────────────────────────────────────────────────────────────────── */
esp_err_t wm8731_set_clock(i2c_dev_t *dev, uint32_t mclk_hz, uint32_t fs_hz)
{
    if (fs_hz == 0) return ESP_ERR_INVALID_ARG;
    uint32_t ratio = (mclk_hz + fs_hz / 2) / fs_hz;
    uint16_t val;
    switch (ratio) {
        case 256: val = 0x000; break;
        case 384: val = 0x018; break;
        case 512: val = 0x040; break;
        case 768: val = 0x058; break;
        default:
            ESP_LOGE(TAG, "MCLK %" PRIu32 " Hz no es 256/384/512/768 x fs (%" PRIu32 " Hz)",
                     mclk_hz, fs_hz);
            return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "MCLK %" PRIu32 " Hz = %" PRIu32 " x fs (%" PRIu32 " Hz) -> R8 = 0x%03x",
             mclk_hz, ratio, fs_hz, val);
    if (val == wm_read_cached(WM8731_REG_SAMPLING)) return ESP_OK;

    esp_err_t res = ESP_OK;
    uint16_t active = wm_read_cached(WM8731_REG_ACTIVATE);
    res |= wm_write_reg_cached(dev, WM8731_REG_ACTIVATE, 0x000);
    res |= wm_write_reg_cached(dev, WM8731_REG_SAMPLING, val);
    res |= wm_write_reg_cached(dev, WM8731_REG_ACTIVATE, active);
    return res;
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
