/*
 * wm8731.h
 *
 * Driver para el códec de audio WM8731S (Wolfson/Cirrus Logic)
 * Basado en comunicación I2C con i2cdev (esp-idf-lib, adaptada al driver
 * I2C maestro de ESP-IDF 5)
 *
 * Configuración hardware:
 *   WM8731S        ESP32
 *   ADCDAT    →    IO35  (I2S data in,  datos ADC → ESP32)
 *   DACDAT    →    IO26  (I2S data out, datos DAC ← ESP32)
 *   ADCLRC    →    IO25  (LRCLK compartido ADC+DAC)
 *   DACLRC    →    IO25  (LRCLK compartido ADC+DAC)
 *   BCLK      →    IO5   (Bit Clock, generado por ESP32)
 *   MCLK      →    IO0   (Master Clock = 256 × fs, generado por el ESP32)
 *   IIC_DAT   →    IO21  (I2C SDA)
 *   IIC_SCK   →    IO22  (I2C SCL)
 *
 * Modo:             WM8731S Slave  (ESP32 genera MCLK, BCLK y LRCLK)
 * Frecuencia:       la del canal de voz HFP, sin conversión en el firmware:
 *                     16 kHz con mSBC (MCLK 4.096 MHz)
 *                      8 kHz con CVSD (MCLK 2.048 MHz)
 * MCLK:             256 × fs (I2S_MCLK_MULTIPLE_256 en btstack_audio_esp32_v5.c).
 *                   El registro de muestreo va en modo normal (SR=0000, BOSR=0),
 *                   en el que fs = MCLK/256. La hoja de datos llama a ese modo
 *                   "48 kHz" porque supone MCLK = 12.288 MHz.
 *
 * Nota sobre registros WM8731:
 *   Registros de 9 bits de dato. Se transmiten en 2 bytes:
 *     Byte 1: [ reg[6:0] | dato[8] ]
 *     Byte 2: [ dato[7:0] ]
 *   El chip es write-only por I2C → se mantiene sombra de registros en RAM.
 */

#ifndef COMPONENTS_WM8731_H_
#define COMPONENTS_WM8731_H_

#include "i2cdev.h"
#include "driver/gpio.h"
#include <stdbool.h>

/* ─── Pinout I2C ────────────────────────────────────────────────────────── */
/* Los pines I2S (MCLK, BCLK, LRCLK, DACDAT, ADCDAT) los gestiona          */
/* btstack_audio_esp32_v5.c directamente — no se declaran aquí.             */
#define WM8731_PIN_SDA      GPIO_NUM_21
#define WM8731_PIN_SCL      GPIO_NUM_22

/* ─── Dirección I2C ─────────────────────────────────────────────────────── */
/* Medir CSB: a GND → 0x1A  |  a VDD → 0x1B                               */
#define WM8731_ADDR_CSB_LOW   0x1A
#define WM8731_ADDR_CSB_HIGH  0x1B

/* ─── Registros ─────────────────────────────────────────────────────────── */
#define WM8731_REG_LLINEIN      0x00
#define WM8731_REG_RLINEIN      0x01
#define WM8731_REG_LHPOUT       0x02
#define WM8731_REG_RHPOUT       0x03
#define WM8731_REG_ANAPATH      0x04
#define WM8731_REG_DIGPATH      0x05
#define WM8731_REG_POWERDOWN    0x06
#define WM8731_REG_DIGIF        0x07
#define WM8731_REG_SAMPLING     0x08
#define WM8731_REG_ACTIVATE     0x09
#define WM8731_REG_RESET        0x0F

/* ─── Bits LLINEIN / RLINEIN ─────────────────────────────────────────────── */
/* LINVOL [4:0]: 0x00=-34.5dB  0x17=0dB  0x1F=+12dB                        */
#define WM8731_LINEIN_MUTE   (1<<7)
#define WM8731_LINEIN_BOTH   (1<<8)  /* Escribe izq+dch en una operación     */

/* ─── Bits LHPOUT / RHPOUT ───────────────────────────────────────────────── */
/* HPVOL [6:0]: 0x30=silencio(-73dB)  0x79=0dB  0x7F=+6dB                  */
#define WM8731_HPOUT_ZCEN    (1<<7)  /* Zero Cross Enable                    */
#define WM8731_HPOUT_BOTH    (1<<8)  /* Escribe izq+dch en una operación     */

