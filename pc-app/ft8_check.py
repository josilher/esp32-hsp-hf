"""
ft8_check.py - Comprueba si una señal FT8 sobrevive al canal de audio del
ESP32 Radio HF (micrófono -> WM8731 -> mSBC -> Bluetooth -> PC).

Hace dos cosas:

  1. Busca la ráfaga FT8 dentro de una grabación cualquiera, correlando con
     los tres arrays de Costas que FT8 lleva como sincronismo. Eso da una
     medida CUANTITATIVA de si la estructura de tonos ha sobrevivido, con
     independencia de que luego el mensaje se decodifique o no.

  2. Recorta, remuestrea y escribe un WAV de 15 s a 12000 Hz con la ráfaga
     colocada donde WSJT-X la espera, listo para pasárselo a jt9.

Uso:
    python ft8_check.py grabacion.ogg [-o salida.wav] [--ref referencia.wav]

Con --ref analiza también el fichero de referencia y compara, que es lo que
de verdad responde a la pregunta: cuanto se ha degradado al pasar por el
dispositivo.

Parametros de FT8 (estandar):
    79 simbolos, 8-FSK, 6.25 baudios (0.16 s/simbolo), tonos cada 6.25 Hz.
    Duracion total 12.64 s. Sincronismo de Costas en los simbolos 0-6,
    36-42 y 72-78, con la secuencia 3,1,4,0,6,5,2.
"""
from __future__ import annotations

import argparse
import sys

import numpy as np

try:
    import soundfile as sf
    from scipy import signal as sps
except ImportError:
    sys.exit("Faltan dependencias: pip install soundfile scipy numpy")

# --- constantes de FT8 ------------------------------------------------------
COSTAS = np.array([3, 1, 4, 0, 6, 5, 2])
SYMBOL_RATE = 6.25                 # baudios
TONE_SPACING = 6.25                # Hz
N_SYMBOLS = 79
SYMBOL_SECONDS = 1.0 / SYMBOL_RATE
BURST_SECONDS = N_SYMBOLS * SYMBOL_SECONDS      # 12.64 s
SYNC_POSITIONS = (0, 36, 72)       # donde empieza cada array de Costas

# WSJT-X trabaja con ficheros de 15 s a 12 kHz y la emision arranca a 0.5 s
JT9_RATE = 12000
JT9_SECONDS = 15.0
JT9_START = 0.5


def load_mono(path: str) -> tuple[np.ndarray, int]:
    x, fs = sf.read(path, always_2d=True)
    return x.mean(axis=1).astype(np.float64), int(fs)


