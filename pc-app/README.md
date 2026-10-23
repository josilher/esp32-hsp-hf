# Panel de control — ESP32 Radio HF

Aplicación de PC, en Python, para controlar el ESP32 Radio HF por el mismo protocolo que ya habla el
firmware ([components/host_link](../components/host_link)): `PING`, `INFO`, PTT, COS, cambio de
velocidad del enlace CAT, y cualquier comando CAT en bruto reenviado a la radio.

Funciona igual si el puerto es el servicio "CAT Radio" emparejado por Bluetooth (SPP) que si es el
cable USB con la UART0 en modo CAT (ver el README principal del proyecto): para Windows los dos son
un puerto COM corriente, así que esta aplicación no necesita saber cuál es cuál.

## Por qué no es una página web ni usa BLE

El control de este proyecto va por Bluetooth Classic (SPP/RFCOMM), no por Bluetooth de baja energía.
La API Web Bluetooth de los navegadores solo habla BLE (GATT), así que no puede llegar a este canal;
de ahí que sea un programa de escritorio normal en vez de una web.

## Instalación

Requiere Python 3.9 o superior.

```
pip install -r requirements.txt
```

## Uso

### Interfaz gráfica

```
python gui.py
```

1. Empareja primero el ESP32 desde Configuración > Bluetooth y otros dispositivos.
2. Busca el puerto COM de salida del servicio "CAT Radio": Configuración > Bluetooth y otros
   dispositivos > Más opciones de Bluetooth > pestaña "COM Ports", o el Administrador de dispositivos.
3. Selecciónalo en el desplegable de la aplicación y pulsa "Conectar".
4. "PING" comprueba el enlace; "INFO" muestra el estado completo (versión, núcleo en el que corre cada
   tarea, enlaces activos, estadísticas del enlace CAT). El botón grande de PTT transmite mientras se
   mantiene pulsado. El indicador COS se actualiza solo cada 2 segundos. El campo de abajo envía
   cualquier texto tal cual, para comandos CAT reales de la radio.

### Consola sin interfaz gráfica

```
python cli.py COM5
```

Mismo protocolo, escribiendo los comandos a mano. Útil para probar el enlace rápido o si `tkinter` no
está disponible.

## Estructura

- `radio_link.py` — cliente del protocolo, reutilizable (clase `RadioLink`). No depende de tkinter; se
  puede importar desde cualquier script propio, por ejemplo para automatizar pruebas.
- `gui.py` — la interfaz gráfica.
- `cli.py` — la consola de texto.

## Limitación conocida

El audio de la llamada (HFP) no lo gestiona esta aplicación: lo maneja el propio Bluetooth de Windows,
igual que con unos auriculares manos libres corrientes. Esta aplicación solo habla el canal de control
(SPP "CAT Radio" o el cable), no el audio.

## Estado

Compilado y probado sin un ESP32 real delante (compilación, construcción de la ventana, y el parseo de
`INFO` contra el formato exacto que genera `host_link.c`). Falta la prueba de extremo a extremo con la
placa, pendiente para cuando esté montado el prototipo final.
