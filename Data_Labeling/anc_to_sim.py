"""
anc_to_sim.py — Convierte un archivo .anc del SGC al formato .bin del firmware
para simular un sismo real en el ESP32.

Formato de salida (.bin):
  Header (8 bytes): magic(uint32_le=0xDA7A1345) + version(uint8=1)
                    + sample_rate(uint8=200) + reserved(uint16=0)
  Per muestra (10 bytes): timestamp_ms(uint32_le) + ax(int16_le)
                          + ay(int16_le) + az(int16_le)

Secuencia generada:
  1. 30 s de ruido sintético (sigma=0.55 LSB) → LTA del firmware converge
  2. 60 s del evento sísmico centrados en el pico PGA

Uso:
  cd Data_Labeling
  python anc_to_sim.py [archivo.anc] [salida.bin]

Defaults:
  archivo.anc → DatosObtenidos/Acelerografo_SGC/2023_mar/SGC2023keosra_CAP2_10.anc
                (M=6.6 Mar Caribe 2023-05-25, PGA=131 mg — mismo evento del training)
  salida.bin  → sim_sismo.bin  (copiar a raíz de la SD card del ESP32)

NOTA: Usar un evento con PGA > 3.9 mg (1 LSB ADXL345) SIN escalar.
      El modelo fue entrenado con estos mismos archivos .anc via STA/LTA automático.
      No usar SCALE_FACTOR — el escalado artificially cambia el patrón espectral.
"""

import sys
import struct
import pathlib
import numpy as np

# ── Parámetros ────────────────────────────────────────────────────────────────
ANC_FILE    = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 \
              else pathlib.Path("DatosObtenidos/Acelerografo_SGC/2023_mar/SGC2023keosra_CAP2_10.anc")
OUT_FILE    = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 \
              else pathlib.Path("sim_sismo.bin")

FS          = 200           # Hz — igual que el firmware
LSB_G       = 0.0039        # 3.9 mg/LSB (ADXL345 ±2g full-res)
HEADER_ROWS = 20            # líneas de encabezado en el .anc
PRE_NOISE_S = 20            # s de ruido sintético antes del evento (warmup LTA)
EVENT_WIN_S = 150            # s de evento centrados en pico PGA
RNG         = np.random.default_rng(42)

# Sin escalado: el evento M=6.6 CAP2 tiene PGA=131 mg (33 LSB) — visible al ADXL345
# y fue procesado sin escala en el training. SCALE_FACTOR=1.0 preserva el patron espectral.
SCALE_FACTOR = 1.0

# ── Cargar .anc ───────────────────────────────────────────────────────────────
print(f"Cargando {ANC_FILE} ...")
data = np.loadtxt(ANC_FILE, skiprows=HEADER_ROWS)   # (N, 3) cm/s²  EW/VER/NS
print(f"  {len(data)} muestras @ {FS} Hz  ({len(data)/FS:.0f} s)")
print(f"  PGA máximo: {np.max(np.abs(data)):.4f} cm/s²")

# ── Localizar pico PGA ────────────────────────────────────────────────────────
pga_per_sample = np.max(np.abs(data), axis=1)
peak_idx       = int(np.argmax(pga_per_sample))
peak_pga_g     = pga_per_sample[peak_idx] / 980.665
print(f"  Pico PGA en muestra {peak_idx} (t={peak_idx/FS:.1f} s)  "
      f"→ {peak_pga_g*1000:.2f} mg = {pga_per_sample[peak_idx]:.4f} cm/s²")

# ── Extraer ventana de evento ─────────────────────────────────────────────────
half_win  = EVENT_WIN_S // 2 * FS
start_idx = max(0, peak_idx - half_win)
end_idx   = min(len(data), peak_idx + half_win)
event_raw = data[start_idx:end_idx]                  # (N_event, 3) cm/s²
event_g   = (event_raw / 980.665 * SCALE_FACTOR).astype(np.float32)  # cm/s² → g, escalado
print(f"  Ventana de evento: [{start_idx/FS:.1f} s, {end_idx/FS:.1f} s]"
      f"  → {len(event_g)/FS:.0f} s extraídos")

# ── Ruido sintético pre-evento (permite que el LTA del firmware converja) ─────
noise_n  = PRE_NOISE_S * FS
noise_g  = RNG.normal(0.0, 0.55 * LSB_G, (noise_n, 3)).astype(np.float32)
print(f"  Ruido pre-evento: {PRE_NOISE_S} s ({noise_n} muestras)")

# ── Concatenar y cuantizar a int16 (escala ADXL345) ──────────────────────────
all_g   = np.vstack([noise_g, event_g])
all_i16 = np.clip(np.round(all_g / LSB_G), -32768, 32767).astype(np.int16)
total_s = len(all_i16) / FS
print(f"  Total: {len(all_i16)} muestras ({total_s:.0f} s)")
print(f"  PGA cuantizado (máx):  "
      f"{np.max(np.abs(all_i16[noise_n:])) * LSB_G * 1000:.2f} mg")

# ── Escribir .bin ─────────────────────────────────────────────────────────────
MAGIC = 0xDA7A1345
with open(OUT_FILE, "wb") as f:
    # Header: magic(4) + version(1=0x01) + sample_rate(1=200) + reserved(2=0)
    f.write(struct.pack("<IBBH", MAGIC, 1, FS, 0))
    for i, row in enumerate(all_i16):
        ts_ms = int(i * 1000 / FS)
        # .anc cols: EW(0) VER(1) NS(2)
        # firmware: _spec_axis(ax,ch0=EW)  _spec_axis(az,ch1=VER)  _spec_axis(ay,ch2=NS)
        # → ax=EW  ay=NS  az=VER
        f.write(struct.pack("<Ihhh", ts_ms, int(row[0]), int(row[2]), int(row[1])))

size_kb = OUT_FILE.stat().st_size / 1024
print(f"\nGenerado: {OUT_FILE}  ({size_kb:.0f} KB, {total_s:.0f} s)")
print()
print("Pasos siguientes:")
print(f"  1. Copiar {OUT_FILE.name} a la raíz de la SD card del ESP32")
print("  2. Descomentar '#define SIM_REPLAY_FILE' en config.h")
print("  3. Compilar y flashear: pio run && pio run --target upload")
print("  4. Monitor serial: debe aparecer [STA/LTA] TRIGGER → [CNN] SISMO → Firebase")