def spectrogram(x: np.ndarray, fs: int) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Espectrograma con la resolucion natural de FT8: una ventana por
    simbolo y un bin por tono."""
    nper = int(round(fs * SYMBOL_SECONDS))        # muestras por simbolo
    f, t, Z = sps.stft(x, fs, window="hann", nperseg=nper,
                       noverlap=nper // 2, boundary=None, padded=False)
    return f, t, np.abs(Z)


def costas_score(S: np.ndarray, f: np.ndarray, t: np.ndarray,
                 f0_idx: int, t0_idx: int, bins_per_tone: float,
                 frames_per_symbol: float) -> float:
    """Potencia en los tonos que marca el sincronismo, menos la potencia
    media de los otros siete tonos en esos mismos instantes. En dB."""
    on, off = [], []
    for base in SYNC_POSITIONS:
        for k, tone in enumerate(COSTAS):
            ti = int(round(t0_idx + (base + k) * frames_per_symbol))
            if ti < 0 or ti >= S.shape[1]:
                return -np.inf
            for cand in range(8):
                fi = int(round(f0_idx + cand * bins_per_tone))
                if fi < 0 or fi >= S.shape[0]:
                    return -np.inf
                (on if cand == tone else off).append(S[fi, ti])
    on_p = np.mean(np.square(on))
    off_p = np.mean(np.square(off)) + 1e-30
    return float(10.0 * np.log10(on_p / off_p))


def find_burst(x: np.ndarray, fs: int, fmin: float = 200.0,
               fmax: float = 3000.0) -> dict:
    """Busca el desplazamiento en tiempo y la frecuencia base de la rafaga."""
    f, t, S = spectrogram(x, fs)
    df = f[1] - f[0]
    dt = t[1] - t[0]
    bins_per_tone = TONE_SPACING / df
    frames_per_symbol = SYMBOL_SECONDS / dt

    f_lo = int(np.searchsorted(f, fmin))
    f_hi = int(np.searchsorted(f, min(fmax, f[-1] - 8 * TONE_SPACING)))
    max_t0 = S.shape[1] - int(N_SYMBOLS * frames_per_symbol) - 1
    if max_t0 <= 0:
        raise SystemExit("La grabacion es mas corta que una rafaga de FT8 (12,64 s).")

    best = (-np.inf, 0, 0)
    # barrido grueso y luego fino alrededor del mejor candidato
    for t0 in range(0, max_t0, max(1, int(frames_per_symbol // 2))):
        for f0 in range(f_lo, f_hi):
            s = costas_score(S, f, t, f0, t0, bins_per_tone, frames_per_symbol)
            if s > best[0]:
                best = (s, f0, t0)
    _, bf, bt = best
    for t0 in range(max(0, bt - int(frames_per_symbol)),
                    min(max_t0, bt + int(frames_per_symbol) + 1)):
        for f0 in range(max(f_lo, bf - 3), min(f_hi, bf + 4)):
            s = costas_score(S, f, t, f0, t0, bins_per_tone, frames_per_symbol)
            if s > best[0]:
                best = (s, f0, t0)

    score, f0_idx, t0_idx = best
    return {
        "sync_db": score,
        "freq_hz": float(f[f0_idx]),
        "start_s": float(t0_idx * dt),
        "f": f, "t": t, "S": S,
        "bins_per_tone": bins_per_tone,
        "frames_per_symbol": frames_per_symbol,
        "f0_idx": f0_idx, "t0_idx": t0_idx,
    }


def tone_purity(info: dict) -> tuple[float, float]:
    """De los 21 simbolos de sincronismo, en cuantos el tono correcto es
    realmente el mas fuerte de los ocho. Y cuanto destaca, en dB."""
    S, bpt, fps = info["S"], info["bins_per_tone"], info["frames_per_symbol"]
    f0, t0 = info["f0_idx"], info["t0_idx"]
    hits, margins = 0, []
    total = 0
    for base in SYNC_POSITIONS:
        for k, tone in enumerate(COSTAS):
            ti = int(round(t0 + (base + k) * fps))
            if ti >= S.shape[1]:
                continue
            vals = []
            for cand in range(8):
                fi = int(round(f0 + cand * bpt))
                vals.append(S[fi, ti] if fi < S.shape[0] else 0.0)
            vals = np.array(vals)
            total += 1
            if int(np.argmax(vals)) == tone:
                hits += 1
            best_other = np.max(np.delete(vals, tone))
            margins.append(20 * np.log10((vals[tone] + 1e-30) / (best_other + 1e-30)))
    return (100.0 * hits / max(1, total)), float(np.mean(margins))


def write_jt9_wav(x: np.ndarray, fs: int, start_s: float, out_path: str) -> None:
    """Recorta desde el inicio de la rafaga, remuestrea a 12 kHz y deja la
    rafaga arrancando en 0.5 s dentro de un fichero de 15 s."""
    begin = int(round(start_s * fs))
    need = int(round((JT9_SECONDS - JT9_START) * fs))
    seg = x[begin:begin + need]
    if len(seg) < need:
        seg = np.pad(seg, (0, need - len(seg)))
    y = sps.resample_poly(seg, JT9_RATE, fs)
    out = np.zeros(int(JT9_SECONDS * JT9_RATE), dtype=np.float64)
    off = int(JT9_START * JT9_RATE)
    out[off:off + len(y)] = y[:len(out) - off]
    peak = np.max(np.abs(out))
    if peak > 0:
        out = out / peak * 0.7
    sf.write(out_path, (out * 32767).astype(np.int16), JT9_RATE, subtype="PCM_16")


def report(path: str, out: str | None) -> dict:
    x, fs = load_mono(path)
    print(f"\n=== {path} ===")
    print(f"  {len(x)/fs:.2f} s a {fs} Hz   "
          f"RMS {20*np.log10(np.sqrt(np.mean(x**2))+1e-12):.1f} dBFS")
    info = find_burst(x, fs)
    pct, margin = tone_purity(info)
    print(f"  rafaga detectada en t = {info['start_s']:.2f} s, "
          f"tono base {info['freq_hz']:.1f} Hz")
    print(f"  sincronismo de Costas : {info['sync_db']:+.1f} dB")
    print(f"  simbolos de sync con el tono correcto mas fuerte: {pct:.0f} % de 21")
    print(f"  margen medio del tono correcto: {margin:+.1f} dB")
    # Umbrales calibrados contra ruido blanco puro: como el detector barre
    # tiempo y frecuencia buscando el mejor encaje, el ruido da un suelo de
    # unos +3,3 dB de sincronismo y un 33 % de aciertos (el azar puro seria
    # 12,5 %). Cualquier criterio tiene que quedar por encima de eso.
    if info["sync_db"] < 5.0 or pct < 50.0:
        print("  >> NO se aprecia estructura de FT8 (indistinguible de ruido)")
    elif info["sync_db"] < 8.0 or pct < 80.0:
        print("  >> estructura presente pero degradada")
    else:
        print("  >> estructura de FT8 intacta")
    if out:
        write_jt9_wav(x, fs, info["start_s"], out)
        print(f"  WAV para jt9 escrito en: {out}")
        print(f"     decodifica con:  jt9 -8 -d 3 \"{out}\"")
    return {"sync_db": info["sync_db"], "pct": pct, "margin": margin}


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("grabacion", help="audio capturado del enlace Bluetooth")
    ap.add_argument("-o", "--out", default=None, help="WAV de 12 kHz para jt9")
    ap.add_argument("--ref", default=None, help="fichero FT8 de referencia, para comparar")
    args = ap.parse_args()

    got = report(args.grabacion, args.out)
    if args.ref:
        ref = report(args.ref, None)
        print("\n=== degradacion introducida por el dispositivo ===")
        print(f"  sincronismo : {ref['sync_db']:+.1f} dB -> {got['sync_db']:+.1f} dB "
              f"({got['sync_db']-ref['sync_db']:+.1f} dB)")
        print(f"  aciertos    : {ref['pct']:.0f} % -> {got['pct']:.0f} %")
        print(f"  margen tonal: {ref['margin']:+.1f} dB -> {got['margin']:+.1f} dB "
              f"({got['margin']-ref['margin']:+.1f} dB)")


if __name__ == "__main__":
    main()
