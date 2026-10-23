"""
cli.py - Consola mínima para el protocolo de control del ESP32 Radio HF, sin
interfaz gráfica. Útil para probar el enlace rápido desde una terminal o si
no hay tkinter disponible.

Uso:
    python cli.py COM5

(el puerto: Configuración > Bluetooth y otros dispositivos > Más opciones de
Bluetooth > pestaña "COM Ports", o el Administrador de dispositivos).

Los comandos son los mismos que entiende el firmware (PING, INFO, PTT=1,
PTT=0, PTT?, COS?, BAUD=<n>); cualquier otra línea se reenvía tal cual a la
radio. Ctrl+C para salir.
"""
from __future__ import annotations

import queue
import sys
import threading

from radio_link import RadioLink, missing_pyserial_message, pyserial_available


def _print_incoming(link: RadioLink, stop: threading.Event) -> None:
    while not stop.is_set():
        try:
            line = link.incoming.get(timeout=0.2)
        except queue.Empty:
            continue
        print(f"<< {line}")


def main() -> None:
    if not pyserial_available():
        print(missing_pyserial_message())
        sys.exit(1)
    if len(sys.argv) != 2:
        print("Uso: python cli.py <PUERTO>   (p.ej. python cli.py COM5)")
        sys.exit(1)

    port = sys.argv[1]
    link = RadioLink(port, baudrate=115200, timeout=2.0)
    try:
        link.connect()
    except Exception as e:
        print(f"No se pudo abrir {port}: {e}")
        sys.exit(1)

    print(f"Conectado a {port}. Escribe un comando y pulsa Enter. Ctrl+C para salir.")
    stop = threading.Event()
    printer = threading.Thread(target=_print_incoming, args=(link, stop), daemon=True)
    printer.start()

    try:
        while True:
            line = input("> ").strip()
            if not line:
                continue
            link.send_line(line)
    except (KeyboardInterrupt, EOFError):
        pass
    finally:
        stop.set()
        link.close()
        print("\nDesconectado.")


if __name__ == "__main__":
    main()
