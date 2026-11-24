# ESP32 Radio HF — interfaz Bluetooth para transceptor de radioaficionado

Firmware para ESP32 que conecta un transceptor de radioaficionado con un ordenador o un teléfono **sin
cable de datos**. El ordenador ve dos dispositivos corrientes —unos auriculares manos libres y un
puerto COM— así que WSJT-X, Direwolf, fldigi o Hamlib funcionan sin modificarse y sin instalar ningún
driver.

- **Audio** por el perfil HFP Hands-Free (mSBC a 16 kHz, o CVSD a 8 kHz como respaldo). El ordenador
  actúa de *audio gateway*: el ESP32 recibe el audio de la «llamada» y lo saca hacia la radio, y envía
  hacia el ordenador el audio que entra desde la radio.
- **Control** (CAT, PTT y COS) por un protocolo de líneas de texto, disponible en dos enlaces
  equivalentes: un puerto serie Bluetooth (SPP, servicio «CAT Radio») o el puerto serie USB de la
  placa. Lo que no es un comando propio se reenvía tal cual a la radio, y lo que la radio contesta
  vuelve al ordenador.

Los dos perfiles funcionan **a la vez**: se puede transmitir audio y gobernar la radio en la misma
sesión.

Este firmware es el resultado de un Trabajo Fin de Grado en Ingeniería Informática (Universidad de
Huelva, 2026). Parte del ejemplo `hfp_hf_demo` de [BTstack](https://github.com/bluekitchen/btstack) y
del fork de [atomic14](https://github.com/atomic14/esp32-hsp-hf).

## Estado

Validado sobre la placa real:

| Prueba | Resultado |
|---|---|
| Audio bidireccional, códec mSBC a 16 kHz | Negociado y transportado sin errores de trama |
| Demodulación de FT8 de extremo a extremo | Decodifica hasta $-44$ dB de atenuación; umbral medido $-16$ dB de SNR frente a los $-21$ dB teóricos del modo, es decir, a 4 o 5 dB del límite de FT8 |
| APRS con un transceptor real (Baofeng, 144,800 MHz) | Recepción y transmisión correctas con Direwolf y PinPoint APRS |
| PTT por comando y ligado a la llamada de audio | Correcto, verificado eléctricamente |
| Enlace CAT en los dos sentidos | Correcto, sin pérdida de tramas |
| Reconexión Bluetooth sin reiniciar la placa | Correcto |

Latencia del canal de audio acotada entre 100 y 300 ms, medida incluyendo la del propio ordenador.

## Hardware

El firmware está escrito para una placa concreta, con un códec WM8731 y aislamiento galvánico hacia el
transceptor. **El diseño de la placa no forma parte de este repositorio.** Esta es la asignación de
terminales que espera el firmware; todos ellos y las velocidades del lado de la radio se cambian en
`menuconfig` sin tocar el código.

| Bloque | Componente | Terminales del ESP32 |
|---|---|---|
| Códec de audio | WM8731 (I2S esclavo; el ESP32 genera el MCLK por el APLL) | MCLK=IO0, BCLK=IO5, LRCLK=IO25, DACDAT=IO26, ADCDAT=IO35 |
| Control del códec | I2C (dirección 0x1A, CSB a masa) | SDA=IO21, SCL=IO22 |
| USB a serie | CH340G en la UART0, con reinicio automático por DTR/RTS | RXD0=IO3, TXD0=IO1 |
| CAT hacia la radio | UART2, aislada con optoacopladores PC817 | TX=IO32, RX=IO34 |
| PTT | Salida, activa a nivel bajo (PC817 y transistor) | IO33 |
| COS (detección de portadora) | Entrada (terminal solo de entrada, elevación externa) | IO39 |
| Audio hacia y desde la radio | Transformadores de aislamiento 1:1, 600 Ω | conector DE9 y mini-DIN de 6 |

> **Aviso sobre RTS y DTR.** En esta placa, las líneas RTS y DTR del CH340G van al circuito de
> reinicio automático del ESP32. Un programa de radio que use RTS o DTR del puerto CAT para accionar
> el PTT **reiniciará la placa**. Por cable, el PTT debe ir por comando (`PTT=1`) o por CAT nativo: en
> WSJT-X, fldigi o Hamlib hay que configurar el método de PTT como **CAT**, nunca RTS/DTR.

### El reloj maestro del códec

En el ESP32, cuando el I2S toma su reloj del APLL, el terminal IO0 saca la señal del APLL y no el
MCLK, y el driver programa el APLL a un múltiplo entero del MCLK de al menos dos. Al códec le llegan
512 veces la frecuencia de muestreo en un canal de 16 kHz y 768 veces en uno de 8 kHz, no las 256 del
valor inicial del registro. El firmware calcula la frecuencia real del terminal y programa en
consecuencia el registro de muestreo del WM8731 cada vez que se abre el canal. Sin esa corrección el
códec trabaja al doble de la frecuencia del canal: el audio se oye, pero un tono muestra una
distorsión periódica en el osciloscopio.

## Protocolo de control

Idéntico por Bluetooth y por cable. Cada línea termina en `\r` o `\n`. Las respuestas vuelven por el
enlace que envió el comando; lo que la radio emite por su cuenta se difunde a todos los enlaces.

| Comando | Respuesta | Efecto |
|---|---|---|
| `PING` | `PONG` | Comprobación del enlace |
| `PTT=1` / `PTT=0` | `OK` | Activa o suelta el PTT |
| `PTT?` | `PTT=1` / `PTT=0` | Estado del PTT |
| `AUTOPTT=1` / `AUTOPTT=0` / `AUTOPTT?` | `OK` / estado | Liga el PTT a la apertura del canal de audio |
| `COS?` | `COS=1` / `COS=0` | Estado de la detección de portadora |
| `BAUD=9600` | `OK` / `ERR` | Cambia la velocidad del enlace CAT en caliente |
| `INFO` | `INFO fw=... cat_core=.. host_core=.. main_core=.. links=spp:1,serial:1 ...` | Versión, núcleo de cada tarea, enlaces activos y estadísticas del enlace CAT |
| cualquier otra línea | lo que devuelva la radio | Se reenvía a la radio con su delimitador (comandos CAT) |

Detalles del reenvío a la radio:

- Si el enlace CAT está configurado con un byte terminador (por ejemplo `;` en Kenwood, Elecraft o
  Yaesu modernos), ese byte también cierra la trama, así que `FA;` se reenvía sin necesidad de CR o LF.
- Los datos binarios sin delimitador (Yaesu antiguos, Icom CI-V) se reenvían tras un breve silencio,
  configurable. El texto imprimible sigue esperando CR o LF, para poder teclear en un terminal.

## Estructura del repositorio

- `components/serial_port/` — **librería de puerto serie** genérica: una instancia por UART, con su
  tarea de recepción, ensamblado de tramas por línea, por byte terminador o por silencio, escritura
  protegida con mutex, estadísticas y anclaje opcional de la tarea a un núcleo. No sabe nada de radio
  ni de Bluetooth.
- `components/cat_bridge/` — enlace físico con la radio: una instancia de `serial_port` sobre la
  UART2, más el PTT y el COS.
- `components/host_link/` — capa común de control: registro de enlaces, búfer de línea por enlace,
  despachador de comandos y cruce seguro desde las tareas de las UART al bucle de BTstack.
  - `host_link_spp.c` — transporte Bluetooth SPP, con cola de salida y envío en `CAN_SEND_NOW`.
  - `host_link_serial.c` — transporte por cable sobre `serial_port`, con silenciado de la consola.
- `components/wm8731/` — driver I2C del códec, con sombra de registros en RAM porque el WM8731 solo
  admite escritura.
- `components/i2cdev/` — acceso I2C seguro entre tareas sobre el driver I2C maestro de ESP-IDF 5.
  Reproduce la interfaz de la librería `i2cdev` de esp-idf-lib, cuya implementación original está
  escrita para el driver antiguo.
- `components/btstack/` — port de BTstack para ESP32. `btstack_audio_esp32_v5.c` está modificado para
  inicializar el WM8731 y tomar el canal correcto del búfer I2S.
- `components/btstack_config/` — el `btstack_config.h` del proyecto.
- `main/hfp_hf_demo.c` — perfil HFP y arranque de los enlaces.
- `main/sco_demo_util.c` — codificación y decodificación SCO (mSBC, CVSD) y búferes de audio.
- `pc-app/` — aplicación de control para el ordenador, en Python: biblioteca cliente del protocolo,
  interfaz gráfica y consola. Ver su [README](pc-app/README.md).

## Compilación

Requiere ESP-IDF 5.3.x (probado con 5.3.4) y el target `esp32`.

No hace falta ninguna dependencia externa: BTstack y el acceso I2C van dentro del repositorio.

```
idf.py set-target esp32
idf.py menuconfig      # opcional, ver abajo
idf.py build
idf.py -p COMx flash monitor
```

> Si la ruta del directorio de usuario contiene caracteres no ASCII (habitual en instalaciones en
> español), la caché de objetos que ESP-IDF trae activada aborta la construcción con un error de la
> propia herramienta de caché. Se resuelve desactivándola: `IDF_CCACHE_ENABLE=0`.

### Opciones de menuconfig

**Radio Interface Configuration**

- *Bluetooth device name*: nombre que se anuncia por Bluetooth y nombre del servicio HFP.
- *Audio Gateway address for the test console*: dirección del teléfono u ordenador al que se conecta
  la consola de pruebas con la tecla `a`. Las conexiones entrantes se aceptan de cualquier dispositivo.
- *RFCOMM channel* y *SPP service name* del puerto serie Bluetooth.
- *UART0 (USB) mode*:
  - **Console** (predeterminado): registro, `printf` y la consola de teclas de BTstack. Para
    desarrollar.
  - **CAT**: la UART0 pasa a ser un enlace de control por cable con el mismo protocolo que SPP. El
    registro y los `printf` se silencian para no mezclarse con el protocolo.
- *Baud rate of the wired host link* (solo en modo CAT), 115200 de serie.

**Component config → CAT bridge (radio link)**: UART, terminales, velocidad, formato de trama de lo
que llega de la radio (en bruto, por líneas o por byte terminador) y polaridad del PTT y del COS.

**Component config → Host link**: tiempo de silencio tras el que se reenvían los datos binarios sin
delimitador.

**Example Board Configuration**: debe estar seleccionado `ESP32 custom board` para que se use el
WM8731.

La configuración que trae el repositorio en `sdkconfig.defaults` es la validada.

## Uso

### Por Bluetooth

1. Emparejar el ESP32 desde el ordenador o el teléfono. Aparecen un dispositivo de audio manos libres
   y un puerto COM virtual (SPP «CAT Radio»).
2. **Desactivar las mejoras de señal del dispositivo de audio.** La supresión de ruido que el sistema
   operativo aplica a los dispositivos de comunicaciones silencia las señales débiles de modos
   digitales. En Windows: Panel de control → Sonido → el dispositivo → Propiedades → Opciones
   avanzadas.
3. En WSJT-X, Hamlib, Direwolf, etc., seleccionar el puerto COM virtual a la velocidad de la radio
   para el CAT, y el dispositivo manos libres como entrada y salida de audio.

### Por cable (modo CAT)

1. Compilar con *UART0 mode = CAT* y programar la placa.
2. Abrir el COM del USB a la velocidad configurada (115200 de serie). No hace falta emparejar.
3. Probar con un terminal: `PING` debe contestar `PONG` e `INFO` muestra el estado. `PTT=1` debe bajar
   IO33 a 0 V.
4. Para usar un programa de radio, apuntarlo a ese COM y poner el PTT en modo CAT (ver el aviso sobre
   RTS y DTR). El Bluetooth sigue funcionando a la vez.

Para volver a ver el registro, recompilar con *UART0 mode = Console*.

## Reparto de tareas entre núcleos (opcional)

El ESP32 tiene dos núcleos, pero el proyecto se compila en modo de un solo núcleo
(`CONFIG_FREERTOS_UNICORE=y`), heredado del ejemplo original. Se puede anclar al segundo núcleo las
dos tareas que leen las UART (la de `cat_bridge` hacia la radio y la de `host_link_serial` hacia el
ordenador por cable), para aislarlas del hilo de BTstack.

**Qué se gana y qué no.** El beneficio no es caudal: el enlace serie lo limita la velocidad de la
línea, no la CPU. Es aislamiento temporal: hoy esas dos tareas comparten núcleo con el hilo de
BTstack, de modo que un pico de trabajo en ellas puede restarle tiempo justo cuando toca entregar un
paquete de audio SCO.

**Qué queda fuera siempre.** El hilo de BTstack, cualquier llamada a su API (`hci_*`, `rfcomm_*`,
`sdp_*`, `hfp_*`) y la codificación y decodificación del audio permanecen en el núcleo 0 sin
excepción. BTstack exige que toda su API se llame desde un único hilo lógico, y el envío de los
paquetes SCO usa funciones de HCI que no son seguras entre hilos.

**Hacen falta dos interruptores independientes, en este orden:**

1. **Sistema:** `idf.py menuconfig` → `Component config` → `FreeRTOS` → desactivar *Run FreeRTOS only
   on first core*. Esto reactiva el núcleo 1 a nivel de hardware; con `CONFIG_FREERTOS_UNICORE=y` ese
   núcleo tiene el reloj apagado desde el arranque, no solo está fuera del planificador.
2. **Aplicación:** `idf.py menuconfig` → `Serial port task placement (advanced)` → activar
   `SERIAL_PORT_DUAL_CORE` y elegir qué enlaces van al núcleo 1.

Si solo se activa el paso 2, el firmware detecta en compilación que sigue en modo de un solo núcleo,
ignora la afinidad pedida y lo avisa en el registro de arranque. Pedir el núcleo 1 al planificador en
ese modo haría abortar el firmware, así que el código nunca llega a intentarlo.

**Resultado de la validación.** Se ha probado con audio real: la orden `INFO` devuelve la tarea CAT en
el núcleo 1 y el hilo de BTstack en el 0, y el canal se establece en mSBC sin errores de CRC. Tiene
dos costes: el cúmulo disponible al arranque baja de unos 238 a unos 190 KiB, porque los 32 KiB de
memoria de instrucciones que el modo de un núcleo cede al cúmulo pasan a ser la caché del segundo
procesador, y la temporización del audio cambia de forma medible. **No aporta un beneficio
apreciable**, así que la configuración que se entrega sigue siendo la de un solo núcleo.

## Consola de pruebas (solo en modo Console)

Con el monitor serie abierto, cualquier tecla no reconocida muestra la ayuda. Las más útiles:

- `a` / `A` conectar y desconectar el enlace HFP con el *audio gateway* configurado.
- `b` / `B` abrir y cerrar la conexión de audio.
- `o`…`P` volumen del altavoz, `q`…`S` ganancia del micrófono.

## Créditos y licencia

- El código propio de este proyecto (`components/serial_port`, `components/cat_bridge`,
  `components/host_link`, `components/wm8731`, `pc-app` y las modificaciones de `main/`) se publica
  bajo la licencia MIT. Ver [LICENSE](LICENSE).
- `components/btstack/` es [BTstack](https://github.com/bluekitchen/btstack), de BlueKitchen GmbH, con
  su propia licencia. Es de tipo BSD de tres cláusulas **con una cuarta cláusula que limita el uso a
  fines no comerciales**; un producto comercial requiere una licencia de BlueKitchen. Esa licencia
  acompaña a cada fichero y prevalece sobre la de este repositorio en todo lo que afecta a BTstack.
- `components/i2cdev/` reproduce la interfaz de la librería `i2cdev` de
  [esp-idf-lib](https://github.com/esp-idf-lib/i2cdev), de Ruslan V. Uss, con licencia MIT. La
  implementación sobre el driver I2C maestro de ESP-IDF 5 es propia; ver
  [components/i2cdev/LICENSE](components/i2cdev/LICENSE).
- El punto de partida fue el ejemplo `hfp_hf_demo` del port de BTstack para ESP32 y el fork de
  [atomic14](https://github.com/atomic14/esp32-hsp-hf).
