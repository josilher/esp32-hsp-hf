"""
gui.py - Panel de control de escritorio para el ESP32 Radio HF.

Habla components/host_link/host_link.c por Bluetooth (servicio SPP "CAT
Radio") o por el cable USB con la UART0 en modo CAT; en ambos casos es un
puerto COM normal para Windows.

Uso:
    pip install -r requirements.txt
    python gui.py

Empareja primero el dispositivo desde Configuración > Bluetooth, busca el
puerto COM de salida del servicio "CAT Radio" (Más opciones de Bluetooth >
pestaña "COM Ports", o el Administrador de dispositivos) y selecciónalo
aquí.
"""
from __future__ import annotations

import queue
import sys
import threading

try:
    import tkinter as tk
    from tkinter import messagebox, scrolledtext, ttk
except ImportError:
    print(
        "No se encontró el módulo 'tkinter' en este Python.\n"
        "No es un paquete de pip: viene incluido con el propio intérprete.\n"
        "  - Windows: reinstala Python desde python.org marcando la casilla\n"
        "    'tcl/tk and IDLE' en el instalador.\n"
        "  - Debian/Ubuntu: sudo apt install python3-tk\n"
        "Mientras tanto puedes usar la consola sin interfaz gráfica:\n"
        "    python cli.py <PUERTO>"
    )
    sys.exit(1)

from radio_link import RadioLink, list_ports, missing_pyserial_message, pyserial_available


class RadioApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("ESP32 Radio HF — panel de control")
        self.geometry("680x560")
        self.minsize(560, 440)

        self.link: RadioLink | None = None
        self._cos_job = None
        self._last_cos: bool | None = None
        # Los hilos de trabajo NUNCA tocan tkinter (ni self.after): dejan lo
        # que haya que hacer en esta cola y el hilo principal la vacía en
        # _pump(). Llamar a tkinter desde otro hilo da errores aleatorios
        # del tipo "main thread is not in main loop".
        self._ui_queue: "queue.Queue[tuple]" = queue.Queue()

        self._build_ui()
        self._refresh_ports()
        self._pump()

    # ------------------------------------------------------------------
    def _build_ui(self):
        pad = {"padx": 6, "pady": 4}

        top = ttk.Frame(self)
        top.pack(fill="x", **pad)

        ttk.Label(top, text="Puerto:").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(top, textvariable=self.port_var, width=32, state="readonly")
        self.port_combo.pack(side="left", padx=4)
        ttk.Button(top, text="Actualizar", command=self._refresh_ports).pack(side="left")

        self.connect_btn = ttk.Button(top, text="Conectar", command=self._toggle_connect)
        self.connect_btn.pack(side="left", padx=8)

        self.status_var = tk.StringVar(value="Desconectado")
        self.status_label = ttk.Label(top, textvariable=self.status_var, foreground="red")
        self.status_label.pack(side="left", padx=8)

        # --- comandos rápidos --------------------------------------------
        cmds = ttk.LabelFrame(self, text="Comandos")
        cmds.pack(fill="x", **pad)

        ttk.Button(cmds, text="PING", command=self._do_ping).grid(row=0, column=0, padx=4, pady=6)
        ttk.Button(cmds, text="INFO", command=self._do_info).grid(row=0, column=1, padx=4, pady=6)

        ttk.Label(cmds, text="Baudios CAT:").grid(row=0, column=2, padx=(20, 4))
        self.baud_var = tk.StringVar(value="9600")
        ttk.Entry(cmds, textvariable=self.baud_var, width=8).grid(row=0, column=3)
        ttk.Button(cmds, text="Aplicar", command=self._do_set_baud).grid(row=0, column=4, padx=4)

        # --- PTT / COS ------------------------------------------------------
        ptt_frame = ttk.LabelFrame(self, text="PTT / COS")
        ptt_frame.pack(fill="x", **pad)

        self.ptt_btn = tk.Button(
            ptt_frame,
            text="MANTENER PULSADO PARA TRANSMITIR",
            width=34,
            height=2,
            bg="#c0392b",
            fg="white",
            state="disabled",
        )
        self.ptt_btn.grid(row=0, column=0, padx=8, pady=8)
        self.ptt_btn.bind("<ButtonPress-1>", lambda e: self._ptt(True))
        self.ptt_btn.bind("<ButtonRelease-1>", lambda e: self._ptt(False))

        self.cos_var = tk.StringVar(value="COS: —")
        self.cos_label = ttk.Label(ptt_frame, textvariable=self.cos_var, font=("Segoe UI", 12, "bold"))
        self.cos_label.grid(row=0, column=1, padx=20)

        # --- comando CAT libre -----------------------------------------------
        cat_frame = ttk.LabelFrame(self, text="Comando CAT / hacia la radio")
        cat_frame.pack(fill="x", **pad)
        self.cat_var = tk.StringVar()
        cat_entry = ttk.Entry(cat_frame, textvariable=self.cat_var)
        cat_entry.pack(side="left", fill="x", expand=True, padx=4, pady=6)
        cat_entry.bind("<Return>", lambda e: self._do_cat())
        ttk.Button(cat_frame, text="Enviar", command=self._do_cat).pack(side="left", padx=4)

        # --- registro -------------------------------------------------------
        log_frame = ttk.LabelFrame(self, text="Registro")
        log_frame.pack(fill="both", expand=True, **pad)
        self.log = scrolledtext.ScrolledText(log_frame, height=16, state="disabled", font=("Consolas", 9))
        self.log.pack(fill="both", expand=True)

        self.protocol("WM_DELETE_WINDOW", self._on_close)

    # ------------------------------------------------------------------
    def _log(self, text: str):
        self.log.configure(state="normal")
        self.log.insert("end", text + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def _refresh_ports(self):
        try:
            ports = list_ports()
        except Exception as e:
            self._log(f"[error listando puertos: {e}]")
            ports = []
        values = [f"{p.device} — {p.description}" for p in ports]
        self.port_combo["values"] = values
        if values and not self.port_var.get():
            self.port_combo.current(0)

    def _selected_port(self) -> str | None:
        val = self.port_var.get()
        if not val:
            return None
        return val.split(" — ")[0].strip()

    # --- conexión -------------------------------------------------------
    def _toggle_connect(self):
        if self.link and self.link.is_connected:
            self._disconnect()
        else:
            self._connect()

    def _connect(self):
        port = self._selected_port()
        if not port:
            messagebox.showwarning("Puerto", "Selecciona un puerto COM primero.")
            return
        try:
            self.link = RadioLink(port, baudrate=115200, timeout=1.5)
            self.link.connect()
        except Exception as e:
            messagebox.showerror("Conexión", f"No se pudo abrir {port}:\n{e}")
            self.link = None
            return
        self.status_var.set(f"Conectado ({port})")
        self.status_label.configure(foreground="green")
        self.connect_btn.configure(text="Desconectar")
        self.ptt_btn.configure(state="normal")
        self._log(f"[conectado a {port}]")
        self._last_cos = None
        self._poll_cos()

    def _disconnect(self):
        if self._cos_job:
            self.after_cancel(self._cos_job)
            self._cos_job = None
        if self.link:
            self.link.close()
        self.link = None
        self.status_var.set("Desconectado")
        self.status_label.configure(foreground="red")
        self.connect_btn.configure(text="Conectar")
        self.ptt_btn.configure(state="disabled")
        self.cos_var.set("COS: —")
        self._log("[desconectado]")

    def _on_close(self):
        self._disconnect()
        self.destroy()

    # --- unico punto donde se toca la interfaz: hilo principal --------------
    def _pump(self):
        """Vacia, ya en el hilo principal, lo que hayan dejado los hilos de
        trabajo y lo que haya llegado por el puerto. Corre siempre, tambien
        estando desconectado, para que los errores se vean."""
        # 1) resultados y mensajes de los hilos de trabajo
        try:
            while True:
                item = self._ui_queue.get_nowait()
                kind = item[0]
                if kind == "log":
                    self._log(item[1])
                elif kind == "done":
                    _, cb, result, error = item
                    try:
                        cb(result, error)
                    except Exception as exc:
                        self._log(f"[error interno: {type(exc).__name__}: {exc}]")
        except queue.Empty:
            pass

        # 2) lineas recibidas del dispositivo
        if self.link:
            try:
                while True:
                    line = self.link.incoming.get_nowait()
                    # Las respuestas al sondeo automático de COS se omiten: se
                    # ven en el indicador, y si no el registro se llena de
                    # 'COS=0' y no se puede leer el tráfico real.
                    if line in ("COS=0", "COS=1"):
                        continue
                    self._log(f"<< {line}")
            except queue.Empty:
                pass

        self.after(120, self._pump)

    def _poll_cos(self):
        if not self.link or not self.link.is_connected:
            return
        self._run_bg(self.link.get_cos, self._on_cos_result, silent=True)
        self._cos_job = self.after(2000, self._poll_cos)

    def _on_cos_result(self, value, error):
        if error:
            return
        self.cos_var.set(f"COS: {'SÍ' if value else 'no'}")
        self.cos_label.configure(foreground=("#c0392b" if value else "#7f8c8d"))
        # solo se registra el cambio de estado, no cada sondeo
        if value != self._last_cos:
            if self._last_cos is not None:
                self._log(f"[COS -> {'portadora detectada' if value else 'sin portadora'}]")
            self._last_cos = value

    # --- para no bloquear la interfaz mientras se espera respuesta ----------
    def _run_bg(self, fn, on_done=None, silent: bool = False):
        """Ejecuta fn() en un hilo. El hilo no toca la interfaz: encola lo
        que haya que mostrar o llamar, y _pump() lo saca en el hilo principal."""

        def worker():
            try:
                result = fn()
                error = None
            except Exception as exc:
                result = None
                error = exc
                if not silent:
                    self._ui_queue.put(("log", f"[error: {type(exc).__name__}: {exc}]"))
            if on_done:
                self._ui_queue.put(("done", on_done, result, error))

        threading.Thread(target=worker, daemon=True).start()

    # --- acciones de los botones ---------------------------------------------
    def _do_ping(self):
        if not self._check_connected():
            return
        self._log(">> PING")
        self._run_bg(self.link.ping, lambda r, e: self._log(f"[ping {'OK' if r else 'fallo'}]") if e is None else None)

    def _do_info(self):
        if not self._check_connected():
            return
        self._log(">> INFO")

        def on_done(info, error):
            if error or info is None:
                return
            self._log(
                f"[fw={info.firmware} cat_baud={info.cat_baud} ptt={info.ptt} "
                f"cos={info.cos} cores(cat={info.cat_core} host={info.host_core} "
                f"main={info.main_core}) links={info.links}]"
            )

        self._run_bg(self.link.info, on_done)

    def _do_set_baud(self):
        if not self._check_connected():
            return
        try:
            baud = int(self.baud_var.get())
        except ValueError:
            messagebox.showwarning("Baudios", "Introduce un número válido.")
            return
        self._log(f">> BAUD={baud}")
        self._run_bg(lambda: self.link.set_baud(baud), lambda r, e: None)

    def _do_cat(self):
        if not self._check_connected():
            return
        text = self.cat_var.get().strip()
        if not text:
            return
        self._log(f">> {text}")
        self._run_bg(lambda: self.link.cat(text), lambda r, e: None)
        self.cat_var.set("")

    def _ptt(self, on: bool):
        if not self.link or not self.link.is_connected:
            return
        self._log(f">> PTT={'1' if on else '0'}")
        self._run_bg(lambda: self.link.set_ptt(on), lambda r, e: None)

    def _check_connected(self) -> bool:
        if not self.link or not self.link.is_connected:
            messagebox.showwarning("Sin conexión", "Conecta primero con el dispositivo.")
            return False
        return True


if __name__ == "__main__":
    if not pyserial_available():
        # tkinter ya sabemos que está disponible (si no, ni llegamos aquí),
        # así que se puede mostrar el aviso en una ventana en vez de solo
        # por consola, útil si se lanza con doble clic y no hay terminal.
        root = tk.Tk()
        root.withdraw()
        messagebox.showerror("Falta pyserial", missing_pyserial_message())
        root.destroy()
        sys.exit(1)
    RadioApp().mainloop()
