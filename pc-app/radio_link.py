"""
radio_link.py - Cliente Python del protocolo de control del ESP32 Radio HF.

Habla el mismo protocolo de texto que components/host_link/host_link.c, tanto
si el puerto serie es el servicio "CAT Radio" emparejado por Bluetooth (SPP)
como si es el cable USB con la UART0 en modo CAT (ver README.md del
firmware): para Windows los dos son un puerto COM corriente, así que esta
clase no necesita saber cuál es cuál.

Protocolo (una línea por comando o respuesta, terminada en CR o LF):
    PING            -> PONG
    PTT=1 / PTT=0   -> OK
    PTT?            -> PTT=1 / PTT=0
    COS?            -> COS=1 / COS=0
    BAUD=<n>        -> OK / ERR
    INFO            -> INFO fw=... cat_baud=... ptt=... cos=... ... links=...
    cualquier otra línea se reenvía tal cual a la radio; la respuesta es lo
    que conteste el equipo, o nada si no hay radio conectada.
"""
from __future__ import annotations

import queue
import threading
from dataclasses import dataclass, field
from typing import Optional


class RadioLinkError(Exception):
    """El enlace no está abierto, o la radio no respondió a tiempo."""


def pyserial_available() -> bool:
    """True si el paquete 'pyserial' (el módulo se llama 'serial') está
    instalado. No lo importa a nivel de módulo para que este fichero se
    pueda usar (por ejemplo, para probar parse_info) en una máquina sin
    pyserial instalado todavía."""
    try:
        import serial  # noqa: F401

        return True
    except ImportError:
        return False


def missing_pyserial_message() -> str:
    return (
        "Falta el paquete 'pyserial' en este Python.\n\n"
        "Instálalo con:\n"
        "    pip install pyserial\n"
        "o, desde la carpeta pc-app:\n"
        "    pip install -r requirements.txt"
    )


@dataclass
class RadioInfo:
    """Resultado ya parseado de una respuesta a INFO."""

    raw: str
    fields: dict = field(default_factory=dict)

    def __getitem__(self, key: str) -> Optional[str]:
        return self.fields.get(key)

    @property
    def firmware(self) -> Optional[str]:
        return self.fields.get("fw")

    @property
    def cat_baud(self) -> Optional[int]:
        v = self.fields.get("cat_baud")
        return int(v) if v is not None else None

    @property
    def ptt(self) -> Optional[bool]:
        v = self.fields.get("ptt")
        return v == "1" if v is not None else None

    @property
    def cos(self) -> Optional[bool]:
        v = self.fields.get("cos")
        return v == "1" if v is not None else None

    @property
    def cat_core(self) -> Optional[int]:
        v = self.fields.get("cat_core")
        return int(v) if v is not None else None

    @property
    def host_core(self) -> Optional[int]:
        v = self.fields.get("host_core")
        return int(v) if v is not None else None

    @property
    def main_core(self) -> Optional[int]:
        v = self.fields.get("main_core")
        return int(v) if v is not None else None

    @property
    def links(self) -> dict:
        """p. ej. {'spp': True, 'serial': False}."""
        raw = self.fields.get("links", "")
        out = {}
        for item in raw.split(","):
            if ":" in item:
                name, state = item.split(":", 1)
                out[name] = state == "1"
        return out


def parse_info(line: str) -> RadioInfo:
    """Convierte una línea 'INFO fw=1.0 cat_baud=9600 ... links=spp:1,serial:0'
    en un RadioInfo. Campos que no se conocen aquí simplemente no se usan, así
    que si el firmware añade alguno nuevo en el futuro esto no se rompe."""
    parts = line.split()
    fields = {}
    for p in parts[1:]:  # parts[0] es la palabra "INFO"
        if "=" in p:
            k, v = p.split("=", 1)
            fields[k] = v
    return RadioInfo(raw=line, fields=fields)