/* ─── Bits ANAPATH ───────────────────────────────────────────────────────── */
#define WM8731_ANAPATH_MICBOOST  (1<<0)
#define WM8731_ANAPATH_MUTEMIC   (1<<1)
#define WM8731_ANAPATH_INSEL     (1<<2)  /* 0=Line In, 1=Mic                 */
#define WM8731_ANAPATH_BYPASS    (1<<3)  /* Line In directo a salida          */
#define WM8731_ANAPATH_DACSEL    (1<<4)  /* DAC → mixer de salida            */
#define WM8731_ANAPATH_SIDETONE  (1<<5)

/* ─── Bits DIGPATH ───────────────────────────────────────────────────────── */
#define WM8731_DIGPATH_ADCHPD      (1<<0)  /* Deshabilitar HPF del ADC       */
#define WM8731_DIGPATH_DEEMP_NONE  0x00
#define WM8731_DIGPATH_DEEMP_32K   (1<<1)
#define WM8731_DIGPATH_DEEMP_44K1  (2<<1)
#define WM8731_DIGPATH_DEEMP_48K   (3<<1)
#define WM8731_DIGPATH_DACMU       (1<<3)  /* Mute suave DAC                 */

/* ─── Bits POWERDOWN ─────────────────────────────────────────────────────── */
#define WM8731_PWR_LINEINPD  (1<<0)
#define WM8731_PWR_MICPD     (1<<1)
#define WM8731_PWR_ADCPD     (1<<2)
#define WM8731_PWR_DACPD     (1<<3)
#define WM8731_PWR_OUTPD     (1<<4)
#define WM8731_PWR_OSCPD     (1<<5)
#define WM8731_PWR_CLKOUTPD  (1<<6)
#define WM8731_PWR_POWEROFF  (1<<7)

/* ─── Bits DIGIF ─────────────────────────────────────────────────────────── */
/* FORMAT [1:0]:  00=MSB-right  01=MSB-left  10=I2S  11=DSP                 */
/* IWL    [3:2]:  00=16b  01=20b  10=24b  11=32b                            */
/* LRP    [4]:    polaridad LRCLK                                            */
/* LRSWAP [5]:    intercambiar canales                                       */
/* MS     [6]:    0=Slave  1=Master                                          */
/* BCLKINV[7]:    invertir BCLK                                              */
#define WM8731_FORMAT_I2S   0x02  /* Formato Philips I2S (FORMAT[1:0]=10)   */
#define WM8731_BITS_16      0x00  /* Word length 16 bits  (IWL[1:0]=00)     */

/* ─── Bits SAMPLING ─────────────────────────────────────────────────────── */
/* USB/NORM [0]: 0=Normal mode (usamos este)                                 */
/* BOSR     [1]: 0 para 48kHz en Normal mode                                 */
/* SR      [5:2]: 0000=48kHz en Normal mode                                  */
/* CLKIDIV2[6], CLKODIV2[7]: divisores adicionales, 0 para 48kHz            */

/* ─── API pública ───────────────────────────────────────────────────────── */

/**
 * @brief Inicializa el descriptor I2C del WM8731.
 *
 * Uso típico:
 *   i2c_dev_t codec;
 *   wm8731_init_desc(&codec, I2C_NUM_0,
 *                    WM8731_PIN_SDA, WM8731_PIN_SCL,
 *                    WM8731_ADDR_CSB_LOW);
 */
esp_err_t wm8731_init_desc(i2c_dev_t *dev, i2c_port_t port,
                            gpio_num_t sda_gpio, gpio_num_t scl_gpio,
                            uint8_t addr);

/** @brief Libera el descriptor I2C. */
esp_err_t wm8731_free_desc(i2c_dev_t *dev);

/**
 * @brief Inicializa el WM8731S vía I2C.
 *
 *   Ruta de señal:
 *     ADC ← Line In (LINPUT/RINPUT)
 *     DAC → Line Out
 *     WM8731 en modo Slave — el I2S lo gestiona btstack_audio_esp32_v5.c
 */
esp_err_t wm8731_init(i2c_dev_t *dev);

/**
 * @brief Volumen entrada Line In (ambos canales).
 * @param volume  0.0 = -34.5 dB  …  100.0 = +12 dB
 */
esp_err_t wm8731_set_input_volume(i2c_dev_t *dev, float volume);

/**
 * @brief Volumen salida Line Out / HP (ambos canales).
 * @param volume  0.0 = silencio  …  100.0 = +6 dB
 */
esp_err_t wm8731_set_output_volume(i2c_dev_t *dev, float volume);

/** @brief Mute suave del DAC. */
esp_err_t wm8731_set_mute(i2c_dev_t *dev, bool enable);

/** @brief Bypass analógico Line In → Line Out (sin pasar por ADC/DAC). */
esp_err_t wm8731_set_bypass(i2c_dev_t *dev, bool enable);

#endif /* COMPONENTS_WM8731_H_ */
