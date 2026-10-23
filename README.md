# ESP32 Radio HF — interfaz Bluetooth para transceptor de radioaficionado

Firmware para ESP32 que expone un transceptor de radioaficionado como dispositivo Bluetooth y,
opcionalmente, por cable USB:

- **Audio** mediante el perfil HFP Hands-Free (mSBC 16 kHz o CVSD 8 kHz). El PC o el móvil actúa como
  Audio Gateway; el ESP32 recibe el audio de la "llamada" y lo saca hacia la radio, y envía hacia el AG
  el audio que entra desde la radio.
- **Control** (CAT, PTT, COS) mediante un protocolo de líneas de texto que se puede hablar por dos
  enlaces equivalentes: un puerto serie Bluetooth (SPP, servicio "CAT Radio") o el puerto serie USB
  de la placa. Lo que no es un comando propio se reenvía tal cual a la radio, y lo que la radio contesta
  vuelve al host.

Basado en el ejemplo `hfp_hf_demo` de [BTstack](https://github.com/bluekitchen/btstack) y en el
fork de [atomic14](https://github.com/atomic14/esp32-hsp-hf).

## Hardware (placa bluesp32)

| Bloque | Componente | Pines ESP32 |
|---|---|---|
| Códec de audio | WM8731 (I2S esclavo, ESP32 genera MCLK por APLL) | MCLK=IO0, BCLK=IO5, LRCLK=IO25, DACDAT=IO26, ADCDAT=IO35 |
| Control del códec | I2C (dirección 0x1A, CSB a GND) | SDA=IO21, SCL=IO22 |
| USB a serie | CH340G en la UART0 (con autoreset por DTR/RTS) | RXD0=IO3, TXD0=IO1 |
| CAT hacia la radio | UART2, aislada por optoacopladores PC817 | TX=IO32, RX=IO34 |
| PTT | Salida, activa a nivel bajo (PC817 → transistor) | IO33 |
| COS / squelch | Entrada (pin solo de entrada, pull-up externo) | IO39 |
| Audio hacia/desde la radio | Transformadores de aislamiento | conector DE9 y mini-DIN 6 |

Todos los pines y velocidades del lado radio se cambian en `menuconfig` sin tocar código.

> **Aviso sobre RTS y DTR.** En esta placa las líneas RTS y DTR del CH340G van al circuito de autoreset
> del ESP32. Un programa de radio que use RTS o DTR del puerto CAT para accionar el PTT reiniciará la
> placa. Por cable, el PTT debe ir por comando (`PTT=1`) o por CAT nativo; en WSJT-X, fldigi o Hamlib
> configura el método de PTT como **CAT**, nunca RTS/DTR.

## Protocolo de control

Igual por Bluetooth y por cable. Cada línea termina en `\r` o `\n`. Las respuestas vuelven por el
enlace que envió el comando; lo que la radio emite por su cuenta se difunde a todos los enlaces.

| Comando | Respuesta | Efecto |
|---|---|---|
| `PING` | `PONG` | Comprobación de enlace |
| `PTT=1` / `PTT=0` | `OK` | Activa / suelta el PTT |
| `PTT?` | `PTT=1` / `PTT=0` | Estado del PTT |
| `COS?` | `COS=1` / `COS=0` | Estado de la detección de portadora |
| `BAUD=9600` | `OK` / `ERR` | Cambia la velocidad del enlace CAT en caliente |
| `INFO` | `INFO fw=... cat_core=.. host_core=.. main_core=.. links=spp:1,serial:1 ...` | Versión, núcleo de cada tarea, enlaces y estadísticas del enlace CAT |
| cualquier otra línea | lo que devuelva la radio | Se reenvía a la radio con su delimitador (comandos CAT) |

Detalles del reenvío a la radio:

- Si el enlace CAT está configurado con un byte terminador (por ejemplo `;` para Kenwood, Elecraft o
  Yaesu modernos), ese byte también cierra trama, así que `FA;` se reenvía sin necesidad de CR/LF.
- Datos binarios sin delimitador (Yaesu antiguos, Icom CI-V) se reenvían tras un breve silencio,
  configurable. El texto imprimible sigue esperando CR/LF, para poder teclear en un terminal.

## Estructura

- `components/serial_port/` — **librería de puerto serie** genérica: una instancia por UART, tarea de
  recepción propia, ensamblado de tramas por línea, por byte terminador o por silencio, escritura con
  mutex, estadísticas y anclaje opcional de la tarea a un núcleo. No sabe nada de radio ni Bluetooth.
- `components/cat_bridge/` — enlace físico con la radio: una instancia de serial_port (UART2) más PTT
  y COS. Configurable en menuconfig.
- `components/host_link/` — capa común de control: registro de enlaces, buffer de línea por enlace,
  despachador de comandos y cruce seguro desde las tareas de las UART al run loop de BTstack.
  - `host_link_spp.c` — transporte Bluetooth SPP con cola de salida y envío en `CAN_SEND_NOW`.
  - `host_link_serial.c` — transporte por cable sobre serial_port, con silenciado de la consola.
- `main/hfp_hf_demo.c` — perfil HFP y arranque de los enlaces.
- `main/sco_demo_util.c` — codificación/decodificación SCO (mSBC, CVSD) y buffers de audio.
- `components/wm8731/`, `components/i2cdev/` — driver I2C del códec.
- `components/btstack/` — port de BTstack para ESP32. `btstack_audio_esp32_v5.c` está modificado para
  inicializar el WM8731 y tomar el canal correcto del buffer I2S.
- `components/btstack_config/` — `btstack_config.h` del proyecto.
- `hardware/` — esquema KiCad de la placa.

## Compilación

Requiere ESP-IDF 5.3.x (probado con 5.3.4) y target `esp32`.

```
idf.py set-target esp32
idf.py menuconfig      # opcional, ver abajo
idf.py build
idf.py -p COMx flash monitor
```

### Opciones en menuconfig

**Radio Interface Configuration**

- *Bluetooth device name*: nombre anunciado por Bluetooth y del servicio HFP.
- *Audio Gateway address for the test console*: dirección del móvil o PC al que se conecta la consola
  de pruebas con la tecla `a`. Las conexiones entrantes se aceptan de cualquier dispositivo.
- *RFCOMM channel* y *SPP service name* del puerto serie Bluetooth.
- *UART0 (USB) mode*:
  - **Console** (por defecto): logs, printf y la consola de teclas de BTstack. Para desarrollar.
  - **CAT**: la UART0 pasa a ser un enlace de control por cable con el mismo protocolo que SPP.
    Los logs y printf se silencian para no mezclarse con el protocolo.
- *Baud rate of the wired host link* (solo en modo CAT), 115200 por defecto.

**Component config → CAT bridge (radio link)**: UART, pines, velocidad, framing de lo que llega de la
radio (raw, líneas o byte terminador) y polaridad de PTT y COS.

**Component config → Host link**: tiempo de silencio tras el que se reenvían datos binarios sin
delimitador.

**Example Board Configuration**: debe estar seleccionado `ESP32 custom board` para que se use el WM8731.

## Uso

### Por Bluetooth

1. Emparejar el ESP32 desde el PC o el móvil. Aparecen un dispositivo de audio manos libres y un
   puerto COM virtual (SPP "CAT Radio").
2. En Hamlib, WSJT-X, etc., seleccionar el COM virtual con la velocidad de la radio para CAT y el
   dispositivo manos libres como entrada y salida de audio.

### Por cable (modo CAT)

1. Compilar con *UART0 mode = CAT* y flashear.
2. Abrir el COM del USB a la velocidad configurada (115200 por defecto). No hace falta emparejar.
3. Probar con un terminal: `PING` debe contestar `PONG`; `INFO` muestra el estado. `PTT=1` debe bajar
   IO33 a 0 V.
4. Para usar un programa de radio, apuntarlo a ese COM y poner el PTT en modo CAT (ver el aviso sobre
   RTS/DTR). El Bluetooth sigue funcionando a la vez.

Para volver a ver los logs, recompilar con *UART0 mode = Console*.

## Reparto de tareas entre núcleos (opcional, experimental)

El ESP32 tiene dos núcleos, pero el proyecto compila hoy en modo de un solo núcleo
(`CONFIG_FREERTOS_UNICORE=y`), heredado del ejemplo original de atomic14. Existe la posibilidad de
anclar al segundo núcleo las dos tareas que leen las UART (la de `cat_bridge` hacia la radio y la de
`host_link_serial` hacia el PC por cable), para aislarlas del hilo de BTstack.

**Qué gana con esto y qué no.** El beneficio no es velocidad: el enlace serie va limitado por baudios,
no por CPU. Es aislamiento de *jitter*: hoy esas dos tareas comparten núcleo con el hilo de BTstack, así
que un pico de trabajo en ellas puede robarle tiempo de CPU justo cuando toca enviar un paquete de audio
SCO. Moviéndolas a otro núcleo, ese trabajo deja de competir con el timing del audio.

**Qué queda fuera, siempre.** El hilo de BTstack (`app_main`, el run loop), cualquier llamada a su API
(`hci_*`, `rfcomm_*`, `sdp_*`, `hfp_*`) y la codificación/decodificación de audio (mSBC, CVSD en
`sco_demo_util.c`) permanecen en el núcleo 0 sin excepción. BTstack exige que toda su API se llame desde
un único hilo lógico, y el envío de los paquetes SCO usa funciones de HCI que no son seguras entre
hilos: intentar moverlas sería un cambio de mucho riesgo para un beneficio de CPU muy pequeño (los
frames de mSBC/CVSD son de unos 60 bytes cada 7,5 ms).

**Hacen falta dos interruptores independientes, en este orden:**

1. **Sistema (obligatorio primero):** `idf.py menuconfig` → `Component config` → `FreeRTOS` → desactivar
   *"Run FreeRTOS only on first core"*. Esto reactiva el núcleo 1 a nivel de hardware (con
   `CONFIG_FREERTOS_UNICORE=y` el núcleo 1 tiene el reloj apagado desde el arranque, no solo está fuera
   del planificador). **Es el único cambio de esta lista sin verificar en hardware con audio HFP real**;
   `sdkconfig.defaults` se deja deliberadamente en modo unicore hasta que se compruebe en la placa.
2. **Aplicación:** `idf.py menuconfig` → `Serial port task placement (advanced)` → activar
   `SERIAL_PORT_DUAL_CORE` y elegir qué enlaces van a core 1 (por defecto, los dos si se activa).

Si solo se activa el paso 2 sin el 1, no pasa nada malo: el firmware detecta en compilación que sigue en
modo de un solo núcleo, ignora la afinidad pedida y lo avisa por log en el arranque. Pedir el núcleo 1
directamente al planificador de FreeRTOS estando en modo unicore haría abortar el firmware, así que el
código nunca llega a intentarlo en ese caso.

| `FREERTOS_UNICORE` | `SERIAL_PORT_DUAL_CORE` | Resultado |
|---|---|---|
| activado (por defecto) | no | Comportamiento actual, sin cambios. |
| activado | sí | Aviso en el log de arranque, tareas siguen sin anclar. Configuración inconsistente pero segura. |
| desactivado | no | Sistema en dual-core pero sin usarlo: BTstack sigue en core 0, las tareas UART sin afinidad explícita. |
| desactivado | sí | Estado buscado: BTstack en core 0, tareas UART en core 1. |

**Cómo comprobarlo.** El comando `INFO` (ver tabla del protocolo) incluye `cat_core`, `host_core` y
`main_core`. Con el reparto activo deben valer `1`, `1` y `0` respectivamente; `main_core` sirve de
control de que el hilo de BTstack nunca se movió.

## Consola de pruebas (solo en modo Console)

Con el monitor serie abierto, cualquier tecla no reconocida muestra la ayuda. Las más útiles:

- `a` / `A` conectar / desconectar el enlace HFP con el AG configurado.
- `b` / `B` abrir / cerrar la conexión de audio.
- `o`…`P` volumen de altavoz, `q`…`S` ganancia de micrófono.