class RadioLink:
    """Cliente del protocolo de control sobre un puerto serie. Lee en un
    hilo propio para no bloquear la interfaz mientras se espera una
    respuesta, y para poder mostrar tráfico que llega sin haberlo pedido
    (datos de la radio difundidos a todos los enlaces)."""

    def __init__(self, port: str, baudrate: int = 115200, timeout: float = 1.5):
        # baudrate solo importa de verdad para el cable USB; en un puerto
        # COM virtual de Bluetooth SPP, Windows lo ignora.
        self.port_name = port
        self.baudrate = baudrate
        self.timeout = timeout
        self._serial = None
        self._reader_thread: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self.incoming: "queue.Queue[str]" = queue.Queue()
        self._response_queue: "queue.Queue[str]" = queue.Queue()

    # --- ciclo de vida ----------------------------------------------------

    def connect(self) -> None:
        import serial  # import perezoso: solo hace falta si se usa de verdad

        ser = serial.Serial()
        ser.port = self.port_name
        ser.baudrate = self.baudrate
        ser.timeout = 0.2
        # En esta placa DTR y RTS van al circuito de autoreset del ESP32 a
        # traves del CH340G, asi que abrir el puerto con esas lineas activas
        # (lo que hace pyserial por defecto) reinicia el dispositivo. Se
        # dejan en reposo antes de abrir.
        try:
            ser.dtr = False
            ser.rts = False
        except Exception:
            pass
        ser.open()
        self._serial = ser
        self._stop.clear()
        self._reader_thread = threading.Thread(target=self._read_loop, daemon=True)
        self._reader_thread.start()

    def close(self) -> None:
        self._stop.set()
        if self._reader_thread:
            self._reader_thread.join(timeout=1.0)
        if self._serial:
            self._serial.close()
        self._serial = None

    def __enter__(self) -> "RadioLink":
        self.connect()
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    @property
    def is_connected(self) -> bool:
        return self._serial is not None and self._serial.is_open

    # --- lectura en segundo plano ------------------------------------------

    def _read_loop(self) -> None:
        buf = b""
        while not self._stop.is_set():
            try:
                chunk = self._serial.read(256)
            except Exception:
                break
            if not chunk:
                continue
            buf += chunk
            while b"\r" in buf or b"\n" in buf:
                idx_r = buf.find(b"\r")
                idx_n = buf.find(b"\n")
                idx = min(i for i in (idx_r, idx_n) if i != -1)
                line = buf[:idx].decode("utf-8", errors="replace").strip()
                buf = buf[idx + 1:]
                if line:
                    self.incoming.put(line)
                    self._response_queue.put(line)

    # --- envío --------------------------------------------------------------

    def send_line(self, text: str) -> None:
        if not self.is_connected:
            raise RadioLinkError("El enlace no está abierto.")
        # vacía el 'buzón' de respuestas antes de mandar, para no leer por
        # error la respuesta de un comando anterior
        while not self._response_queue.empty():
            try:
                self._response_queue.get_nowait()
            except queue.Empty:
                break
        self._serial.write((text + "\r\n").encode("utf-8"))

    def wait_response(self, timeout: Optional[float] = None) -> str:
        try:
            return self._response_queue.get(timeout=timeout or self.timeout)
        except queue.Empty:
            raise RadioLinkError(f"Sin respuesta a tiempo ({timeout or self.timeout} s).")

    def command(self, text: str, timeout: Optional[float] = None) -> str:
        """Envía una línea y espera la siguiente que llegue como respuesta.
        Vale tanto para los comandos propios como para CAT reenviado."""
        self.send_line(text)
        return self.wait_response(timeout)

    # --- comandos de alto nivel ----------------------------------------------

    def ping(self, timeout: Optional[float] = None) -> bool:
        return self.command("PING", timeout) == "PONG"

    def info(self, timeout: Optional[float] = None) -> RadioInfo:
        return parse_info(self.command("INFO", timeout))

    def set_ptt(self, on: bool, timeout: Optional[float] = None) -> bool:
        return self.command(f"PTT={1 if on else 0}", timeout) == "OK"

    def get_ptt(self, timeout: Optional[float] = None) -> bool:
        return self.command("PTT?", timeout) == "PTT=1"

    def get_cos(self, timeout: Optional[float] = None) -> bool:
        return self.command("COS?", timeout) == "COS=1"

    def set_baud(self, baud: int, timeout: Optional[float] = None) -> bool:
        return self.command(f"BAUD={baud}", timeout) == "OK"

    def cat(self, command: str, timeout: Optional[float] = None) -> str:
        """Comando CAT en bruto: se reenvía tal cual a la radio."""
        return self.command(command, timeout)


def list_ports():
    """Puertos serie disponibles (COMx en Windows) con su descripción, para
    identificar el de 'CAT Radio' o el del cable."""
    from serial.tools import list_ports as _lp

    return list(_lp.comports())
